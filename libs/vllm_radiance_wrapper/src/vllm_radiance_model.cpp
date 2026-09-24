#include "vllm_radiance_wrapper/vllm_radiance_model.hpp"

#include <Python.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <thread>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <map>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace inferdeck::vllm_radiance_wrapper {
namespace {

foundation::Result<void> fail(const std::string& message) {
    return foundation::Err<void>(foundation::ErrorCode::Unavailable, message);
}

std::string python_error() {
    if (!PyErr_Occurred()) return "Python call failed";
    PyObject *type = nullptr, *value = nullptr, *traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback); PyErr_NormalizeException(&type, &value, &traceback);
    PyObject* text = value ? PyObject_Str(value) : nullptr;
    const char* utf8 = text ? PyUnicode_AsUTF8(text) : nullptr;
    std::string result = utf8 ? utf8 : "unprintable Python exception";
    if (type && value) PyErr_Display(type, value, traceback);
    Py_XDECREF(text); Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(traceback);
    return result;
}

class GIL final {
public:
    GIL() : state_(PyGILState_Ensure()) {}
    ~GIL() { PyGILState_Release(state_); }
private: PyGILState_STATE state_;
};

class GILRelease final {
public:
    GILRelease() : state_(PyEval_SaveThread()) {}
    ~GILRelease() { PyEval_RestoreThread(state_); }
private: PyThreadState* state_;
};

