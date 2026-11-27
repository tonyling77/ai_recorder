"""Who writes the summary.

Three ways: the model itself, an agent that runs on this Mac, or an agent
that is not connected here. Every writer is asked the same question and
every answer is checked the same way, so the note a route produces has the
same shape whichever route produced it.

The agent keeps its own keys in its own configuration. The inbox never sees
a provider key of an agent, only the answer the agent gives. An agent whose
provider is not signed in says so, and that is what the inbox records.

An agent is a program that takes a question and answers with a note. The
inbox knows a few of them and the configuration may declare any other one,
so nobody is held back by the ones that happen to be built in.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
from pathlib import Path

import model_api
from agent_route import AGENTS, MODELS, AgentUnavailable, RouteError

# The model each built-in agent answers to, under the short name the user
# picks. The short name is the same everywhere; the model behind it is not,
# and each agent knows its own. The provider is named when the agent has
# more than one: Hermes reaches MiniMax under one name and Qwen under
# another.
AGENT_MODELS = {
    "hermes": {
        "minimax": ("minimax-cn", "MiniMax-M3"),
        "mimo": ("xiaomi", "mimo-v2.6-pro"),
        "deepseek": ("deepseek", "deepseek-v4-pro"),
        "glm": ("openrouter", "z-ai/glm-5.3"),
        "qwen": ("openrouter", "qwen/qwen3.8-max-0902"),
    },
    "workbuddy": {
        "minimax": ("", "minimax-m2.7"),
        "glm": ("", "glm-5.1"),
        "deepseek": ("", "deepseek-v3-2-volc"),
    },
    "opencode": {
        "minimax": ("", "minimax-cn-coding-plan/MiniMax-M3"),
        "mimo": ("", "xiaomi-mimo/mimo-v2.5-pro"),
        "deepseek": ("", "deepseek/deepseek-v4-pro"),
        "glm": ("", "minimax-cn-coding-plan/MiniMax-M3.1-Flash"),
        "qwen": ("", "minimax-cn-coding-plan/MiniMax-M3.1-Flash"),
    },
    "muse": {},
}

# The CLI of WorkBuddy. The desktop application carries its own, and the
# path is inside the application, so it is looked for rather than assumed.
WORKBUDDY_CLI = "/Applications/WorkBuddy.app/Contents/Resources/app.asar.unpacked/cli/dist/codebuddy-lite-wb.mjs"

# How long an agent may take to answer.
AGENT_TIMEOUT_SECONDS = 600


def build_backends(settings: model_api.ModelSettings, route: tuple[str, str] = ("", ""),
                   overrides: dict | None = None, agents: dict | None = None) -> dict:
    """Every route that can be taken, by (agent, model).

    The direct route is there for every short name: the model that answers
    is the one the configuration names, and the short name says which
    provider it is meant to be. An agent route is there only for the models
    that agent knows.

    The overrides are the provider and the model id the user wrote down,
    and they belong to the route the user chose. The agents are the ones
    the configuration declares, and they answer under every short name.
    """
    overrides = overrides or {}
    backends = {}
    for name in MODELS:
        backends[("direct", name)] = _direct(settings, name)
    for agent, models in AGENT_MODELS.items():
        for name, (provider, model_id) in models.items():
            if (agent, name) == route:
                provider = overrides.get("provider") or provider
                model_id = overrides.get("model_id") or model_id
            backends[(agent, name)] = _agent(agent, provider, model_id)
    for agent, declaration in (agents or {}).items():
        for name in MODELS:
            model_id = overrides.get("model_id") or _model_id_of(agent, name)
            backends[(agent, name)] = _declared_agent(agent, declaration, model_id)
    return backends


def _model_id_of(agent: str, model: str) -> str:
    """The model id of an agent, when the inbox knows the agent."""
    entry = AGENT_MODELS.get(agent, {}).get(model)
    return entry[1] if entry else model


def resolve_route(settings: model_api.ModelSettings, agent: str = "", model: str = "",
                  configured: dict | None = None, agents: dict | None = None) -> tuple[str, str]:
    """The route the host asks for.

    The command line wins over the configuration, and the configuration wins
    over what the address of the endpoint says.
    """
    configured = configured or {}
    agent = agent or configured.get("agent", "") or "direct"
    model = model or configured.get("model", "") or model_api.provider_family(settings.base_url)
    if agent not in AGENTS and agent not in (agents or {}):
        raise RouteError(f"{agent}/{model}")
    if not model:
        raise RouteError("the model of the route is not named: add route: to the configuration")
    if agent == "direct":
        family = model_api.provider_family(settings.base_url)
        if family and family != model:
            raise RouteError(f"the configured endpoint is a {family} endpoint, not a {model} one")
    return (agent, model)


def ensure_route_usable(route: tuple[str, str], backends: dict) -> None:
    """Refuse a route that no backend answers.

    A combination that does not exist is refused before a recording is
    lost. An agent that has no model under that short name is not called,
    and the recording that was waiting for it would sit untranscribed.
    """
    agent, model = route
    if (agent, model) not in backends:
        raise RouteError(f"no {agent} answers for {model}")


def _direct(settings: model_api.ModelSettings, name: str):
    """The route that asks the model itself.

    The model that answers is the one the configuration names. The short
    name says which provider it is meant to be, and a known provider that
    does not match is refused rather than quietly answered by another one.
    The check is here and not in the build of the routes, because a route
    that is not in use is not wrong.
    """
    roots = model_api.candidate_roots(settings.base_url)

    def write(transcript: str) -> str:
        family = model_api.provider_family(settings.base_url)
        if family and family != name:
            raise RouteError(f"the configured endpoint is a {family} endpoint, not a {name} one")
        content = model_api.chat_completion(
            model_api.summary_prompt(transcript),
            settings.api_key,
            settings.model,
            roots,
        )
        return model_api.render_summary(content, transcript)

    return write


def _agent(agent: str, provider: str, model_id: str):
    """The route that asks an agent on this Mac."""

    def write(transcript: str) -> str:
        answer = run_agent(agent, model_api.summary_prompt(transcript), provider, model_id)
        return model_api.render_summary(answer, transcript)

    return write


def _declared_agent(agent: str, declaration: dict, model_id: str):
    """The route that asks an agent the configuration declares.

    The command is a line of arguments. The question goes where the line
    says: into the {prompt} if the line has one, and onto the standard
    input if it does not.
    """
    command = declaration.get("command", "")

    def write(transcript: str) -> str:
        prompt = model_api.summary_prompt(transcript)
        answer = run_declared(agent, command, prompt, model_id)
        return model_api.render_summary(answer, transcript)

    return write


def run_agent(agent: str, prompt: str, provider: str, model_id: str) -> str:
    """Ask an agent on this Mac, and take what it answers.

    The question goes in on standard input, so a long transcript is not put
    on a command line where it can be seen.
    """
    if agent == "hermes":
        command = hermes_command(provider, model_id)
    elif agent == "workbuddy":
        command = workbuddy_command(model_id)
    elif agent == "opencode":
        command = opencode_command(model_id)
    else:
        raise AgentUnavailable(agent)
    return _run(command, prompt)


def run_declared(agent: str, command: str, prompt: str, model_id: str) -> str:
    """Ask an agent the configuration declares, and take what it answers."""
    if not command.strip():
        raise AgentUnavailable(agent)
    argv = shlex.split(command.replace("{model}", model_id))
    if any("{prompt}" in item for item in argv):
        argv = [item.replace("{prompt}", prompt) for item in argv]
        return _run(argv, None)
    return _run(argv, prompt)


def hermes_command(provider: str, model_id: str) -> list[str]:
    """The command that asks Hermes.

    The question goes in through a file, because a long transcript does not
    belong on a command line. The quiet flag keeps the answer the only
    thing that comes out.
    """
    path = os.environ.get("HERMES_CLI") or shutil.which("hermes")
    if not path:
        candidate = Path.home() / ".local" / "bin" / "hermes"
        path = str(candidate) if candidate.exists() else ""
    if not path:
        raise AgentUnavailable("hermes")
    command = [path, "chat", "--query-file", "-", "--oneshot", "-Q", "--format", "text"]
    if provider:
        command.extend(["--provider", provider])
    command.extend(["-m", model_id])
    return command


def workbuddy_command(model_id: str) -> list[str]:
    """The command that asks WorkBuddy.

    WorkBuddy is a desktop application that carries its own CLI. The CLI in
    the bundle is the one that answers, so it is looked for.
    """
    candidates = []
    override = os.environ.get("WORKBUDDY_CLI")
    if override:
        candidates.append(override)
    candidates.append(WORKBUDDY_CLI)
    candidates.append(str(Path.home() / "Applications" / WORKBUDDY_CLI.lstrip("/")))
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return ["node", candidate, "-p", "--output-format", "text", "--model", model_id]
    raise AgentUnavailable("workbuddy")


def opencode_command(model_id: str) -> list[str]:
    """The command that asks OpenCode.

    The question goes in as an argument, because that is the shape of
    `opencode run`.
    """
    path = os.environ.get("OPENCODE_CLI") or shutil.which("opencode")
    if not path:
        candidate = Path.home() / ".opencode" / "bin" / "opencode"
        path = str(candidate) if candidate.exists() else ""
    if not path:
        raise AgentUnavailable("opencode")
    return [path, "run", "--model", model_id, "{prompt}"]


def _run(command: list[str], prompt: str | None) -> str:
    try:
        result = subprocess.run(
            command,
            input=prompt,
            capture_output=True,
            text=True,
            timeout=AGENT_TIMEOUT_SECONDS,
        )
    except FileNotFoundError as error:
        raise AgentUnavailable(str(error)) from error
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("the agent did not answer in time") from error
    answer = (result.stdout or "").strip()
    if not answer:
        # An agent that is not signed in says so on its error stream and
        # answers nothing. That is the reason, and it is worth more than
        # an empty answer.
        detail = (result.stderr or "").strip().splitlines()
        if detail:
            raise RuntimeError("the agent answered nothing: " + detail[-1][:200])
        raise RuntimeError("the agent answered nothing")
    if result.returncode != 0:
        detail = (result.stderr or "").strip().splitlines()
        reason = detail[-1][:200] if detail else f"exit {result.returncode}"
        raise RuntimeError("the agent failed: " + reason)
    return answer
