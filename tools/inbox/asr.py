"""Which backend transcribes the recording.

MiniMax has an endpoint of its own for that. Every other provider takes the
audio in an OpenAI-style chat request, and the model that does it is named
in the configuration.
"""

from __future__ import annotations

from model_api import ModelSettings, candidate_roots, provider_family, transcribe_openai_audio
from minimax import transcribe_audio as transcribe_minimax

# The model that transcribes when the configuration does not name one and
# the address says which provider it is.
DEFAULT_ASR_MODELS = {
    "qwen": "qwen3-asr-flash",
}


def asr_endpoint(settings: ModelSettings) -> str:
    """The address the transcription goes to.

    It is the address of the summary unless the configuration names another
    one, so that a transcription and a summary may sit on two providers.
    """
    return (settings.asr_base_url or settings.base_url).rstrip("/")


def asr_model(settings: ModelSettings) -> str:
    """The model that transcribes.

    MiniMax names its own model, so the address decides. Every other
    provider needs the name in the configuration; without it, the name that
    the Qwen models answer to is used when the address says Qwen, and any
    other provider is asked to say which model it wants.
    """
    endpoint = asr_endpoint(settings).lower()
    if "minimax" in endpoint:
        return "asr-1.0"
    if settings.asr_model:
        return settings.asr_model
    family = provider_family(asr_endpoint(settings))
    if family in DEFAULT_ASR_MODELS:
        return DEFAULT_ASR_MODELS[family]
    raise ValueError("the model that transcribes is not named: add asr: to the model section")


def transcriber(settings: ModelSettings):
    """The function that turns one slice into text."""
    endpoint = asr_endpoint(settings)
    roots = candidate_roots(endpoint)
    key = settings.asr_api_key or settings.api_key
    model = asr_model(settings)

    if "minimax" in endpoint.lower():
        def transcribe(audio: bytes, name: str) -> str:
            return transcribe_minimax(audio, name, key, roots)
    else:
        def transcribe(audio: bytes, name: str) -> str:
            return transcribe_openai_audio(audio, name, model, key, roots,
                                           instruction=settings.asr_prompt)

    return transcribe
