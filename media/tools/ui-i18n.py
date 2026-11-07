#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Language packs for the admin UI.

English is in media/ui/index.html and media/ui/app.js. A translator edits
media/ui/i18n/<code>.json, which maps English to the translation. This script
writes media/ui/lang-<code>.js, which maps a hash of the English to the
translation, so the browser downloads no English it does not need and a reworded
sentence stops matching: it shows in English until someone translates it again.

  ui-i18n.py          rewrite the packs
  ui-i18n.py --check  fail if a pack is stale, a string has no translation, or a
                      translation drops or invents a {placeholder}
  ui-i18n.py --list   print every string of the UI, as JSON
  ui-i18n.py --pseudo DIR  copy the UI to DIR with every string marked, as lang-es.js
A plural is {"one": ..., "few": ..., "other": ...} under the English plural.
"""
import argparse, html.parser, json, pathlib, re, shutil, sys

HERE = pathlib.Path(__file__).resolve().parent.parent / "ui"
LANGS = ["de", "es", "fr", "pt-BR", "ru", "zh-CN"]
LITERAL = r"'((?:[^'\\]|\\.)*)'"


def ikey(text):
    """FNV-1a over UTF-16 units, base 36: the same as ikey() in app.js."""
    x = 2166136261
    data = text.encode("utf-16-le")
    for i in range(0, len(data), 2):
        x = ((x ^ int.from_bytes(data[i:i + 2], "little")) * 16777619) & 0xFFFFFFFF
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    out = ""
    while True:
        x, r = divmod(x, 36)
        out = digits[r] + out
        if not x:
            return out


def unescape(s):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), s)


class Page(html.parser.HTMLParser):
    """What walk() in app.js translates: text and four attributes, outside svg, code, pre and .mono."""
    VOID = {"meta", "link", "input", "br", "img", "path", "use", "circle"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack, self.found = [], set()

    def skipping(self):
        return any(self.stack)

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        skip = tag in ("svg", "code", "pre", "script", "style", "title", "head") or "mono" in (a.get("class") or "").split()
        if tag not in self.VOID:
            self.stack.append(skip)
        if not self.skipping() and not skip:
            for k in ("aria-label", "placeholder", "data-t", "data-l"):
                if a.get(k):
                    self.found.add(a[k])

    def handle_endtag(self, tag):
        if tag not in self.VOID and self.stack:
            self.stack.pop()

    def handle_data(self, data):
        if not self.skipping() and data.strip():
            self.found.add(data.strip())


def catalog():
    """(singles, plurals): plurals maps the English plural to its singular."""
    page = Page()
    page.feed((HERE / "index.html").read_text(encoding="utf-8"))
    js = (HERE / "app.js").read_text(encoding="utf-8")
    js = js[js.index("function main()"):]  # above it is the runtime itself
    singles = set(page.found)
    singles |= {unescape(m) for m in re.findall(r"\b_\(\s*" + LITERAL, js)}
    plurals = {}
    for one, other in re.findall(r"\b_n\([^'\n]*?,\s*" + LITERAL + r",\s*" + LITERAL, js):
        plurals[unescape(other)] = unescape(one)
    if re.search(r"\b_\(\s*[^'\s]", js):
        sys.exit("ui-i18n: _() takes a literal string, or this script cannot see it: " + re.search(r"\b_\(\s*[^'\s].{0,40}", js).group(0))
    return singles, plurals


def has_words(t):
    return re.search(r"[^\W\d_]{2}", t) is not None


def pack(code, singles, plurals):
    """(text of the pack, missing, problems)."""
    source = HERE / "i18n" / f"{code}.json"
    tr = json.loads(source.read_text(encoding="utf-8")) if source.exists() else {}
    out, missing, problems, seen = {}, [], [], {}
    for en in sorted(singles | set(plurals)):
        k = ikey(en)
        if k in seen and seen[k] != en:
            problems.append(f"hash collision: {en!r} and {seen[k]!r}")
        seen[k] = en
        t = tr.get(en)
        if t is None:
            if has_words(en):
                missing.append(en)
            continue
        want = set(re.findall(r"\{(\w+)\}", en))
        for form in t.values() if isinstance(t, dict) else [t]:
            if set(re.findall(r"\{(\w+)\}", form)) != want:
                problems.append(f"{code}: placeholders differ: {en!r} -> {form!r}")
        if isinstance(t, dict) and "other" not in t:
            problems.append(f"{code}: a plural needs 'other': {en!r}")
        if t != en:
            out[k] = t
    for en in tr:
        if en not in singles and en not in plurals:
            problems.append(f"{code}: no longer in the UI: {en!r}")
    body = json.dumps(out, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return f"self.JIPC_PACK={body};\n", missing, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--pseudo", metavar="DIR")
    args = ap.parse_args()
    singles, plurals = catalog()
    if args.list:
        print(json.dumps({"singles": sorted(singles), "plurals": plurals}, ensure_ascii=False, indent=0))
        return
    if args.pseudo:
        out = pathlib.Path(args.pseudo)
        out.mkdir(parents=True, exist_ok=True)
        for p in HERE.iterdir():
            if p.is_file():
                shutil.copy(p, out / p.name)
        marked = {ikey(e): f"⟦{e}⟧" for e in singles}
        marked.update({ikey(e): {"other": f"⟦{e}⟧"} for e in plurals})
        (out / "lang-es.js").write_text("self.JIPC_PACK=" + json.dumps(marked, ensure_ascii=False) + ";\n", encoding="utf-8")
        return
    failed = []
    for code in LANGS:
        text, missing, problems = pack(code, singles, plurals)
        failed += problems
        path = HERE / f"lang-{code}.js"
        if args.check:
            if missing:
                failed.append(f"{code}: {len(missing)} strings have no translation, for example {missing[0]!r}")
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                failed.append(f"{code}: {path.name} is stale; run tools/ui-i18n.py")
        else:
            path.write_text(text, encoding="utf-8")
            print(f"{path.name}: {len(text.encode())} bytes, {len(missing)} missing")
    if failed:
        sys.exit("ui-i18n: FAIL:\n  " + "\n  ".join(failed))
    if args.check:
        print(f"ui-i18n: PASS ({len(singles) + len(plurals)} strings, {len(LANGS)} languages)")


if __name__ == "__main__":
    main()
