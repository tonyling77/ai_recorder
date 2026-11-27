import unittest

from device_note import card_from_summary


class DeviceNoteTest(unittest.TestCase):
    def test_summary_becomes_a_short_card(self):
        markdown = "\n".join([
            "# 产品评审会",
            "",
            "## 讨论结论",
            "",
            "- 纪要放到设备上阅读。",
            "",
            "## 待办事项",
            "",
            "- 确认左滑进入纪要",
            "  - 原句：请确认左滑进入纪要",
            "- 纪要写入 Obsidian 后下发",
            "  - 原句：写入 Obsidian 后再下发",
            "",
            "## 原话摘录",
            "",
            "> 请确认左滑进入纪要",
        ])
        card = card_from_summary(markdown, "board-1", "2026-11-11 15:20")
        self.assertIn("id board-1\n", card)
        self.assertIn("issued 2026-11-11 15:20\n", card)
        self.assertIn("title 产品评审会\n", card)
        self.assertIn("conclusion 纪要放到设备上阅读。\n", card)
        self.assertIn("todo 确认左滑进入纪要\n", card)
        self.assertIn("todo 纪要写入 Obsidian 后下发\n", card)
        self.assertNotIn("原句", card)
        self.assertNotIn("原话", card)


if __name__ == "__main__":
    unittest.main()
