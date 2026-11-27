import os
import shlex
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import writers
from agent_route import MODELS, AgentUnavailable, RouteError, ensure_route, route_summary
from model_api import ModelSettings


def settings(base_url="https://maas.qianwenaiapi.com/compatible-mode/v1", model="qwen-turbo"):
    return ModelSettings(base_url=base_url, model=model, api_key="secret")


class BackendsTest(unittest.TestCase):
    def test_the_model_itself_can_answer_under_every_short_name(self):
        backends = writers.build_backends(settings())
        for name in MODELS:
            self.assertIn(("direct", name), backends)

    def test_an_agent_has_the_models_it_knows_and_no_other(self):
        backends = writers.build_backends(settings())
        self.assertIn(("hermes", "minimax"), backends)
        self.assertIn(("hermes", "qwen"), backends)
        self.assertIn(("workbuddy", "minimax"), backends)
        self.assertNotIn(("workbuddy", "qwen"), backends)
        self.assertNotIn(("muse", "minimax"), backends)

    def test_an_agent_that_is_not_connected_is_not_called(self):
        backends = writers.build_backends(settings())
        with self.assertRaises(AgentUnavailable):
            ensure_route("muse", "minimax", backends)

    def test_a_name_that_is_not_allowed_is_refused_before_any_backend(self):
        backends = writers.build_backends(settings())
        with self.assertRaises(RouteError):
            ensure_route("direct", "gpt", backends)
        with self.assertRaises(RouteError):
            ensure_route("someone", "qwen", backends)

    def test_an_agent_the_configuration_declares_is_used(self):
        backends = writers.build_backends(settings(), ("my-agent", "qwen"), {},
                                           {"my-agent": {"command": "my-agent --model {model}"}})
        self.assertIn(("my-agent", "qwen"), backends)

    def test_a_declared_agent_that_is_not_connected_is_not_called(self):
        backends = writers.build_backends(settings(), ("my-agent", "qwen"), {},
                                           {"my-agent": {"command": ""}})
        with self.assertRaises(AgentUnavailable):
            backends[("my-agent", "qwen")]("明天交报告")

    def test_the_question_goes_into_the_prompt_when_the_command_has_one(self):
        command = "my-agent --model {model} --ask {prompt}"
        argv = shlex.split(command.replace("{model}", "qwen-id"))
        self.assertIn("{prompt}", argv)
        self.assertEqual([item.replace("{prompt}", "问题") for item in argv],
                         ["my-agent", "--model", "qwen-id", "--ask", "问题"])

    def test_the_question_goes_on_standard_input_when_the_command_has_none(self):
        command = "my-agent --model {model}"
        argv = shlex.split(command.replace("{model}", "qwen-id"))
        self.assertNotIn("{prompt}", argv)

    def test_a_route_that_no_backend_answers_is_refused(self):
        backends = writers.build_backends(settings())
        writers.ensure_route_usable(("direct", "qwen"), backends)
        with self.assertRaisesRegex(RouteError, "no workbuddy answers for qwen"):
            writers.ensure_route_usable(("workbuddy", "qwen"), backends)

    def test_the_note_names_the_route_that_wrote_it(self):
        seen = []

        def backend(text):
            seen.append(text)
            return "# 纪要\n\n## 待办事项\n\n- 明天交报告\n"

        note = route_summary("hermes", "minimax", "明天交报告", {("hermes", "minimax"): backend})
        self.assertEqual(seen, ["明天交报告"])
        self.assertTrue(note.startswith("Agent: hermes\nModel: minimax\n"))
        self.assertIn("明天交报告", note)