using PyPtr = std::unique_ptr<PyObject, decltype(&Py_DecRef)>;
PyPtr owned(PyObject* value) { return PyPtr(value, &Py_DecRef); }
void set_owned(PyObject* dictionary, const char* key, PyPtr value) {
    if (!value || PyDict_SetItemString(dictionary, key, value.get()) != 0) throw std::runtime_error(python_error());
}
PyPtr json_value(const std::string& text) {
    PyPtr module = owned(PyImport_ImportModule("json"));
    if (!module) throw std::runtime_error(python_error());
    PyPtr value = owned(PyObject_CallMethod(module.get(), "loads", "s", text.c_str()));
    if (!value) throw std::runtime_error("invalid JSON value: " + python_error());
    return value;
}
std::optional<std::string> validate_sampling(const model::SamplingConfig& defaults) {
    if (defaults.dry_multiplier != 0.0f)
        return "vllm_radiance does not support nonzero dry_multiplier";
    return std::nullopt;
}
PyObject* text_dict(const model::InferenceRequest& request,
                    const model::SamplingConfig& defaults) {
    PyPtr result = owned(PyDict_New()); PyPtr messages = owned(PyList_New(0));
    for (const auto& message : request.messages) {
        PyPtr item = owned(PyDict_New()); const char* role = "user";
        switch (message.role) { case inference::MessageRole::Developer: role="developer"; break; case inference::MessageRole::System: role="system"; break; case inference::MessageRole::Assistant: role="assistant"; break; case inference::MessageRole::Tool: role="tool"; break; case inference::MessageRole::Function: role="function"; break; default: break; }
        set_owned(item.get(),"role",owned(PyUnicode_FromString(role))); std::string content;
        for(const auto& part:message.content) { const auto* text=std::get_if<inference::TextContent>(&part); if(!text){PyErr_SetString(PyExc_NotImplementedError,"vllm_radiance supports text content only");return nullptr;} content+=text->text; }
        set_owned(item.get(),"content",owned(PyUnicode_FromString(content.c_str())));
        if(!message.reasoning.empty()) set_owned(item.get(),"reasoning_content",owned(PyUnicode_FromString(message.reasoning.c_str())));
        if(!message.tool_call_id.empty()) set_owned(item.get(),"tool_call_id",owned(PyUnicode_FromString(message.tool_call_id.c_str())));
        if(!message.tool_calls.empty()) { PyPtr calls=owned(PyList_New(0)); for(const auto& call:message.tool_calls){ PyPtr function=owned(PyDict_New());set_owned(function.get(),"name",owned(PyUnicode_FromString(call.name.c_str())));set_owned(function.get(),"arguments",owned(PyUnicode_FromString(call.arguments.c_str())));PyPtr tool=owned(PyDict_New());set_owned(tool.get(),"id",owned(PyUnicode_FromString(call.id.c_str())));set_owned(tool.get(),"type",owned(PyUnicode_FromString("function")));set_owned(tool.get(),"function",std::move(function));if(PyList_Append(calls.get(),tool.get())!=0)return nullptr;}set_owned(item.get(),"tool_calls",std::move(calls)); }
        if(PyList_Append(messages.get(),item.get())!=0)return nullptr;
    }
    set_owned(result.get(),"messages",std::move(messages)); PyPtr tools=owned(PyList_New(0));
    for(const auto& tool:request.tools){PyPtr function=owned(PyDict_New());set_owned(function.get(),"name",owned(PyUnicode_FromString(tool.name.c_str())));set_owned(function.get(),"description",owned(PyUnicode_FromString(tool.description.c_str())));set_owned(function.get(),"parameters",json_value(tool.parameters_schema));PyPtr item=owned(PyDict_New());set_owned(item.get(),"type",owned(PyUnicode_FromString("function")));set_owned(item.get(),"function",std::move(function));if(PyList_Append(tools.get(),item.get())!=0)return nullptr;}
    set_owned(result.get(),"tools",std::move(tools)); PyPtr sampling=owned(PyDict_New());
    const float temperature = request.sampling.temperature.value_or(defaults.temperature);
    const float top_p = request.sampling.top_p.value_or(defaults.top_p);
    const int top_k = request.sampling.top_k.value_or(defaults.top_k) <= 0 ? -1 : request.sampling.top_k.value_or(defaults.top_k);
    const float min_p = request.sampling.min_p.value_or(defaults.min_p);
    const float repeat_penalty = request.sampling.repeat_penalty.value_or(defaults.repeat_penalty);
    const int repeat_last_n = request.sampling.repeat_last_n.value_or(defaults.repeat_last_n);
    set_owned(sampling.get(),"temperature",owned(PyFloat_FromDouble(temperature)));
    set_owned(sampling.get(),"top_p",owned(PyFloat_FromDouble(top_p)));
    set_owned(sampling.get(),"top_k",owned(PyLong_FromLong(top_k)));
    set_owned(sampling.get(),"min_p",owned(PyFloat_FromDouble(min_p)));
    if(request.sampling.seed>=0)set_owned(sampling.get(),"seed",owned(PyLong_FromLongLong(request.sampling.seed)));set_owned(result.get(),"sampling",std::move(sampling));
    set_owned(result.get(), "repeat_last_n", owned(PyLong_FromLong(repeat_last_n)));
    set_owned(PyDict_GetItemString(result.get(), "sampling"), "repetition_penalty", owned(PyFloat_FromDouble(repeat_penalty)));
    if (request.sampling.frequency_penalty) set_owned(PyDict_GetItemString(result.get(), "sampling"), "frequency_penalty", owned(PyFloat_FromDouble(*request.sampling.frequency_penalty)));
    if (request.sampling.presence_penalty) set_owned(PyDict_GetItemString(result.get(), "sampling"), "presence_penalty", owned(PyFloat_FromDouble(*request.sampling.presence_penalty)));
    if (request.sampling.mirostat.value_or(0) != 0 || request.sampling.tfs_z.value_or(1.0f) != 1.0f)
        throw std::runtime_error("vllm_radiance does not support mirostat or tail-free sampling");
    if (!request.sampling.logit_bias.empty())
    {
        PyPtr bias = owned(PyDict_New());
        for (const auto& [token, value] : request.sampling.logit_bias)
        {
            PyPtr key = owned(PyLong_FromLong(token));
            PyPtr amount = owned(PyFloat_FromDouble(value));
            if (!key || !amount || PyDict_SetItem(bias.get(), key.get(), amount.get()) != 0)
                throw std::runtime_error(python_error());
        }
        set_owned(PyDict_GetItemString(result.get(), "sampling"), "logit_bias", std::move(bias));
    }
    PyPtr stop=owned(PyList_New(0));for(const auto& value:request.stop){PyPtr text=owned(PyUnicode_FromString(value.c_str()));if(PyList_Append(stop.get(),text.get())!=0)return nullptr;}set_owned(result.get(),"stop",std::move(stop));set_owned(result.get(),"max_output_tokens",owned(PyLong_FromLong(request.max_output_tokens)));set_owned(result.get(),"add_generation_prompt",owned(PyBool_FromLong(request.add_generation_prompt)));if(request.reasoning_effort)set_owned(result.get(),"reasoning_effort",owned(PyUnicode_FromString(request.reasoning_effort->c_str())));if(request.enable_reasoning)set_owned(result.get(),"enable_reasoning",owned(PyBool_FromLong(*request.enable_reasoning)));
    const char* choice="auto"; if(request.tool_choice.kind==inference::ToolChoiceKind::None)choice="none";else if(request.tool_choice.kind==inference::ToolChoiceKind::Required)choice="required";set_owned(result.get(),"tool_choice",owned(PyUnicode_FromString(choice)));set_owned(result.get(),"include_reasoning",owned(PyBool_FromLong(request.enable_reasoning.value_or(true))));
    if (request.output.kind == inference::StructuredOutputKind::JsonObject || request.output.kind == inference::StructuredOutputKind::JsonSchema)
    {
        PyPtr structured = owned(PyDict_New());
        if (request.output.kind == inference::StructuredOutputKind::JsonObject)
            set_owned(structured.get(), "json_object", owned(PyBool_FromLong(1)));
        else
            set_owned(structured.get(), "json", json_value(request.output.schema));
        set_owned(result.get(), "structured_outputs", std::move(structured));
    }
    else if (request.output.kind != inference::StructuredOutputKind::Text)
        throw std::runtime_error("vllm_radiance supports json_object and json_schema structured output only");
    set_owned(result.get(),"logprobs",owned(PyBool_FromLong(request.logprobs)));return result.release();
}
} // namespace

