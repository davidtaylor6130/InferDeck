#include <Python.h>
#include <cassert>
#include <atomic>
#include <iostream>
#include <thread>
#include <string>
#include "vllm_radiance_wrapper/vllm_radiance_model.hpp"
int main() {
    Py_Initialize();
    PyRun_SimpleString("import sys; sys.path.insert(0, r'libs/vllm_radiance_wrapper/tests/stub_python')");
    PyThreadState* mainState=PyEval_SaveThread();
    int failures = 0; auto check = [&failures](bool value, const char* message) { if (!value) { std::cerr << message << "\n"; ++failures; } };
    inferdeck::model::ModelInfo info; info.runtime="vllm_radiance"; info.compute=inferdeck::model::ModelCompute::RocmGpu; info.context_size=106496; info.n_slots=1; info.min_slots=1; info.artifacts["python_root"]="build/runtime-radiance-probe/toolchain/python312-standalone/python";
    inferdeck::vllm_radiance_wrapper::VllmRadianceModel model(info); inferdeck::foundation::Result<void> loaded = inferdeck::foundation::Err<void>(inferdeck::foundation::ErrorCode::Internal, "not run"); std::thread loader([&] { loaded = model.load(); }); loader.join(); check(loaded.has_value(), "cross-thread load failed"); auto slot=model.acquire_slot(); check(slot.has_value(), "slot acquisition failed"); if (!slot) { PyEval_RestoreThread(mainState); Py_Finalize(); return 1; }
    inferdeck::model::InferenceRequest request; request.max_output_tokens=32; request.sampling.presence_penalty=1.0f; request.sampling.frequency_penalty=0.5f; request.sampling.repeat_penalty=1.0f; request.sampling.logit_bias={{42, -2.0f}}; request.tool_choice.kind=inferdeck::inference::ToolChoiceKind::Required; request.enable_reasoning=true;
    inferdeck::inference::Message message(inferdeck::inference::MessageRole::Assistant,"prior"); message.reasoning="prior reasoning"; message.tool_calls.push_back({"old","weather","{\"city\":\"Leeds\"}"}); request.messages.push_back(std::move(message));
    inferdeck::inference::FunctionTool tool; tool.name="weather"; tool.parameters_schema="{\"type\":\"object\"}"; request.tools.push_back(tool);
    int callbacks=0; auto result=model.predict_stream(*slot,request,[&](const inferdeck::model::InferenceDelta& delta){++callbacks;return true;},nullptr); check(result.has_value(), "stream result failed"); check(callbacks==3, "tool deltas were not streamed"); check(result && result->tool_calls.size()==1, "tool calls did not merge"); check(result && result->tool_calls[0].function_name=="weather", "tool name mismatch"); check(result && result->tool_calls[0].function_arguments=="{\"city\":\"Leeds\"}", "tool argument merge mismatch"); check(model.release_slot(*slot).has_value(), "slot release failed");
    auto cancelSlot=model.acquire_slot(); check(cancelSlot.has_value(), "cancel slot acquisition failed"); std::atomic<bool> cancelled{true}; auto cancelledResult=model.predict_cancellable(*cancelSlot,request,&cancelled); check(!cancelledResult, "cancelled request unexpectedly succeeded"); check(model.release_slot(*cancelSlot).has_value(), "cancel slot release failed"); auto freshSlot=model.acquire_slot(); check(freshSlot.has_value(), "fresh slot acquisition after cancel failed"); if (freshSlot) { auto fresh=model.predict_stream(*freshSlot,request,[](const inferdeck::model::InferenceDelta&) { return true; },nullptr); check(fresh.has_value(), "fresh request after cancellation failed"); check(model.release_slot(*freshSlot).has_value(), "fresh slot release failed"); } check(model.unload().has_value(), "initial unload failed"); for (int cycle=0; cycle<20; ++cycle) { check(model.load().has_value(), "repeat load failed"); check(model.unload().has_value(), "repeat unload failed"); }
    PyEval_RestoreThread(mainState); Py_Finalize(); if (failures != 0) return 1; std::cout << "PASS\n";
}
