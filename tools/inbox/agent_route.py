"""Chooses which Mac agent writes a recording summary.

The device never sees provider keys. A route is a short agent name plus a
short model name. An agent that has no backend is not called.

The agent is who writes the note: the model itself, or an agent that runs on
this Mac. The model is which model answers. The short names are the same
everywhere; the model behind them is not, and each agent knows its own.
"""


class RouteError(ValueError):
    """The agent or model name is not one of the allowed short names."""


class AgentUnavailable(Exception):
    """The named agent is allowed, but this Mac has no backend for it."""


AGENTS = ("direct", "hermes", "workbuddy", "opencode", "muse")
# The short names of the models. Every one of them stands for a provider that
# answers in the OpenAI format, so the inbox can write the summary with any
# of them. An agent knows its own models; these are the names it is asked
# for.
MODELS = ("minimax", "mimo", "deepseek", "glm", "qwen",
          "doubao", "kimi", "baidu", "spark", "hunyuan")


def ensure_route(agent: str, model: str, backends: dict) -> None:
    """Check that the route can be taken.

    The backends are the authority: a route that a backend answers is
    good, whether the name is one of the allowed short names or one the
    configuration declared.
    """
    if (agent, model) in backends:
        return
    if agent not in AGENTS or model not in MODELS:
        raise RouteError(f"{agent}/{model}")
    raise AgentUnavailable(agent)


def route_summary(agent: str, model: str, transcript: str, backends: dict) -> str:
    ensure_route(agent, model, backends)
    backend = backends[(agent, model)]
    note = backend(transcript)
    return f"Agent: {agent}\nModel: {model}\n{note}"
