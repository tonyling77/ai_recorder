"""The model layer of the inbox.

Everything here speaks the OpenAI format, so the provider behind the
configuration does not matter: the same request shape reaches MiniMax, Qwen
and anything else that answers /chat/completions.

The transcription of MiniMax is the one exception. It has an endpoint of its
own, so it lives in minimax.py and asr.py chooses between the two.

The API key is read from the configuration and sent only as an Authorization
header. It is not written into transcripts, notes, or error text.
"""

from __future__ import annotations

import base64
import json
import re
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

# The providers the short names stand for. The mark is what the address of
# the endpoint carries. An address that carries none of them belongs to
# nobody known: a proxy in front of a provider is still that provider, but
# the address does not say which one.
PROVIDER_FAMILIES = {
    "minimax": ("minimax",),
    "qwen": ("qianwen", "dashscope", "aliyuncs", "qwen"),
    "mimo": ("mimo", "xiaomi"),
    "deepseek": ("deepseek",),
    # The address of Zhipu says bigmodel, not glm.
    "glm": ("glm", "z-ai", "bigmodel"),
    "doubao": ("volces", "doubao", "ark.cn-beijing"),
    "kimi": ("moonshot", "kimi"),
    "baidu": ("qianfan", "baidubce"),
    "spark": ("xinghuo", "spark"),
    "hunyuan": ("hunyuan",),
}

# The audio types the inbox carries. The recorder writes Ogg Opus, and the
# Ogg container is what the audio is named after.
AUDIO_MIME = {
    ".ogg": "audio/ogg",
    ".opus": "audio/ogg",
    ".wav": "audio/wav",
    ".mp3": "audio/mpeg",
    ".m4a": "audio/mp4",
}

# The question every writer is asked. One question for every route, so that
# the model and the agents write the same note and the note is checked the
# same way.
SUMMARY_FORMAT = (
    "根据下面的会议转写生成纪要。只返回一个 JSON 对象，不要 Markdown，不要解释，不要输出思考过程。"
    '格式为 {"title":"标题","conclusions":["结论"],'
    '"todos":[{"item":"待办","evidence":"转写中的完整原句"}],"quotes":["原话"]}。'
    "每条待办的 evidence 必须逐字出现在转写中；没有明确行动要求时 todos 必须为空。"
)


@dataclass(frozen=True)
class ModelSettings:
    """What the configuration says about the model.

    base_url, model and api_key are the OpenAI format: the endpoint, the
    model that writes the summary, and the key. asr_model is the model that
    transcribes. asr_base_url and asr_api_key let the transcription sit on
    another endpoint, and they are empty when it sits on the same one.
    """

    base_url: str
    model: str
    api_key: str
    asr_model: str = ""
    asr_base_url: str = ""
    asr_api_key: str = ""
    asr_prompt: str = ""


def read_section(path: Path, name: str) -> dict[str, str]:
    """The keys of one top-level section of the configuration.

    The file is read by hand, so that the inbox needs nothing but the
    standard library. A line that starts in the first column opens a new
    section, and a comment is a comment wherever it stands.
    """
    keys: dict[str, str] = {}
    inside = False
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.strip().startswith("#"):
            continue
        if not raw.startswith((" ", "\t")):
            inside = raw.partition(":")[0].strip() == name
            continue
        if not inside:
            continue
        key, _, value = raw.strip().partition(":")
        keys[key.strip()] = value.strip().strip("'").strip('"')
    return keys


def read_model_settings(path: Path) -> ModelSettings:
    """Read the model section of the configuration.

    Only the model section is read. A key that sits under another section
    belongs to that section and never reaches a request.
    """
    model = read_section(path, "model")
    base_url = model.get("base_url", "")
    name = model.get("default", "")
    api_key = model.get("api_key", "")
    if not base_url or not name or not api_key:
        raise ValueError("model settings are incomplete")
    for field, value in (("base_url", base_url), ("default", name), ("api_key", api_key),
                         ("asr", model.get("asr", ""))):
        if _is_placeholder(value):
            raise ValueError(f"the {field} in the model section is still the example")
    return ModelSettings(
        base_url=base_url,
        model=name,
        api_key=api_key,
        asr_model=model.get("asr", ""),
        asr_base_url=model.get("asr_base_url", ""),
        asr_api_key=model.get("asr_api_key", ""),
        asr_prompt=model.get("asr_prompt", ""),
    )


def _is_placeholder(value: str) -> bool:
    """Whether a value is still the one from the example.

    The example writes what a value is between angle brackets. A value that
    still has them was never filled in, and it is caught here rather than
    at the provider, where the answer would be a puzzling rejection.
    """
    text = (value or "").strip()
    return text.startswith("<") and text.endswith(">")


