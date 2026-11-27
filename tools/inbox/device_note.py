"""Short meeting card delivered to the recorder.

The full summary stays in Obsidian. The device receives the title, conclusions,
and todo lines only.
"""

from __future__ import annotations


def card_from_summary(markdown: str, note_id: str, issued: str) -> str:
    title = "纪要"
    conclusions: list[str] = []
    todos: list[str] = []
    section = ""
    for raw in markdown.splitlines():
        line = raw.strip()
        if line.startswith("# "):
            title = line[2:].strip() or title
            continue
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        if not line.startswith("- ") or "原句" in line or "原话" in line:
            continue
        item = line[2:].strip()
        if not item:
            continue
        if section.startswith("讨论结论"):
            conclusions.append(item)
        elif section.startswith("待办"):
            todos.append(item)
    lines = [
        f"id {note_id}",
        f"issued {issued}",
        f"title {title}",
        "conclusion " + " ".join(conclusions),
    ]
    lines.extend(f"todo {item}" for item in todos[:4])
    return "\n".join(lines) + "\n"
