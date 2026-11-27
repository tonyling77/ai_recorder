import base64
import io
import json
import unittest
import urllib.error
from pathlib import Path
from tempfile import TemporaryDirectory

from model_api import (
    candidate_roots,
    chat_completion,
    provider_family,
    read_agents,
    read_model_settings,
    read_route,
    read_section,
    render_summary,
    summary_prompt,
    transcribe_openai_audio,
)


class Response:
    def __init__(self, payload):
        self.payload = json.dumps(payload).encode()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def read(self):
        return self.payload


def answer(content):
    return Response({"choices": [{"message": {"content": content}}]})


class SettingsTest(unittest.TestCase):
    def test_settings_come_from_the_model_section(self):
        with TemporaryDirectory() as root:
            path = Path(root) / "config.yaml"
            path.write_text(
                "\n".join(
                    [
                        "model:",
                        "  base_url: https://api.minimax.cn/v1",
                        "  default: MiniMax-M3",
                        "  api_key: super-secret",
                        "providers:",
                        "  api_key: other-secret",
                    ]
                ),
                encoding="utf-8",
            )
            settings = read_model_settings(path)
            self.assertEqual(settings.base_url, "https://api.minimax.cn/v1")
            self.assertEqual(settings.model, "MiniMax-M3")
            self.assertEqual(settings.api_key, "super-secret")
            self.assertEqual(
                candidate_roots(settings.base_url),
                ["https://api.minimax.cn/v1", "https://api.minimax.io/v1"],
            )

    def test_the_key_of_another_section_never_reaches_a_request(self):
        with TemporaryDirectory() as root:
            path = Path(root) / "config.yaml"
            path.write_text(
                "\n".join(
                    [
                        "providers:",
                        "  api_key: other-secret",
                        "model:",
                        "  base_url: https://api.minimax.cn/v1",
                        "  default: MiniMax-M3",
                        "  api_key: super-secret",
                    ]
                ),
                encoding="utf-8",
            )
            settings = read_model_settings(path)
            self.assertEqual(settings.api_key, "super-secret")

    def test_the_transcription_may_have_its_own_model_and_endpoint(self):
        with TemporaryDirectory() as root:
            path = Path(root) / "config.yaml"
            path.write_text(
                "\n".join(
                    [
                        "model:",
                        "  base_url: https://maas.qianwenaiapi.com/compatible-mode/v1",
                        "  default: qwen-turbo",
                        "  api_key: super-secret",
                        "  asr: qwen3-asr-flash",
                        "  asr_base_url: https://other.example/v1",
                        "  asr_api_key: other-secret",
                    ]
                ),
                encoding="utf-8",
            )
            settings = read_model_settings(path)
            self.assertEqual(settings.asr_model, "qwen3-asr-flash")
            self.assertEqual(settings.asr_base_url, "https://other.example/v1")
            self.assertEqual(settings.asr_api_key, "other-secret")

    def test_the_route_is_read_from_its_own_section(self):
        with TemporaryDirectory() as root:
            path = Path(root) / "config.yaml"
            path.write_text(
                "\n".join(
                    [
                        "model:",
                        "  base_url: https://maas.qianwenaiapi.com/compatible-mode/v1",
                        "  default: qwen-turbo",
                        "  api_key: super-secret",
                        "route:",
                        "  agent: workbuddy",
                        "  model: qwen",
                    ]
                ),
                encoding="utf-8",
            )
            self.assertEqual(read_route(path), {"agent": "workbuddy", "model": "qwen"})
            self.assertEqual(read_section(path, "route")["agent"], "workbuddy")

    def test_a_value_that_is_still_the_example_is_named(self):
        with TemporaryDirectory() as root:
            path = Path(root) / "config.yaml"
            path.write_text(
                "\n".join(
                    [
                        "model:",
                        "  base_url: <你的 OpenAI 兼容接口地址>",
                        "  default: qwen-turbo",
                        "  api_key: sk-x",
                    ]
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "base_url"):
                read_model_settings(path)

    def test_the_provider_of_an_address_is_named(self):
        self.assertEqual(provider_family("https://maas.qianwenaiapi.com/compatible-mode/v1"), "qwen")
        self.assertEqual(provider_family("https://api.minimax.cn/v1"), "minimax")
        self.assertEqual(provider_family("https://open.bigmodel.cn/api/paas/v4/"), "glm")
        self.assertEqual(provider_family("https://api.deepseek.com"), "deepseek")
        self.assertEqual(provider_family("https://ark.cn-beijing.volces.com/api/v3"), "doubao")
        self.assertEqual(provider_family("https://api.moonshot.cn/v1"), "kimi")
        self.assertEqual(provider_family("http://127.0.0.1:8000/v1"), "")

    def test_the_agents_the_configuration_declares_are_read(self):
        with TemporaryDirectory() as root:
            path = Path(root) / "config.yaml"
            path.write_text(
                "\n".join(
                    [
                        "model:",
                        "  base_url: https://maas.qianwenaiapi.com/compatible-mode/v1",
                        "  default: qwen-turbo",
                        "  api_key: super-secret",
                        "agents:",
                        "  my-agent:",
                        "    command: my-agent --model {model}",
                        "  other:",
                        "    command: other --stdin",
                    ]
                ),
                encoding="utf-8",
            )
            agents = read_agents(path)
            self.assertEqual(agents["my-agent"]["command"], "my-agent --model {model}")
            self.assertEqual(agents["other"]["command"], "other --stdin")
            self.assertNotIn("model", agents)


class SummaryTest(unittest.TestCase):
    def test_summary_asks_the_chat_model_for_a_note(self):
        def opener(request):
            self.assertEqual(request.full_url, "https://api.minimax.cn/v1/chat/completions")
            body = json.loads(request.data.decode())
            self.assertEqual(body["model"], "MiniMax-M3")
            self.assertIn("明天交报告", body["messages"][0]["content"])
            content = {
                "title": "项目会议",
                "conclusions": ["报告需要提交"],
                "todos": [{"item": "提交报告", "evidence": "明天交报告"}],
                "quotes": ["明天交报告"],
            }
            return answer(json.dumps(content, ensure_ascii=False))

        content = chat_completion(
            summary_prompt("明天交报告"),
            "super-secret",
            "MiniMax-M3",
            ["https://api.minimax.cn/v1"],
            opener,
        )
        note = render_summary(content, "明天交报告")
        self.assertIn("# 项目会议", note)
        self.assertIn("原句：明天交报告", note)

    def test_summary_removes_model_thinking(self):
        content = {
            "title": "测试",
            "conclusions": ["完成测试"],
            "todos": [],
            "quotes": ["一二三"],
        }
        raw = "<think>不应保存的推理</think>\n```json\n" + json.dumps(content, ensure_ascii=False) + "\n```"
        note = render_summary(raw, "一二三")
        self.assertNotIn("think", note.lower())
        self.assertNotIn("不应保存的推理", note)
        self.assertIn("# 测试", note)

    def test_summary_rejects_todo_without_exact_evidence(self):
        content = {
            "title": "测试",
            "conclusions": [],
            "todos": [{"item": "明天发邮件", "evidence": "明天发邮件"}],
            "quotes": [],
        }
        raw = json.dumps(content, ensure_ascii=False)
        with self.assertRaisesRegex(RuntimeError, "todo evidence"):
            render_summary(raw, "今天讨论天气")

    def test_the_json_of_an_agent_is_found_inside_its_prose(self):
        content = {
            "title": "周会",
            "conclusions": ["进度正常"],
            "todos": [{"item": "发周报", "evidence": "周五之前发周报"}],
            "quotes": ["周五之前发周报"],
        }
        raw = "好的，这是纪要：\n```json\n" + json.dumps(content, ensure_ascii=False) + "\n```\n希望有用。"
        note = render_summary(raw, "周五之前发周报")
        self.assertIn("# 周会", note)
        self.assertIn("原句：周五之前发周报", note)

    def test_the_punctuation_of_another_model_does_not_break_the_check(self):
        content = {
            "title": "周会",
            "conclusions": [],
            "todos": [{"item": "发周报", "evidence": "周五之前发周报。"}],
            "quotes": [],
        }
        raw = json.dumps(content, ensure_ascii=False)
        note = render_summary(raw, "周五之前发周报")
        self.assertIn("发周报", note)

    def test_an_answer_without_a_summary_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "invalid JSON"):
            render_summary("我不知道", "明天交报告")