def read_route(path: Path) -> dict[str, str]:
    """The route the configuration asks for.

    It names the agent and the model, and it may name the provider and the
    model id of that agent, for the case where the model behind a short
    name is not the one this Mac has.
    """
    return read_section(path, "route")


def read_agents(path: Path) -> dict[str, dict[str, str]]:
    """The agents the configuration declares.

    An agent is a program that takes a question and answers with a note.
    The inbox knows a few of them, and the configuration may declare any
    other one, so that a person is not held back by the ones that happen
    to be built in.
    """
    agents: dict[str, dict[str, str]] = {}
    inside = False
    agent = ""
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.strip().startswith("#"):
            continue
        if not raw.startswith((" ", "\t")):
            inside = raw.partition(":")[0].strip() == "agents"
            agent = ""
            continue
        if not inside:
            continue
        key, _, value = raw.strip().partition(":")
        name = key.strip()
        if not value.strip():
            # A name with nothing after it opens an agent.
            agent = name
            agents.setdefault(agent, {})
            continue
        if agent:
            agents[agent][name] = value.strip().strip("'").strip('"')
    return agents


def provider_family(base_url: str) -> str:
    """The short name of the provider behind an endpoint, or an empty one."""
    address = (base_url or "").lower()
    for family, marks in PROVIDER_FAMILIES.items():
        if any(mark in address for mark in marks):
            return family
    return ""


def candidate_roots(base_url: str) -> list[str]:
    """The endpoints to try, in order.

    MiniMax answers on two hosts and the first one is not always the one
    that is reachable from here, so both are tried. Every other provider has
    one endpoint, which is the one that was configured.
    """
    cn = "https://api.minimax.cn/v1"
    io = "https://api.minimax.io/v1"
    if "minimax.cn" in base_url:
        return [cn, io]
    if "minimax.io" in base_url:
        return [io, cn]
    return [base_url.rstrip("/")]


def summary_prompt(transcript: str) -> str:
    """The question a writer is asked.

    One question for every route, so that the model and the agents write the
    same note and the note is checked the same way.
    """
    return f"{SUMMARY_FORMAT}\n\n{transcript}"


def chat_completion(prompt: str, api_key: str, model: str, roots: list[str],
                    opener=urllib.request.urlopen) -> str:
    """Ask the model, in the OpenAI format, and take what it answers.

    The answer is what the model wrote and not a note: the note is made from
    it by render_summary, so that every route is checked the same way.
    """
    body = json.dumps(
        {"model": model, "messages": [{"role": "user", "content": prompt}]},
        ensure_ascii=False,
    ).encode()
    last_error = "the model did not answer"
    for root in roots:
        request = urllib.request.Request(f"{root}/chat/completions", data=body, method="POST")
        request.add_header("Authorization", f"Bearer {api_key}")
        request.add_header("Content-Type", "application/json")
        try:
            with opener(request) as response:
                payload = json.loads(response.read().decode())
        except urllib.error.HTTPError as error:
            last_error = f"{model} rejected the request ({error.code})"
            continue
        content = _content_of(payload)
        if content:
            return content
        last_error = f"{model} returned no text"
    raise RuntimeError(last_error)


def transcribe_openai_audio(audio: bytes, name: str, model: str, api_key: str,
                            roots: list[str], opener=urllib.request.urlopen,
                            instruction: str = "") -> str:
    """Transcribe with a model that takes the audio in a chat request.

    The audio goes in as a data URI, which is the OpenAI format of an audio
    input. The answer is the text of the recording.

    A model that is only good at transcription needs no question. A model
    that also talks answers in words, so it is given one: the instruction
    goes in front of the audio and asks for the words and nothing else.
    """
    content = []
    if instruction:
        content.append({"type": "text", "text": instruction})
    content.append({
        "type": "input_audio",
        "input_audio": {"data": _audio_data_uri(audio, name)},
    })
    body = json.dumps(
        {"model": model, "messages": [{"role": "user", "content": content}]},
        ensure_ascii=False,
    ).encode()
    last_error = "transcription failed"
    for root in roots:
        request = urllib.request.Request(f"{root}/chat/completions", data=body, method="POST")
        request.add_header("Authorization", f"Bearer {api_key}")
        request.add_header("Content-Type", "application/json")
        try:
            with opener(request) as response:
                payload = json.loads(response.read().decode())
        except urllib.error.HTTPError as error:
            last_error = f"transcription rejected ({error.code})"
            continue
        text = _content_of(payload)
        if text:
            return text
        last_error = "transcription returned no text"
    raise RuntimeError(last_error)


