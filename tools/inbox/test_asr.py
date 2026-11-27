import unittest

from asr import asr_endpoint, asr_model, transcriber
from model_api import ModelSettings


def settings(base_url, **kwargs):
    return ModelSettings(base_url=base_url, model="MiniMax-M3", api_key="secret", **kwargs)


class AsrChoiceTest(unittest.TestCase):
    def test_minimax_keeps_the_model_it_always_used(self):
        chosen = settings("https://api.minimax.cn/v1")
        self.assertEqual(asr_model(chosen), "asr-1.0")
        self.assertEqual(asr_endpoint(chosen), "https://api.minimax.cn/v1")

    def test_qwen_names_the_model_that_transcribes(self):
        chosen = settings("https://maas.qianwenaiapi.com/compatible-mode/v1")
        self.assertEqual(asr_model(chosen), "qwen3-asr-flash")

    def test_the_configuration_wins_over_what_the_address_says(self):
        chosen = settings(
            "https://maas.qianwenaiapi.com/compatible-mode/v1",
            asr_model="qwen-audio-3.1-asr-flash",
        )
        self.assertEqual(asr_model(chosen), "qwen-audio-3.1-asr-flash")

    def test_the_transcription_may_sit_on_another_endpoint(self):
        chosen = settings(
            "https://maas.qianwenaiapi.com/compatible-mode/v1",
            asr_model="qwen3-asr-flash",
            asr_base_url="https://api.minimax.cn/v1",
        )
        self.assertEqual(asr_endpoint(chosen), "https://api.minimax.cn/v1")

    def test_a_provider_that_is_not_known_is_asked_to_name_its_model(self):
        chosen = settings("http://127.0.0.1:8000/v1")
        with self.assertRaisesRegex(ValueError, "asr"):
            asr_model(chosen)

    def test_the_transcriber_of_minimax_calls_its_own_endpoint(self):
        chosen = settings("https://api.minimax.cn/v1")
        transcribe = transcriber(chosen)
        self.assertTrue(callable(transcribe))


if __name__ == "__main__":
    unittest.main()