class OpenAiTranscriptionTest(unittest.TestCase):
    def test_the_audio_goes_in_as_a_data_uri(self):
        seen = []

        def opener(request):
            seen.append(request.full_url)
            body = json.loads(request.data.decode())
            self.assertEqual(body["model"], "qwen3-asr-flash")
            content = body["messages"][0]["content"]
            self.assertEqual(content[0]["type"], "input_audio")
            data = content[0]["input_audio"]["data"]
            self.assertTrue(data.startswith("data:audio/ogg;base64,"))
            self.assertEqual(
                base64.b64decode(data.split(",", 1)[1]),
                b"OggS-slice",
            )
            return answer("明天交报告")

        text = transcribe_openai_audio(
            b"OggS-slice",
            "0.ogg",
            "qwen3-asr-flash",
            "super-secret",
            ["https://maas.qianwenaiapi.com/compatible-mode/v1"],
            opener,
        )
        self.assertEqual(text, "明天交报告")
        self.assertEqual(
            seen,
            ["https://maas.qianwenaiapi.com/compatible-mode/v1/chat/completions"],
        )

    def test_the_question_goes_in_front_of_the_audio_when_there_is_one(self):
        seen = {}

        def opener(request):
            body = json.loads(request.data.decode())
            seen["content"] = body["messages"][0]["content"]
            return answer("明天交报告")

        transcribe_openai_audio(
            b"OggS", "0.ogg", "qwen3.8-omni-flash", "k",
            ["https://maas.qianwenaiapi.com/compatible-mode/v1"], opener, "逐字转写",
        )
        self.assertEqual(seen["content"][0]["type"], "text")
        self.assertEqual(seen["content"][0]["text"], "逐字转写")
        self.assertEqual(seen["content"][1]["type"], "input_audio")

    def test_no_question_means_the_audio_alone(self):
        seen = {}

        def opener(request):
            body = json.loads(request.data.decode())
            seen["content"] = body["messages"][0]["content"]
            return answer("明天交报告")

        transcribe_openai_audio(
            b"OggS", "0.ogg", "qwen3-asr-flash", "k",
            ["https://maas.qianwenaiapi.com/compatible-mode/v1"], opener,
        )
        self.assertEqual(len(seen["content"]), 1)
        self.assertEqual(seen["content"][0]["type"], "input_audio")

    def test_a_rejected_transcription_does_not_reveal_the_key(self):
        def opener(request):
            raise urllib.error.HTTPError(request.full_url, 401, "no", hdrs=None, fp=io.BytesIO(b"super-secret"))

        with self.assertRaises(RuntimeError) as caught:
            transcribe_openai_audio(
                b"OggS", "0.ogg", "qwen3-asr-flash", "super-secret",
                ["https://maas.qianwenaiapi.com/compatible-mode/v1"], opener,
            )
        self.assertNotIn("super-secret", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
