import io
import json
import unittest
import urllib.error

from minimax import transcribe_audio


class Response:
    def __init__(self, payload):
        self.payload = json.dumps(payload).encode()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def read(self):
        return self.payload


class MinimaxTest(unittest.TestCase):
    def test_transcription_uses_the_second_host_when_the_first_rejects(self):
        seen = []

        def opener(request):
            seen.append(request.full_url)
            if "minimax.cn" in request.full_url:
                raise urllib.error.HTTPError(request.full_url, 401, "no", hdrs=None, fp=io.BytesIO(b""))
            self.assertIn(b"asr-1.0", request.data)
            self.assertIn(b"note.ogg", request.data)
            self.assertNotIn(b"super-secret", request.data)
            return Response({"text": "明天交报告"})

        text = transcribe_audio(
            b"OggS",
            "note.ogg",
            "super-secret",
            ["https://api.minimax.cn/v1", "https://api.minimax.io/v1"],
            opener,
        )
        self.assertEqual(text, "明天交报告")
        self.assertEqual(
            seen,
            [
                "https://api.minimax.cn/v1/speech_to_text",
                "https://api.minimax.io/v1/speech_to_text",
            ],
        )

    def test_a_rejected_transcription_does_not_reveal_the_key(self):
        def opener(request):
            raise urllib.error.HTTPError(request.full_url, 401, "no", hdrs=None, fp=io.BytesIO(b"super-secret"))

        with self.assertRaises(RuntimeError) as caught:
            transcribe_audio(b"OggS", "note.ogg", "super-secret", ["https://api.minimax.cn/v1"], opener)
        self.assertNotIn("super-secret", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
