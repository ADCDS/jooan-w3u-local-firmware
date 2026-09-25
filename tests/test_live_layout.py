"""Keep the live controls and NVR details in their intended sections."""

from html.parser import HTMLParser
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VOID = {"meta", "link", "input"}


class Elements(HTMLParser):
    def __init__(self):
        super().__init__()
        self.root = {"tag": "root", "attrs": {}, "children": [], "text": ""}
        self.stack = [self.root]

    def handle_starttag(self, tag, attrs):
        node = {"tag": tag, "attrs": dict(attrs), "children": [], "text": ""}
        self.stack[-1]["children"].append(node)
        if tag not in VOID:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        self.stack.pop()

    def handle_endtag(self, tag):
        if tag not in VOID:
            assert self.stack[-1]["tag"] == tag, f"unexpected </{tag}>"
            self.stack.pop()

    def handle_data(self, data):
        self.stack[-1]["text"] += data


def walk(node):
    yield node
    for child in node["children"]:
        yield from walk(child)


def by_id(root, name):
    matches = [node for node in walk(root) if node["attrs"].get("id") == name]
    assert len(matches) == 1, f"expected one #{name}, got {len(matches)}"
    return matches[0]


def cards(node):
    return [child for child in node["children"]
            if "card" in child["attrs"].get("class", "").split()]


def heading(card):
    return next(node["text"].strip() for node in walk(card) if node["tag"] == "h2")


class LiveLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tree = Elements()
        tree.feed((ROOT / "web/index.html").read_text(encoding="utf-8"))
        assert tree.stack == [tree.root], "unclosed HTML elements"
        cls.root = tree.root

    def test_presets_follow_jog_in_live_sidebar(self):
        live = by_id(self.root, "zone-live")
        side = next(node for node in live["children"] if node["tag"] == "aside")
        self.assertEqual([heading(card) for card in cards(side)],
                         ["Jog", "Presets", "Talkback"])
        preset = cards(side)[1]
        for name in ("preset-chips", "preset-set", "preset-delete", "preset-confirm"):
            self.assertIn(by_id(self.root, name), list(walk(preset)))
        self.assertNotIn("Presets", [heading(card) for card in cards(by_id(self.root, "zone-control"))])

    def test_rtsp_is_visible_in_system_on_phone(self):
        system = by_id(self.root, "zone-system")
        rtsp = next(card for card in cards(system) if heading(card) == "RTSP")
        self.assertIn(by_id(self.root, "streams"), list(walk(rtsp)))
        self.assertNotIn("desktop-only", rtsp["attrs"].get("class", "").split())
        css = (ROOT / "web/styles.css").read_text(encoding="utf-8")
        self.assertNotIn(".desktop-only{display:none}", css)
        self.assertIn(".side .grid2{grid-template-columns:1fr}",
                      css.split("/* ---------- phone ---------- */", 1)[1])

    def test_both_live_tiles_remain_focusable(self):
        live = by_id(self.root, "zone-live")
        for name in ("main", "sub"):
            tile = by_id(self.root, f"tile-{name}")
            self.assertIn(tile, list(walk(live)))
            self.assertEqual(tile["attrs"]["tabindex"], "0")
            self.assertEqual(tile["attrs"]["role"], "button")
            self.assertIn(by_id(self.root, f"video-{name}"), list(walk(tile)))


if __name__ == "__main__":
    unittest.main()
