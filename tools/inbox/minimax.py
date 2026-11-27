"""The transcription of MiniMax.

MiniMax has an endpoint of its own for that, so it does not speak the OpenAI
format that the rest of the model layer speaks. The API key is sent only as
an Authorization header. It is not written into transcripts, notes, or error
text.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.request


def transcribe_audio(audio: bytes, name: str, api_key: str, roots: list[str],
                     opener=urllib.request.urlopen) -> str:
    last_error = "transcription failed"
    for root in roots:
        request = _multipart(f"{root}/speech_to_text", audio, name, api_key)
        try:
            with opener(request) as response:
                payload = json.loads(response.read().decode())
        except urllib.error.HTTPError as error:
            last_error = f"transcription rejected ({error.code})"
            continue
        text = payload.get("text") if isinstance(payload, dict) else None
        if isinstance(text, str) and text:
            return text
        last_error = "transcription returned no text"
    raise RuntimeError(last_error)


def _multipart(url: str, audio: bytes, name: str, api_key: str) -> urllib.request.Request:
    boundary = "----ai-recorder"
    head = (
        f"--{boundary}\r\n"
        'Content-Disposition: form-data; name="model"\r\n\r\n'
        "asr-1.0\r\n"
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{name}"\r\n'
        "Content-Type: application/ogg\r\n\r\n"
    ).encode()
    tail = f"\r\n--{boundary}--\r\n".encode()
    request = urllib.request.Request(url, data=head + audio + tail, method="POST")
    request.add_header("Authorization", f"Bearer {api_key}")
    request.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
    return request