def render_summary(content: str, transcript: str) -> str:
    """Turn the answer of a writer into the note.

    The answer is JSON. An agent answers in prose and puts the JSON inside
    it, so the JSON is looked for before it is read. The note is then
    checked: a todo whose evidence is not in the transcript word for word
    is refused, because a note that invents an action is worse than no note.
    """
    final = re.sub(r"<think\b[^>]*>.*?</think\s*>", "", content, flags=re.IGNORECASE | re.DOTALL).strip()
    if re.search(r"</?think\b", final, flags=re.IGNORECASE):
        raise RuntimeError("summary contained incomplete reasoning")
    data = _summary_data(final)
    if not isinstance(data.get("title"), str) or not data["title"].strip():
        raise RuntimeError("summary title is missing")
    conclusions = _string_list(data.get("conclusions"), "conclusions")
    quotes = _string_list(data.get("quotes"), "quotes")
    todos = data.get("todos")
    if not isinstance(todos, list):
        raise RuntimeError("summary todos are invalid")
    checked_todos = []
    for todo in todos:
        if not isinstance(todo, dict):
            raise RuntimeError("summary todo is invalid")
        item = todo.get("item")
        evidence = todo.get("evidence")
        if not isinstance(item, str) or not item.strip() or not isinstance(evidence, str) or not evidence.strip():
            raise RuntimeError("summary todo is invalid")
        if _normalize(evidence) not in _normalize(transcript):
            raise RuntimeError("summary todo evidence is absent from transcript")
        checked_todos.append((item.strip(), evidence.strip()))
    for quote in quotes:
        if _normalize(quote) not in _normalize(transcript):
            raise RuntimeError("summary quote is absent from transcript")
    lines = [f"# {data['title'].strip()}", "", "## 讨论结论", ""]
    lines.extend(f"- {item}" for item in conclusions)
    if not conclusions:
        lines.append("无明确结论。")
    lines.extend(["", "## 待办事项", ""])
    for item, evidence in checked_todos:
        lines.extend([f"- {item}", f"  - 原句：{evidence}"])
    if not checked_todos:
        lines.append("无明确待办。")
    lines.extend(["", "## 原话摘录", ""])
    lines.extend(f"> {quote}" for quote in quotes)
    if not quotes:
        lines.append("无。")
    return "\n".join(lines).rstrip() + "\n"


def _content_of(payload) -> str:
    """The text of an answer in the OpenAI format."""
    if not isinstance(payload, dict):
        return ""
    choices = payload.get("choices")
    if not choices or not isinstance(choices[0], dict):
        return ""
    message = choices[0].get("message")
    if not isinstance(message, dict):
        return ""
    content = message.get("content")
    return content if isinstance(content, str) else ""


def _summary_data(text: str) -> dict:
    """The summary inside an answer.

    A model answers with JSON and nothing else. An agent answers in prose
    and puts the JSON inside it, so the JSON is looked for.
    """
    if text.startswith("```"):
        text = re.sub(r"^```(?:json)?\s*", "", text, count=1, flags=re.IGNORECASE)
        text = re.sub(r"\s*```\s*$", "", text)
    try:
        data = json.loads(text)
    except (json.JSONDecodeError, TypeError):
        data = None
    if isinstance(data, dict):
        return data
    start = text.find("{")
    while start >= 0:
        end = _closing_brace(text, start)
        if end > start:
            try:
                data = json.loads(text[start:end + 1])
            except (json.JSONDecodeError, TypeError):
                data = None
            if isinstance(data, dict):
                return data
        start = text.find("{", start + 1)
    raise RuntimeError("summary returned invalid JSON")


def _closing_brace(text: str, start: int) -> int:
    """The index of the brace that closes the one at start.

    A brace inside a string is not a brace, so the strings are stepped over.
    """
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(text)):
        char = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index
    return -1


def _string_list(value, name: str) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) or not item.strip() for item in value):
        raise RuntimeError(f"summary {name} are invalid")
    return [item.strip() for item in value]


# The punctuation that is written in two ways. Two models write the same
# sentence with different punctuation and different spaces around it, so
# both are taken away before the text is looked for. What is left is the
# words, and a sentence that is not in the transcript is still not in it.
_PUNCTUATION = set(
    "。、，；：？！“”‘’（）【】《》〈〉「」『』—～…·．"
    ".,;:!?\"'()[]{}<>/\\|`~@#$%^&*_+=-\n\r\t "
)


def _normalize(text: str) -> str:
    """The text as the comparison sees it."""
    return "".join(char for char in text if char not in _PUNCTUATION)


def _audio_data_uri(audio: bytes, name: str) -> str:
    """The audio in the form a chat request carries it."""
    suffix = "." + name.rsplit(".", 1)[-1].lower() if "." in name else ""
    mime = AUDIO_MIME.get(suffix, "audio/ogg")
    return f"data:{mime};base64," + base64.b64encode(audio).decode("ascii")
