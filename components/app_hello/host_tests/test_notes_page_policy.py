import unittest
from pathlib import Path


COMPONENT = Path(__file__).parent.parent
APP_HELLO_SOURCE = COMPONENT / "app_hello.c"


def _source() -> str:
    return APP_HELLO_SOURCE.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class NotesPageCyclePolicyTest(unittest.TestCase):
    """The page of the minutes is painted in steps, and every step counts.

    The page is painted one step per tick, so the whole page takes a few
    ticks. The steps belong to one version of the minutes, and the version
    is marked by a counter that every change bumps.

    The counter and the step were not changed together. A cycle that was
    cut short by leaving the page resumed in the middle, so the list of the
    minutes was never painted again, and the page came back empty after a
    card was closed. Every change of the minutes now starts the page from
    the top.
    """

    def _helper(self) -> str:
        source = _source()
        self.assertIn("static void bump_notes(struct app_hello *state)", source,
                      "the minutes must be marked as changed in one place")
        return _body(source, "static void bump_notes(struct app_hello *state)\n{", "\n}")

    def test_the_step_is_restarted_when_the_minutes_change(self):
        """A cycle that was cut short must not resume in the middle."""
        helper = self._helper()
        self.assertIn("state->notes_step", helper)

    def test_an_open_card_skips_the_list(self):
        """A card that is open shows its own pages.

        The list is four steps of painting, and a card that is open covers
        it, so the page starts at the conclusion instead of walking the
        list first.
        """
        helper = self._helper()
        self.assertIn("meeting_notes_is_open", helper)
        self.assertIn("6", helper)

    def test_no_change_of_the_minutes_bypasses_the_helper(self):
        """Every change of the minutes goes through the helper.

        A bare bump of the counter leaves the step where it was, and that
        is the bug this guards against.
        """
        source = _source()
        helper = self._helper()
        outside = source.replace(helper, "")
        self.assertNotIn("notes_revision++", outside,
                         "the minutes must only be changed by bump_notes")
        self.assertEqual(source.count("bump_notes(state);"), 7,
                         "every place that changes the minutes must use the helper")


if __name__ == "__main__":
    unittest.main()