class VllmRadianceModel::State {
public:
    explicit State(model::ModelInfo value) : info(std::move(value)) {}
    model::ModelInfo info; mutable std::mutex mutex; std::condition_variable cv;
    bool loaded{false}; bool unloading{false}; std::array<bool, 4> slots{}; int active_slots{0};
    std::atomic<bool> healthy{true}; std::atomic<bool> cancel{false};
    PyObject* module{nullptr}; PyObject* engine{nullptr};
};

VllmRadianceModel::VllmRadianceModel(model::ModelInfo info) : state_(std::make_unique<State>(std::move(info))) {}
VllmRadianceModel::~VllmRadianceModel() { (void)unload(); }
const model::ModelInfo& VllmRadianceModel::info() const { return state_->info; }

foundation::Result<void> VllmRadianceModel::load()
{
    std::lock_guard lock(state_->mutex);
    if (state_->loaded) return foundation::Ok();
    const auto attention = state_->info.artifacts.find("prefill_attention");
    const bool int4 = attention != state_->info.artifacts.end() && attention->second == "r4d_int4";
    if (state_->info.runtime != "vllm_radiance" || state_->info.compute != model::ModelCompute::RocmGpu ||
        state_->info.context_size != 106496 || state_->info.n_slots < 1 ||
        state_->info.n_slots > (int4 ? 4 : 1) || state_->info.min_slots != state_->info.n_slots)
        return fail("vllm_radiance requires ROCm compute, 106496 context, and fixed slots (one for BF16 or up to four for INT4)");
    const auto root = state_->info.artifacts.find("python_root");
    if (root == state_->info.artifacts.end()) return fail("python_root artifact is required");
    const std::filesystem::path python_root = std::filesystem::weakly_canonical(root->second);
    const std::filesystem::path executable = python_root / "python.exe";
    if (!std::filesystem::is_regular_file(executable)) return fail("python_root does not contain python.exe");
    static std::mutex interpreter_mutex;
    {
        std::lock_guard interpreter_lock(interpreter_mutex);
        if (!Py_IsInitialized())
        {
            PyConfig config;
            PyConfig_InitIsolatedConfig(&config);
            config.site_import = 1;
            PyStatus status = PyConfig_SetString(&config, &config.home, python_root.c_str());
            if (!PyStatus_Exception(status)) status = PyConfig_SetString(&config, &config.executable, executable.c_str());
            if (!PyStatus_Exception(status)) status = Py_InitializeFromConfig(&config);
            const std::string error = PyStatus_Exception(status) && status.err_msg ? status.err_msg : "";
            PyConfig_Clear(&config);
            if (PyStatus_Exception(status)) return fail("CPython initialization failed: " + error);
            PyEval_SaveThread();
        }
    }
    GIL gil;
    try
    {
        if (std::filesystem::weakly_canonical(Py_GetPrefix()) != python_root)
            return fail("A different CPython root is already active; restart is required to change dependencies");
        wchar_t executable_path[32768];
        const DWORD length = GetModuleFileNameW(nullptr, executable_path, 32768);
        if (length == 0 || length >= 32768) return fail("Cannot locate gateway executable directory");
        const std::filesystem::path modules = std::filesystem::path(executable_path).parent_path() / "python";
        if (std::filesystem::is_directory(modules))
        {
            PyPtr path = owned(PyUnicode_FromWideChar(modules.c_str(), -1));
            if (!path || PyList_Insert(PySys_GetObject("path"), 0, path.get()) != 0)
                return fail(python_error());
        }
        PyPtr module = owned(PyImport_ImportModule("inferdeck_vllm_radiance_profile"));
        if (!module) return fail(python_error());
        PyPtr config = owned(PyDict_New());
        for (const auto& [key, value] : state_->info.artifacts)
            set_owned(config.get(), key.c_str(), owned(PyUnicode_FromString(value.c_str())));
        set_owned(config.get(), "runtime", owned(PyUnicode_FromString("vllm_radiance")));
        set_owned(config.get(), "context_size", owned(PyLong_FromLong(state_->info.context_size)));
        set_owned(config.get(), "n_slots", owned(PyLong_FromLong(state_->info.n_slots)));
        set_owned(config.get(), "min_slots", owned(PyLong_FromLong(state_->info.min_slots)));
        PyPtr created = owned(PyObject_CallMethod(module.get(), "create", "O", config.get()));
        if (!created) return fail(python_error());
        state_->module = module.release();
        state_->engine = created.release();
        state_->loaded = true;
        state_->unloading = false;
        state_->slots.fill(false);
        state_->active_slots = 0;
        state_->healthy.store(true);
        state_->cancel.store(false);
        return foundation::Ok();
    }
    catch (const std::exception& error) { return fail(error.what()); }
}

