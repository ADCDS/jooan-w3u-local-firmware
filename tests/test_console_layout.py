"""The Web UI is a platform console: the media daemon serves NVRs."""

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


class ConsoleLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tree = Elements()
        tree.feed((ROOT / "web/index.html").read_text(encoding="utf-8"))
        assert tree.stack == [tree.root], "unclosed HTML elements"
        cls.root = tree.root

    def test_only_maintenance_zones_remain(self):
        zones = [node["attrs"]["id"] for node in walk(self.root)
                 if "zone" in node["attrs"].get("class", "").split()]
        self.assertEqual(zones, ["zone-network", "zone-system"])
        rail = [node["attrs"]["data-zone"] for node in walk(by_id(self.root, "rail"))
                if node["tag"] == "button"]
        self.assertEqual(sorted(rail), ["network", "system"])
        self.assertEqual(by_id(self.root, "console")["attrs"]["data-zone"], "system")

    def test_media_belongs_to_the_media_daemon(self):
        system = by_id(self.root, "zone-system")
        media = next(card for card in cards(system) if heading(card) == "Media")
        self.assertNotIn("desktop-only", media["attrs"].get("class", "").split())
        app = (ROOT / "web/app.js").read_text(encoding="utf-8")
        for gone in ("/onvif/", "/api/v1/streams", "/api/v1/system/audio-pause"):
            self.assertNotIn(gone, app)

    def test_network_time_is_configurable_in_system(self):
        system = by_id(self.root, "zone-system")
        clock = next(card for card in cards(system) if heading(card) == "Camera time")
        ids = {node["attrs"].get("id") for node in walk(clock)}
        self.assertTrue({"ntp-state", "ntp-server", "ntp-save"} <= ids)

    def test_no_media_client_is_shipped(self):
        app = (ROOT / "web/app.js").read_text(encoding="utf-8")
        self.assertNotIn("import ", app)
        for gone in ("/api/v1/video/", "/api/v1/audio/", "/api/v1/ptz/", "/api/v1/recordings/"):
            self.assertNotIn(gone, app)
        self.assertEqual(sorted(path.name for path in (ROOT / "web").iterdir() if not path.name.endswith(".test.js")),
                         ["app.js", "index.html", "styles.css"])


if __name__ == "__main__":
    unittest.main()
