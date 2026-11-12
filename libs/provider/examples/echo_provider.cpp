// Minimal InferDeck provider. Copy this file to start a new provider.
//
// It registers the runtime "echo" with three operations:
//   chat    echoes the last user message back, streaming one word at a time
//   image   returns the bytes of the artifact named "image" as the PNG result
//   decide  gives every option of every question the same probability
//
// Build it as a shared library, drop it into the gateway's providers/
// directory, and select it per model with `runtime: echo`.
// The contract it implements is docs/providers.md.
#define INFERDECK_PROVIDER_BUILD
#include "inferdeck/provider_abi.h"

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{

using json = nlohmann::json;

/// <summary>State for one configured model.</summary>
struct EchoModel
{
    json config;
    bool loaded{false};
};

/// <summary>Reports an error message to the host and returns the status to hand back.</summary>
int32_t fail(const inferdeck_call* call, inferdeck_status status, const std::string& message)
{
    call->reply(call->context, json{{"message", message}}.dump().c_str(), nullptr, 0);
    return status;
}

int32_t echo_create(const char* model_json, const inferdeck_call* call, void** instance)
{
    json config = json::parse(model_json, nullptr, false);
    if (!config.is_object())
    {
        return fail(call, INFERDECK_PARSE_ERROR, "model description is not JSON");
    }
    *instance = new EchoModel{std::move(config)};
    return INFERDECK_OK;
}

void echo_destroy(void* instance)
{
    delete static_cast<EchoModel*>(instance);
}

int32_t echo_load(void* instance, const inferdeck_call* call)
{
    EchoModel* model = static_cast<EchoModel*>(instance);
    if (model->config["artifacts"].value("fail_load", "") == "true")
    {
        return fail(call, INFERDECK_UNAVAILABLE, "echo provider was asked to fail its load");
    }
    model->loaded = true;
    return INFERDECK_OK;
}

int32_t echo_unload(void* instance, const inferdeck_call*)
{
    static_cast<EchoModel*>(instance)->loaded = false;
    return INFERDECK_OK;
}

int32_t echo_vram_usage_mb(void*)
{
    return -1;
}

int32_t echo_chat(const json& request, const inferdeck_call* call)
{
    std::string last_user_text;
    for (const json& message : request["messages"])
    {
        if (message["role"] != "user")
        {
            continue;
        }
        last_user_text.clear();
        for (const json& part : message["content"])
        {
            if (part["type"] == "text")
            {
                last_user_text += part["text"].get<std::string>();
            }
        }
    }
    std::istringstream words(last_user_text);
    std::string word;
    std::string text;
    int tokens = 0;
    while (words >> word)
    {
        const std::string piece = (tokens == 0 ? "" : " ") + word;
        text += piece;
        ++tokens;
        const std::string event = json{{"type", "delta"}, {"content", piece}}.dump();
        if (request.value("stream", false) && call->emit(call->context, event.c_str(), nullptr, 0))
        {
            return fail(call, INFERDECK_CANCELLED, "chat cancelled");
        }
        if (call->cancelled(call->context))
        {
            return fail(call, INFERDECK_CANCELLED, "chat cancelled");
        }
    }
    const json result = {
        {"text", text},
        {"finish_reason", "stop"},
        {"prompt_tokens", tokens},
        {"completion_tokens", tokens},
    };
    call->reply(call->context, result.dump().c_str(), nullptr, 0);
    return INFERDECK_OK;
}

int32_t echo_image(const EchoModel& model, const inferdeck_call* call)
{
    std::ifstream file(model.config["artifacts"].value("image", ""), std::ios::binary);
    if (!file)
    {
        return fail(call, INFERDECK_IO_ERROR, "artifact \"image\" could not be read");
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    call->emit(call->context, R"({"type":"progress","percent":100})", nullptr, 0);
    const inferdeck_blob png{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    call->reply(call->context, "{}", &png, 1);
    return INFERDECK_OK;
}

int32_t echo_decide(const json& request, const inferdeck_call* call)
{
    json answers = json::array();
    for (const json& question : request["questions"])
    {
        const size_t options = question["type"] == "predicate" ? 2 : question["options"].size();
        answers.push_back({{"probabilities", std::vector<double>(options, 1.0 / static_cast<double>(options))}});
    }
    call->reply(call->context, json{{"answers", answers}, {"input_tokens", 0}}.dump().c_str(), nullptr, 0);
    return INFERDECK_OK;
}

int32_t echo_invoke(void* instance, const char* operation, const char* request_json,
                    const inferdeck_blob*, size_t, const inferdeck_call* call)
{
    try
    {
        const EchoModel* model = static_cast<const EchoModel*>(instance);
        if (!model->loaded)
        {
            return fail(call, INFERDECK_NOT_LOADED, "echo model is not loaded");
        }
        const json request = json::parse(request_json);
        const std::string name = operation;
        if (name == "chat")
        {
            return echo_chat(request, call);
        }
        if (name == "image")
        {
            return echo_image(*model, call);
        }
        if (name == "decide")
        {
            return echo_decide(request, call);
        }
        return fail(call, INFERDECK_UNAVAILABLE, "echo provider does not implement " + name);
    }
    catch (const std::exception& error)
    {
        return fail(call, INFERDECK_INTERNAL, error.what());
    }
}

const inferdeck_provider echo_provider = {
    INFERDECK_PROVIDER_ABI_VERSION,
    R"({
        "runtime": "echo",
        "version": "1.0.0",
        "modalities": {
            "text": ["chat_completions", "responses"],
            "image": ["image_generation"],
            "decision": ["decisions"]
        },
        "model_source": "none"
    })",
    echo_create,
    echo_destroy,
    echo_load,
    echo_unload,
    echo_vram_usage_mb,
    echo_invoke,
};

}

extern "C" const inferdeck_provider* inferdeck_provider_v1(void)
{
    return &echo_provider;
}
