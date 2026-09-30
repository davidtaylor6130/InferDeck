def create(config):
    return {"active": set(), "request": None, "step": 0}
def begin(state, request):
    assert request["tool_choice"] == "required"
    assert abs(request["sampling"]["temperature"] - 0.65) < 1e-6
    assert abs(request["sampling"]["top_p"] - 0.87) < 1e-6
    assert request["sampling"]["top_k"] == 17
    assert abs(request["sampling"]["min_p"] - 0.0) < 1e-6
    assert request["sampling"]["presence_penalty"] == 1.0
    assert request["sampling"]["frequency_penalty"] == 0.5
    assert any(abs(request["sampling"]["repetition_penalty"] - value) < 1e-6 for value in (1.0, 1.2))
    assert request["repeat_last_n"] in (-1, 64)
    assert request["sampling"]["logit_bias"] == {42: -2.0}
    assistant = request["messages"][0]
    assert assistant["reasoning_content"] == "prior reasoning"
    assert assistant["tool_calls"][0]["function"]["arguments"] == "{\"city\":\"Leeds\"}"
    assert isinstance(assistant["tool_calls"][0]["function"]["arguments"], str)
    state["request"] = request; state["step"] = 0; state["active"].add("stub"); return "stub"
def step(state, request_id, request):
    if request_id not in state["active"]: return []
    state["step"] += 1
    rows = [
      {"text":"","reasoning":"","tool_calls":[{"index":0,"id":"call_1","type":"function","name":"weather","arguments":"{\"city\":"}],"finished":False,"prompt_tokens":8,"cached_tokens":2,"completion_tokens":1,"prompt_duration_ms":4.0,"generation_duration_ms":2.0,"finish_reason":"stop"},
      {"text":"","reasoning":"","tool_calls":[{"index":0,"id":"","type":"","name":"","arguments":"\"Le"}],"finished":False,"prompt_tokens":8,"cached_tokens":2,"completion_tokens":2,"prompt_duration_ms":4.0,"generation_duration_ms":4.0,"finish_reason":"stop"},
      {"text":"","reasoning":"","tool_calls":[{"index":0,"id":"","type":"","name":"","arguments":"eds\"}"}],"finished":True,"prompt_tokens":8,"cached_tokens":2,"completion_tokens":3,"prompt_duration_ms":4.0,"generation_duration_ms":6.0,"finish_reason":"tool_calls"},
    ]
    row=rows[state["step"]-1]
    if row["finished"]: state["active"].discard(request_id)
    return [row]
def abort(state, request_id): state["active"].discard(request_id)
def shutdown(state): state["active"].clear()


def collect_released_resources(): return None
