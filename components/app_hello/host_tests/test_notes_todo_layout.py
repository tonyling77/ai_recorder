import json
import unittest
from pathlib import Path


COMPONENT = Path(__file__).parent.parent
NOTES_SCENE = COMPONENT / "ui" / "notes.json"
APP_HELLO_SOURCE = COMPONENT / "app_hello.c"

# The screen of the board.
SCREEN_HEIGHT = 480
# The line height of the text in the notes page, from the size of the font
# and the space between the lines.
LINE_HEIGHT = 24
# How many to-dos a card can hold. The board keeps four.
TODOS_PER_CARD = 4


def _scene() -> dict:
    return json.loads(NOTES_SCENE.read_text(encoding="utf-8"))


def _object(scene: dict, name: str) -> dict:
    for entry in scene["objects"]:
        if entry.get("name") == name:
            return entry
    raise AssertionError(f"the scene has no object named {name}")


def _source() -> str:
    return APP_HELLO_SOURCE.read_text(encoding="utf-8")


class NotesTodoLayoutTest(unittest.TestCase):
    """Every to-do of a card has to be readable.

    The card of a meeting can carry four to-dos, and a to-do that is too
    long for one line takes two. The box of the to-dos held two lines, so
    the rest of the to-dos could not be read at all. The to-dos have a page
    of their own now, and the box is as tall as the page.
    """

    def test_the_box_of_the_to_dos_holds_every_to_do(self):
        scene = _scene()
        label = _object(scene, "notes_detail_todos")
        height = int(label["h"])
        self.assertGreaterEqual(
            height,
            LINE_HEIGHT * TODOS_PER_CARD,
            "the box of the to-dos must hold every to-do of a card",
        )

    def test_the_box_of_the_to_dos_is_inside_its_card(self):
        scene = _scene()
        card = _object(scene, "notes_todo_card")
        label = _object(scene, "notes_detail_todos")
        self.assertGreaterEqual(int(card["h"]), int(label["y"]) + int(label["h"]),
                                "the to-dos must stay inside the card")
        self.assertGreaterEqual(int(label["x"]), 0)
        self.assertLessEqual(int(label["x"]) + int(label["w"]), int(card["w"]))

    def test_the_to_do_page_fits_on_the_screen(self):
        """The swipe of the board is sideways, so there is no scrolling.

        Everything of the page has to fit, or the to-dos that are at the
        bottom of the page can never be read.
        """
        scene = _scene()
        card = _object(scene, "notes_todo_card")
        top = int(card["y"])
        bottom = top + int(card["h"])
        self.assertGreaterEqual(top, 0)
        self.assertLessEqual(bottom, SCREEN_HEIGHT,
                             "the box of the to-dos must fit on the screen")

    def test_the_card_and_the_to_dos_do_not_share_the_page(self):
        """The conclusion and the to-dos have a page each.

        The box of the to-dos takes the whole screen of its page, so the
        conclusion cannot be on that page. The pages are separate, so the
        boxes do not have to fit on the screen together.
        """
        source = _source()
        self.assertIn("state->notes_step == 6", source)
        self.assertIn("state->notes_step == 7", source)
        conclusion = source.split("state->notes_step == 6", 1)[1].split("state->notes_step == 7", 1)[0]
        todos = source.split("state->notes_step == 7", 1)[1].split("} else {", 1)[0]
        self.assertIn("NOTES_CONCLUSION_CARD", conclusion)
        self.assertIn("NOTES_TODO_CARD", todos)
        # The to-do page hides the conclusion, and the conclusion page hides
        # the to-dos, so the two boxes never fight for the same place.
        self.assertIn("NOTES_CONCLUSION_CARD, false", todos)
        self.assertIn("NOTES_TODO_CARD, false", conclusion)

    def test_the_button_that_removes_the_card_is_on_the_conclusion_page(self):
        """The button is about the card, not about the to-dos."""
        source = _source()
        conclusion = source.split("state->notes_step == 6", 1)[1].split("state->notes_step == 7", 1)[0]
        todos = source.split("state->notes_step == 7", 1)[1].split("} else {", 1)[0]
        self.assertIn("NOTES_REMOVE, true", conclusion)
        self.assertIn("NOTES_REMOVE, false", todos)

    def test_the_detail_stays_on_the_screen_while_the_card_is_open(self):
        """The card is open, so the detail has to be on the screen.

        The list hides the rows of the list while a card is open, because
        the detail covers them. A detail that was pushed off the screen
        left the page with four empty rows on it and nothing to read.
        """
        source = _source()
        closing = source.split("/* The list starts over here.", 1)[1]
        self.assertIn("if (!open && state->notes_detail_shown)", closing,
                      "the detail goes away only when the card is closed")
        self.assertNotIn("if (state->notes_detail_shown) {", closing,
                         "the detail must not be pushed off the screen while "
                         "the card is open")


if __name__ == "__main__":
    unittest.main()
