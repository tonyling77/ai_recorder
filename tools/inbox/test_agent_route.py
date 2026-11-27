import unittest

from agent_route import AgentUnavailable, RouteError, route_summary


class AgentRouteTest(unittest.TestCase):
    def test_hermes_minimax_names_the_selected_route(self):
        seen = []

        def backend(text):
            seen.append(text)
            return "# 纪要\n\n## 待办\n\n- 明天交报告\n"

        note = route_summary(
            "hermes",
            "minimax",
            "明天交报告",
            {("hermes", "minimax"): backend},
        )
        self.assertEqual(seen, ["明天交报告"])
        self.assertTrue(note.startswith("Agent: hermes\nModel: minimax\n"))
        self.assertIn("明天交报告", note)

    def test_deepseek_uses_its_own_backend_and_leaves_the_transcript_unchanged(self):
        seen = []

        def minimax(text):
            seen.append(("minimax", text))
            return "minimax note"

        def deepseek(text):
            seen.append(("deepseek", text))
            return "deepseek note"

        note = route_summary(
            "hermes",
            "deepseek",
            "明天交报告",
            {("hermes", "minimax"): minimax, ("hermes", "deepseek"): deepseek},
        )
        self.assertEqual(seen, [("deepseek", "明天交报告")])
        self.assertIn("Model: deepseek\n", note)
        self.assertNotIn("minimax note", note)

    def test_an_unconnected_agent_is_not_called(self):
        called = []

        def backend(text):
            called.append(text)
            return "should not run"

        with self.assertRaises(AgentUnavailable):
            route_summary(
                "muse",
                "minimax",
                "明天交报告",
                {("hermes", "minimax"): backend},
            )
        self.assertEqual(called, [])

    def test_an_unknown_model_is_rejected_before_any_backend(self):
        called = []

        def backend(text):
            called.append(text)
            return "should not run"

        with self.assertRaises(RouteError):
            route_summary(
                "hermes",
                "gpt",
                "明天交报告",
                {("hermes", "minimax"): backend},
            )
        self.assertEqual(called, [])


if __name__ == "__main__":
    unittest.main()