foundation::Result<void> VllmRadianceModel::load(const model::LifecycleControl& control)
{
    if (control.is_cancelled() || control.is_expired())
        return foundation::Err<void>(foundation::ErrorCode::Cancelled, "backend load cancelled");
    return load();
}

foundation::Result<void> VllmRadianceModel::unload()
{
    return unload(model::LifecycleControl{});
}

foundation::Result<void> VllmRadianceModel::unload(const model::LifecycleControl& control)
{
    std::unique_lock lock(state_->mutex);
    if (!state_->loaded) return foundation::Ok();
    state_->unloading = true;
    state_->cancel.store(true);
    const auto deadline = std::min(control.deadline, model::LifecycleControl::clock::now() + std::chrono::seconds(30));
    while (state_->active_slots != 0)
    {
        if (control.is_cancelled())
        {
            state_->unloading = false;
            state_->cancel.store(false);
            return foundation::Err<void>(foundation::ErrorCode::Cancelled, "backend unload cancelled");
        }
        if (model::LifecycleControl::clock::now() >= deadline)
        {
            state_->unloading = false;
            state_->cancel.store(false);
            return fail("vllm_radiance unload timed out waiting for its active request");
        }
        state_->cv.wait_for(lock, std::chrono::milliseconds(25));
    }
    GIL gil;
    if (state_->engine)
    {
        PyPtr ignored = owned(PyObject_CallMethod(state_->module, "shutdown", "O", state_->engine));
        if (!ignored)
        {
            state_->healthy.store(false);
            return fail(python_error());
        }
        Py_CLEAR(state_->engine);
    }
    PyPtr collected = owned(PyObject_CallMethod(state_->module, "collect_released_resources", nullptr));
    if (!collected)
    {
        state_->healthy.store(false);
        return fail(python_error());
    }
    Py_CLEAR(state_->module);
    state_->loaded = false;
    state_->unloading = false;
    state_->cancel.store(false);
    return foundation::Ok();
}
bool VllmRadianceModel::is_loaded() const { std::lock_guard lock(state_->mutex); return state_->loaded; }
bool VllmRadianceModel::execution_healthy() const { return state_->healthy.load(); }
int VllmRadianceModel::vram_usage_mb() const { return is_loaded() ? state_->info.vram_required_mb : 0; }
int VllmRadianceModel::n_slots() const { return state_->info.n_slots; }
int VllmRadianceModel::n_free_slots() const { std::lock_guard lock(state_->mutex); return state_->loaded && !state_->unloading && state_->healthy.load() ? state_->info.n_slots - state_->active_slots : 0; }
foundation::Result<int> VllmRadianceModel::acquire_slot()
{
    std::lock_guard lock(state_->mutex);
    if (!state_->loaded || state_->unloading || !state_->healthy.load() || state_->cancel.load())
        return foundation::Err<int>(foundation::ErrorCode::ResourceBusy, "vllm_radiance slot unavailable");
    for (int slot_id = 0; slot_id < state_->info.n_slots; ++slot_id)
    {
        if (!state_->slots[slot_id])
        {
            state_->slots[slot_id] = true;
            ++state_->active_slots;
            return slot_id;
        }
    }
    return foundation::Err<int>(foundation::ErrorCode::ResourceBusy, "vllm_radiance slot unavailable");
}
foundation::Result<void> VllmRadianceModel::release_slot(int slot_id)
{
    {
        std::lock_guard lock(state_->mutex);
        if (slot_id < 0 || slot_id >= state_->info.n_slots || !state_->slots[slot_id])
            return foundation::Err<void>(foundation::ErrorCode::InvalidArgument, "invalid vllm_radiance slot");
        state_->slots[slot_id] = false;
        --state_->active_slots;
    }
    state_->cv.notify_all();
    return foundation::Ok();
}
bool VllmRadianceModel::slot_busy(int slot_id) const
{
    std::lock_guard lock(state_->mutex);
    return slot_id >= 0 && slot_id < state_->info.n_slots && state_->slots[slot_id];
}
void VllmRadianceModel::request_cancel() { state_->cancel.store(true); }
foundation::Result<model::InferenceResult> VllmRadianceModel::predict(int slot_id,const model::InferenceRequest& r){return predict_cancellable(slot_id,r,nullptr);}
foundation::Result<model::InferenceResult> VllmRadianceModel::predict_cancellable(int slot_id,const model::InferenceRequest& r,const std::atomic<bool>* c){ return predict_stream(slot_id,r,[](const model::InferenceDelta&){return true;},c); }
foundation::Result<model::InferenceResult> VllmRadianceModel::predict_stream(
    int slot_id, const model::InferenceRequest& request, const TokenCallback& callback,
    const std::atomic<bool>* external)
{
    {
        std::lock_guard lock(state_->mutex);
        if (slot_id < 0 || slot_id >= state_->info.n_slots || !state_->loaded ||
            !state_->slots[slot_id] || !state_->healthy.load())
            return foundation::Err<model::InferenceResult>(foundation::ErrorCode::InvalidArgument, "inactive vllm_radiance slot");
    }
    GIL gil;
    std::string request_id;
    const auto abort = [&]()
    {
        if (request_id.empty()) return;
        PyPtr ignored = owned(PyObject_CallMethod(state_->module, "abort", "Os", state_->engine, request_id.c_str()));
        request_id.clear();
        if (!ignored)
        {
            state_->healthy.store(false);
            throw std::runtime_error("request abort failed: " + python_error());
        }
    };
    try
    {
        if (const auto sampling_error = validate_sampling(state_->info.sampling))
            return foundation::Err<model::InferenceResult>(foundation::ErrorCode::InvalidArgument, *sampling_error);
        const auto cancelled = [&]() { return state_->cancel.load() || (external && external->load()); };
        if (cancelled()) return foundation::Err<model::InferenceResult>(foundation::ErrorCode::Cancelled, "request cancelled");
        const auto began = std::chrono::steady_clock::now();
        PyPtr input = owned(text_dict(request, state_->info.sampling));
        if (!input) throw std::runtime_error(python_error());
        set_owned(input.get(), "model", owned(PyUnicode_FromString(state_->info.name.c_str())));
        if (request.tool_choice.kind == inference::ToolChoiceKind::Function)
        {
            PyPtr choice = owned(PyDict_New());
            PyPtr function = owned(PyDict_New());
            set_owned(function.get(), "name", owned(PyUnicode_FromString(request.tool_choice.function_name.c_str())));
            set_owned(choice.get(), "type", owned(PyUnicode_FromString("function")));
            set_owned(choice.get(), "function", std::move(function));
            set_owned(input.get(), "tool_choice", std::move(choice));
        }
        PyPtr id = owned(PyObject_CallMethod(state_->module, "begin", "OO", state_->engine, input.get()));
        if (!id || !PyUnicode_Check(id.get())) throw std::runtime_error(python_error());
        const char* chars = PyUnicode_AsUTF8(id.get());
        if (!chars) throw std::runtime_error(python_error());
        request_id = chars;
        model::InferenceResult result;
        if (request.progress) request.progress->phase.store(2);
        std::map<std::size_t, model::ToolCall> merged_calls;
        bool observed_first_token = false;
        while (true)
        {
            if (cancelled())
            {
                abort();
                return foundation::Err<model::InferenceResult>(foundation::ErrorCode::Cancelled, "request cancelled");
            }
            PyPtr events = owned(PyObject_CallMethod(state_->module, "step", "OsO", state_->engine, request_id.c_str(), input.get()));
            if (!events || !PyList_Check(events.get())) throw std::runtime_error(python_error());
            for (Py_ssize_t i = 0; i < PyList_Size(events.get()); ++i)
            {
                PyObject* event = PyList_GetItem(events.get(), i);
                if (!PyDict_Check(event)) throw std::runtime_error("invalid bridge event");
                const auto text = [](PyObject* object, const char* key)
                {
                    PyObject* value = PyDict_GetItemString(object, key);
                    if (!value || value == Py_None) return std::string{};
                    const char* value_text = PyUnicode_AsUTF8(value);
                    if (!value_text) throw std::runtime_error(python_error());
                    return std::string(value_text);
                };
                model::InferenceDelta delta;
                delta.content = text(event, "text");
                delta.reasoning_text = text(event, "reasoning");
                const auto count = [&](const char* key)
                {
                    PyObject* value = PyDict_GetItemString(event, key);
                    const long number = value ? PyLong_AsLong(value) : 0;
                    if (PyErr_Occurred() || number < 0) throw std::runtime_error("invalid bridge token count");
                    return static_cast<int>(number);
                };
                const auto milliseconds = [&](const char* key)
                {
                    PyObject* value = PyDict_GetItemString(event, key);
                    const double duration = value ? PyFloat_AsDouble(value) : 0.0;
                    if (PyErr_Occurred() || !std::isfinite(duration) || duration < 0)
                        throw std::runtime_error("invalid bridge timing");
                    return static_cast<float>(duration);
                };
                const int prompt_tokens = count("prompt_tokens");
                const int cached_tokens = count("cached_tokens");
                const int completion_tokens = count("completion_tokens");
                const float prompt_ms = milliseconds("prompt_duration_ms");
                const float generation_ms = milliseconds("generation_duration_ms");
                if (request.progress)
                {
                    request.progress->prompt_tokens.store(prompt_tokens);
                    request.progress->processed_tokens.store(prompt_tokens);
                    request.progress->cached_tokens.store(cached_tokens);
                    request.progress->output_tokens.store(completion_tokens);
                    request.progress->prompt_ms.store(prompt_ms);
                    request.progress->generation_ms.store(generation_ms);
                    request.progress->phase.store(3);
                }
                PyObject* calls = PyDict_GetItemString(event, "tool_calls");
                if (calls && PyList_Check(calls))
                {
                    for (Py_ssize_t j = 0; j < PyList_Size(calls); ++j)
                    {
                        PyObject* item = PyList_GetItem(calls, j);
                        if (!PyDict_Check(item)) throw std::runtime_error("invalid tool delta");
                        model::ToolCallDelta tool;
                        PyObject* index = PyDict_GetItemString(item, "index");
                        tool.index = index ? PyLong_AsSize_t(index) : static_cast<std::size_t>(j);
                        if (PyErr_Occurred()) throw std::runtime_error(python_error());
                        tool.id = text(item, "id"); tool.type = text(item, "type");
                        tool.function_name = text(item, "name"); tool.function_arguments = text(item, "arguments");
                        delta.tool_calls.push_back(std::move(tool));
                    }
                }
                bool keep_streaming = true;
                if (!delta.content.empty() || !delta.reasoning_text.empty() || !delta.tool_calls.empty())
                {
                    GILRelease release;
                    keep_streaming = callback(delta);
                }
                if (!keep_streaming)
                {
                    abort();
                    return foundation::Err<model::InferenceResult>(foundation::ErrorCode::Cancelled, "stream disconnected");
                }
                if (!observed_first_token && (!delta.content.empty() || !delta.reasoning_text.empty() || !delta.tool_calls.empty()))
                {
                    observed_first_token = true;
                    result.first_token_duration_ms = static_cast<float>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
                }
                result.text += delta.content;
                result.reasoning_text += delta.reasoning_text;
                for (const auto& tool : delta.tool_calls)
                {
                    model::ToolCall& call = merged_calls[tool.index];
                    if (!tool.id.empty()) call.id = tool.id;
                    if (!tool.type.empty()) call.type = tool.type;
                    call.function_name += tool.function_name;
                    call.function_arguments += tool.function_arguments;
                }
                PyObject* finished = PyDict_GetItemString(event, "finished");
                if (finished && PyObject_IsTrue(finished) == 1)
                {
                    result.prompt_tokens = prompt_tokens;
                    result.cached_prompt_tokens = cached_tokens;
                    result.completion_tokens = completion_tokens;
                    result.finish_reason = text(event, "finish_reason");
                    result.prompt_duration_ms = prompt_ms;
                    result.generation_duration_ms = generation_ms;
                    for (auto& [index, call] : merged_calls) result.tool_calls.push_back(std::move(call));
                    result.duration_ms = static_cast<float>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
                    request_id.clear();
                    return result;
                }
            }
            {
                GILRelease release;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    catch (const std::exception& error)
    {
        std::string message = error.what();
        PyErr_Clear();
        try { abort(); } catch (const std::exception& cleanup) { message += "; " + std::string(cleanup.what()); }
        return foundation::Err<model::InferenceResult>(foundation::ErrorCode::Unavailable, message);
    }
}
} // namespace inferdeck::vllm_radiance_wrapper
