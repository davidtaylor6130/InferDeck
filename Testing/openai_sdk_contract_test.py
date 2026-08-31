import base64
import json
import struct
import unittest

import httpx2
from openai import OpenAI


RESPONSE_OBJECT = {
    "id": "resp_contract",
    "object": "response",
    "created_at": 123,
    "completed_at": 124,
    "status": "completed",
    "output_text": "Hello",
    "error": None,
    "incomplete_details": None,
    "instructions": None,
    "metadata": {},
    "model": "contract-model",
    "output": [{
        "id": "msg_contract",
        "type": "message",
        "status": "completed",
        "role": "assistant",
        "content": [{"type": "output_text", "text": "Hello", "annotations": []}],
    }],
    "parallel_tool_calls": True,
    "temperature": 1,
    "tool_choice": "auto",
    "tools": [],
    "top_p": 1,
    "background": False,
    "conversation": None,
    "max_output_tokens": None,
    "previous_response_id": None,
    "reasoning": {},
    "service_tier": "default",
    "store": False,
    "text": {"format": {"type": "text"}},
    "truncation": "disabled",
    "usage": {
        "input_tokens": 3,
        "input_tokens_details": {"cached_tokens": 0},
        "output_tokens": 4,
        "output_tokens_details": {"reasoning_tokens": 0},
        "total_tokens": 7,
    },
}


class StrictOpenAIContractTest(unittest.TestCase):
    def setUp(self):
        self.requested = []
        self.chat_request = None
        self.client = OpenAI(
            api_key="contract-test",
            base_url="http://inferdeck.test/v1",
            http_client=httpx2.Client(transport=httpx2.MockTransport(self.handle)),
        )

    def tearDown(self):
        self.client.close()

    def handle(self, request):
        path = request.url.path
        self.requested.append(f"{request.method} {path}")
        if path == "/v1/models":
            return httpx2.Response(200, json={
                "object": "list",
                "data": [{"id": "contract-model", "object": "model", "created": 0, "owned_by": "inferdeck"}],
            })
        if path == "/v1/chat/completions":
            self.chat_request = json.loads(request.content)
            return httpx2.Response(200, json={
                "id": "chatcmpl-contract",
                "object": "chat.completion",
                "created": 123,
                "model": "contract-model",
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": "Hello"},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 3, "completion_tokens": 4, "total_tokens": 7},
            })
        if path == "/v1/responses":
            body = json.loads(request.content)
            if body.get("stream"):
                created = dict(RESPONSE_OBJECT)
                created.pop("completed_at")
                created.update(status="in_progress", output_text="", output=[], usage=None)
                frames = (
                    "event: response.created\n"
                    f"data: {json.dumps({'type': 'response.created', 'sequence_number': 0, 'response': created})}\n\n"
                    "event: response.completed\n"
                    f"data: {json.dumps({'type': 'response.completed', 'sequence_number': 1, 'response': RESPONSE_OBJECT})}\n\n"
                )
                return httpx2.Response(200, text=frames, headers={"content-type": "text/event-stream"})
            return httpx2.Response(200, json=RESPONSE_OBJECT)
        if path == "/v1/embeddings":
            encoded = base64.b64encode(struct.pack("<ff", 0.1, 0.2)).decode()
            return httpx2.Response(200, json={
                "object": "list",
                "model": "contract-embedding",
                "data": [{"object": "embedding", "index": 0, "embedding": encoded}],
                "usage": {"prompt_tokens": 2, "total_tokens": 2},
            })
        if path == "/v1/images/generations":
            return httpx2.Response(200, json={
                "created": 123,
                "output_format": "png",
                "data": [{"b64_json": "iVBORw=="}],
            })
        if path == "/v1/audio/speech":
            return httpx2.Response(200, content=b"RIFF", headers={"content-type": "audio/wav"})
        if path == "/v1/audio/transcriptions":
            return httpx2.Response(200, json={"text": "contract transcript"})
        return httpx2.Response(500, json={
            "error": {"message": "unexpected route", "type": "server_error", "param": None, "code": "unexpected_route"},
        })

    def test_every_strict_endpoint(self):
        models = self.client.models.list()
        self.assertEqual(models.data[0].id, "contract-model")
        chat = self.client.chat.completions.create(
            model="contract-model",
            messages=[{"role": "user", "content": "Hello"}],
            frequency_penalty=1.2,
            presence_penalty=-0.4,
        )
        self.assertEqual(chat.choices[0].message.content, "Hello")
        self.assertEqual(self.chat_request["frequency_penalty"], 1.2)
        self.assertEqual(self.chat_request["presence_penalty"], -0.4)
        response = self.client.responses.create(
            model="contract-model", input="Hello", store=False)
        self.assertEqual(response.output_text, "Hello")
        self.assertEqual(response.completed_at, 124)
        stream = self.client.responses.create(
            model="contract-model", input="Hello", store=False, stream=True)
        events = list(stream)
        self.assertEqual([event.type for event in events],
                         ["response.created", "response.completed"])
        self.assertEqual(events[-1].response.output_text, "Hello")
        embeddings = self.client.embeddings.create(
            model="contract-embedding", input="Hello")
        self.assertAlmostEqual(embeddings.data[0].embedding[0], 0.1, places=6)
        self.assertAlmostEqual(embeddings.data[0].embedding[1], 0.2, places=6)
        image = self.client.images.generate(
            model="contract-image", prompt="pixel", response_format="b64_json")
        self.assertEqual(image.data[0].b64_json, "iVBORw==")
        speech = self.client.audio.speech.create(
            model="contract-speech", input="Hello", voice="default",
            response_format="wav", stream_format="audio")
        self.assertEqual(speech.content, b"RIFF")
        transcription = self.client.audio.transcriptions.create(
            model="contract-transcription",
            file=("test.wav", b"RIFF", "audio/wav"),
            response_format="json",
            stream=False,
        )
        self.assertEqual(transcription.text, "contract transcript")
        self.assertEqual(self.requested, [
            "GET /v1/models",
            "POST /v1/chat/completions",
            "POST /v1/responses",
            "POST /v1/responses",
            "POST /v1/embeddings",
            "POST /v1/images/generations",
            "POST /v1/audio/speech",
            "POST /v1/audio/transcriptions",
        ])


if __name__ == "__main__":
    unittest.main()