class ResolveRouteTest(unittest.TestCase):
    def test_the_command_line_wins_over_the_configuration(self):
        chosen = writers.resolve_route(settings(), "workbuddy", "glm", {"agent": "hermes", "model": "qwen"})
        self.assertEqual(chosen, ("workbuddy", "glm"))

    def test_the_configuration_wins_over_what_the_address_says(self):
        chosen = writers.resolve_route(settings(), "", "", {"agent": "hermes", "model": "qwen"})
        self.assertEqual(chosen, ("hermes", "qwen"))

    def test_an_agent_the_configuration_declares_is_allowed(self):
        chosen = writers.resolve_route(settings(), "my-agent", "qwen", {},
                                       {"my-agent": {"command": "my-agent"}})
        self.assertEqual(chosen, ("my-agent", "qwen"))

    def test_an_agent_nobody_declares_is_refused(self):
        with self.assertRaises(RouteError):
            writers.resolve_route(settings(), "my-agent", "qwen", {})

    def test_the_address_says_which_provider_it_is(self):
        chosen = writers.resolve_route(settings())
        self.assertEqual(chosen, ("direct", "qwen"))

    def test_a_known_provider_that_does_not_match_is_refused(self):
        with self.assertRaises(RouteError):
            writers.resolve_route(settings("https://api.minimax.cn/v1"), "direct", "qwen")
        with self.assertRaises(RouteError):
            writers.resolve_route(settings("https://maas.qianwenaiapi.com/compatible-mode/v1"), "direct", "minimax")

    def test_an_endpoint_of_nobody_known_takes_the_name_that_was_asked_for(self):
        chosen = writers.resolve_route(settings("http://127.0.0.1:8000/v1"), "direct", "qwen")
        self.assertEqual(chosen, ("direct", "qwen"))

    def test_the_direct_route_refuses_a_known_provider_that_does_not_match(self):
        backends = writers.build_backends(settings("https://api.minimax.cn/v1"))
        with self.assertRaises(RouteError):
            backends[("direct", "qwen")]("明天交报告")
        self.assertTrue(callable(backends[("direct", "minimax")]))


class CommandTest(unittest.TestCase):
    def test_hermes_is_asked_on_standard_input(self):
        with TemporaryDirectory() as root:
            fake = Path(root) / "hermes"
            fake.write_text("", encoding="utf-8")
            os.environ["HERMES_CLI"] = str(fake)
            try:
                command = writers.hermes_command("minimax-cn", "MiniMax-M3")
            finally:
                del os.environ["HERMES_CLI"]
        self.assertEqual(command[1:], ["chat", "--query-file", "-", "--oneshot", "-Q", "--format", "text",
                                       "--provider", "minimax-cn", "-m", "MiniMax-M3"])

    def test_hermes_without_a_provider_keeps_the_one_it_has(self):
        with TemporaryDirectory() as root:
            fake = Path(root) / "hermes"
            fake.write_text("", encoding="utf-8")
            os.environ["HERMES_CLI"] = str(fake)
            try:
                command = writers.hermes_command("", "MiniMax-M3")
            finally:
                del os.environ["HERMES_CLI"]
        self.assertNotIn("--provider", command)

    def test_workbuddy_is_looked_for_rather_than_assumed(self):
        with TemporaryDirectory() as root:
            fake = Path(root) / "codebuddy-lite-wb.mjs"
            fake.write_text("", encoding="utf-8")
            os.environ["WORKBUDDY_CLI"] = str(fake)
            try:
                command = writers.workbuddy_command("minimax-m2.7")
            finally:
                del os.environ["WORKBUDDY_CLI"]
        self.assertEqual(command[0], "node")
        self.assertEqual(command[1], str(fake))
        self.assertIn("--model", command)
        self.assertEqual(command[-1], "minimax-m2.7")

    def test_an_agent_that_is_not_connected_is_not_run(self):
        with self.assertRaises(AgentUnavailable):
            writers.run_agent("muse", "问题", "", "minimax")

    def test_an_agent_that_answers_nothing_says_why(self):
        with TemporaryDirectory() as root:
            script = Path(root) / "agent.sh"
            script.write_text("#!/bin/sh\necho 'Authentication required' >&2\n", encoding="utf-8")
            script.chmod(0o755)
            with self.assertRaisesRegex(RuntimeError, "Authentication required"):
                writers._run([str(script)], "问题")


if __name__ == "__main__":
    unittest.main()
