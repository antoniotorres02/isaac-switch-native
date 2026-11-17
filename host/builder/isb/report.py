"""The text behind “Copy report”: enough to help someone, with their home as ~."""
from __future__ import annotations

from . import VERSION, system
from .checks import Item
from .texts import t

MARK = {"ok": "✔", "warn": "⚠", "fail": "✘"}


def item_line(item: Item) -> str:
    return "%s %s" % (MARK.get(item.state, "·"), t(item.key, **item.args))


def build(sections: dict[str, list[str]]) -> str:
    lines = ["Isaac Switch Builder %s" % VERSION, system.describe_system(), ""]
    for title, body in sections.items():
        if body:
            lines.append("## " + title)
            lines.extend(body)
            lines.append("")
    return system.redact("\n".join(lines))
