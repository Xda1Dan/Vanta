#!/usr/bin/env python3
"""
Generates the Roblox side of the editor's completion engine -
gui/Resources/monaco/api.js and api-docs.js - from the Roblox API Reference,
robloxapi.github.io/ref, mirrored under build/api-cache/ref by
tools/fetch-ref.py. That site is the source of truth here: what it lists, the
editor lists; what it says about a name, the editor says.

The site is built from two things, and so is this:

  1. Roblox's *full* API dump for the build the site documents,
     setup.rbxcdn.com/<version>-Full-API-Dump.json - every class, member,
     enum and item, the hidden and RobloxSecurity ones included, with exact
     types, parameters, defaults, tags and security. The site's member set is
     this file exactly (less four entries with a space in the name, which it
     drops too), which was checked page by page before this was written.
  2. The site's own pages, for the words: a class's summary and description,
     every member's description, every enum's, every item's. The site renders
     these from Roblox's creator-docs, and reading the rendered page rather
     than the docs repository is what makes the two agree.

Two things the site has no pages for still come from creator-docs
(github.com/Roblox/creator-docs, content/en-us/reference/engine): the
datatypes (Vector3, CFrame, ...) with their constructors and methods, the Luau
libraries (math, string, task, ...) and the globals - and, as a supplement the
site does not render, the *reason* a member or item is deprecated. Two files
of this repository's own cover what neither has: tools/engine-tables.json is
what a live client answered when asked what it has (Vector3.max, SharedTable,
the capitalised legacy globals) and tools/engine-docs.json says what each of
those means, by hand.

The output is two files. api.js is one JSON object on globalThis.NEXA_API:
every name, its signature, and a one-line summary - what a suggestion list
and a parameter hint show. api-docs.js is globalThis.NEXA_API_DOCS, the full
description of everything that has one, keyed `Players.GetPlayers`, loaded
after the editor is up and read by the hover. Nothing is dropped.

A member the site describes with nothing is still described - from what the
dump does say about it: its kind, its owner, its tags and the security it
sits behind - and says that Roblox documents nothing more. That sentence is
written by the resolver, from the row, so it is never out of date.

    python3 tools/gen-api.py            # from build/api-cache, fetching what is missing
    python3 tools/gen-api.py --fetch    # refresh everything from the network first
    python3 tools/gen-api.py --offline  # never touch the network; fail if the cache is short

The cache (build/api-cache) is not committed; api.js and api-docs.js are,
like globals.js.
"""

import importlib.util
import json
import re
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "build" / "api-cache"
DOCS = CACHE / "docs"
SITE = CACHE / "ref"
FULL_DUMP = CACHE / "Full-API-Dump.json"
FULL_DUMP_VERSION = CACHE / "Full-API-Dump.version"
TARGET = ROOT / "gui" / "Resources" / "monaco" / "api.js"
TARGET_DOCS = ROOT / "gui" / "Resources" / "monaco" / "api-docs.js"
GLOBALS_JS = ROOT / "gui" / "Resources" / "monaco" / "globals.js"
FETCH_REF = ROOT / "tools" / "fetch-ref.py"

FULL_DUMP_URL = "https://setup.rbxcdn.com/{version}-Full-API-Dump.json"
DOCS_API = "https://api.github.com/repos/Roblox/creator-docs/contents/content/en-us/reference/engine/{section}"
DOCS_RAW = "https://raw.githubusercontent.com/Roblox/creator-docs/main/content/en-us/reference/engine/{section}/{name}"
SECTIONS = ["classes", "datatypes", "libraries", "globals", "enums"]

SUMMARY_MAX = 140
DESCRIPTION_MAX = 1600


# --------------------------------------------------------------------- fetching

def fetch(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "nexa-gen-api", "Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def fetch_ref_module():
    spec = importlib.util.spec_from_file_location("fetch_ref", FETCH_REF)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def site_version():
    """The build the mirrored site documents: (guid, dotted) - the newest
    entry of its update list, e.g. ("version-dcbeee682ce74ee0", "0.735.0.7351131")."""
    index = SITE / "index.html"
    if not index.is_file():
        return None, None
    text = index.read_text(encoding="utf-8", errors="replace")
    guid = re.search(r"version-[0-9a-f]{16}", text)
    dotted = re.search(r"<span class=version>(.*?)</span>\)", text)
    dotted = re.sub(r"<[^>]+>", "", dotted.group(1)) if dotted else ""
    return (guid.group(0) if guid else None), dotted.lstrip("v")


def refresh_cache(force, offline=False):
    CACHE.mkdir(parents=True, exist_ok=True)

    # The site. A --fetch re-reads every page, because a page that exists
    # already is exactly the page whose members and words may have changed.
    if not offline and (force or not (SITE / "index.html").is_file()):
        fetch_ref_module().mirror(fresh=force)

    # The full dump for the build the site documents, so the two agree.
    guid, _ = site_version()
    have = FULL_DUMP_VERSION.read_text().strip() if FULL_DUMP_VERSION.exists() else ""
    if guid and not offline and (force or not FULL_DUMP.exists() or have != guid):
        print(f"gen-api: fetching the full API dump {guid}")
        FULL_DUMP.write_bytes(fetch(FULL_DUMP_URL.format(version=guid), timeout=180))
        FULL_DUMP_VERSION.write_text(guid + "\n")

    if offline:
        return
    for section in SECTIONS:
        folder = DOCS / section
        if not force and folder.is_dir() and any(folder.glob("*.yaml")):
            continue
        folder.mkdir(parents=True, exist_ok=True)
        listing = json.loads(fetch(DOCS_API.format(section=section)))
        names = [e["name"] for e in listing if isinstance(e, dict) and e.get("name", "").endswith(".yaml")]
        print(f"gen-api: fetching {len(names)} {section}")

        def one(name):
            (folder / name).write_bytes(fetch(DOCS_RAW.format(section=section, name=name)))

        with ThreadPoolExecutor(max_workers=12) as pool:
            list(pool.map(one, names))


# ------------------------------------------------------- a small YAML reader

# The reference files are machine-generated and use a narrow, regular subset
# of YAML: mappings, lists of mappings, block scalars, quoted and plain
# scalars. PyYAML is not part of a stock macOS Python, and this is enough.

class Line:
    __slots__ = ("indent", "text")

    def __init__(self, raw):
        self.text = raw.rstrip("\n")
        stripped = self.text.lstrip(" ")
        self.indent = len(self.text) - len(stripped)
        self.text = stripped


def yaml_load(source):
    lines = [Line(raw) for raw in source.splitlines()]
    value, _ = _parse_node(lines, 0, 0)
    return value


def _is_blank(line):
    return line.text == "" or line.text.startswith("#")


def _skip_blank(lines, i):
    while i < len(lines) and _is_blank(lines[i]):
        i += 1
    return i


def _parse_node(lines, i, indent):
    i = _skip_blank(lines, i)
    if i >= len(lines) or lines[i].indent < indent:
        return None, i
    if lines[i].text.startswith("- ") or lines[i].text == "-":
        return _parse_list(lines, i, lines[i].indent)
    return _parse_mapping(lines, i, lines[i].indent)


def _parse_list(lines, i, indent):
    items = []
    while True:
        i = _skip_blank(lines, i)
        if i >= len(lines) or lines[i].indent != indent or not (lines[i].text.startswith("- ") or lines[i].text == "-"):
            return items, i
        head = lines[i].text[2:].strip() if lines[i].text != "-" else ""
        if head == "":
            value, i = _parse_node(lines, i + 1, indent + 1)
            items.append(value)
            continue
        # The item is a mapping whose first key sits on the dash line. Rewrite
        # that line as a key at the item's indent and parse the mapping; the
        # item's other keys follow at that same indent. Single pass, so the
        # rewritten line is never needed again.
        if _looks_like_key(head):
            lines[i] = Line(" " * (indent + 2) + head)
            value, i = _parse_mapping(lines, i, indent + 2)
            items.append(value)
            continue
        items.append(_scalar(head))
        i += 1


_KEY = re.compile(r"^([A-Za-z_][A-Za-z0-9_.\-]*):(?:\s+(.*))?$")


def _looks_like_key(text):
    return _KEY.match(text) is not None


def _parse_mapping(lines, i, indent):
    out = {}
    while True:
        i = _skip_blank(lines, i)
        if i >= len(lines) or lines[i].indent != indent:
            return out, i
        m = _KEY.match(lines[i].text)
        if not m:
            # Not a key at this level: the mapping is over.
            return out, i
        key, rest = m.group(1), m.group(2)
        if rest is None or rest == "":
            # Either a nested node on the following lines or an empty value.
            j = _skip_blank(lines, i + 1)
            if j < len(lines) and lines[j].indent > indent:
                value, i = _parse_node(lines, j, lines[j].indent)
            elif j < len(lines) and lines[j].indent == indent and (lines[j].text.startswith("- ")):
                # A list at the same indent as its key (YAML allows it).
                value, i = _parse_list(lines, j, indent)
            else:
                value, i = None, i + 1
            out[key] = value
            continue
        rest = rest.strip()
        if rest in ("|", "|-", "|+", ">", ">-", ">+"):
            value, i = _parse_block_scalar(lines, i + 1, indent, folded=rest.startswith(">"))
            out[key] = value
            continue
        out[key] = _scalar(rest)
        i += 1


def _parse_block_scalar(lines, i, parent_indent, folded):
    collected = []
    block_indent = None
    while i < len(lines):
        line = lines[i]
        if line.text == "":
            collected.append("")
            i += 1
            continue
        if line.indent <= parent_indent:
            break
        if block_indent is None:
            block_indent = line.indent
        extra = " " * max(0, line.indent - block_indent)
        collected.append(extra + line.text)
        i += 1
    while collected and collected[-1] == "":
        collected.pop()
    text = " ".join(collected) if folded else "\n".join(collected)
    return text, i


def _scalar(text):
    if text == "''" or text == '""':
        return ""
    if text == "[]":
        return []
    if text == "{}":
        return {}
    if text in ("null", "~"):
        return None
    if text == "true":
        return True
    if text == "false":
        return False
    if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
        try:
            return json.loads(text)
        except ValueError:
            return text[1:-1]
    if len(text) >= 2 and text[0] == "'" and text[-1] == "'":
        return text[1:-1].replace("''", "'")
    return text


# ------------------------------------------------------------- text helpers

_REF = re.compile(r"`(?:Class|Datatype|Library|Enum|Global)\.([^`|]+?)(?:\|([^`]+?))?`")
_CODE = re.compile(r"`([^`]*)`")
_LINK = re.compile(r"\[([^\]]+)\]\([^)]*\)")


def clean(text):
    """One readable sentence from a documentation summary."""
    if not text:
        return ""
    text = text.replace("\n", " ")

    def ref(m):
        shown = m.group(2) or m.group(1)
        # `Class.Players:GetPlayers()` -> `Players:GetPlayers()`
        return "`" + shown + "`"

    text = _REF.sub(ref, text)
    text = _LINK.sub(r"\1", text)
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) > SUMMARY_MAX:
        cut = text.find(". ", 40)
        if 0 < cut < SUMMARY_MAX + 40:
            text = text[:cut + 1]
        else:
            text = text[:SUMMARY_MAX - 1].rstrip() + "…"
    return text


_BLOCK_LINE = re.compile(r"^([-*+]\s|\d+\.\s|\||>)")
_HEADING = re.compile(r"^#{1,6}\s+(.*)$")


def clean_long(text):
    """The full description as markdown a hover can show.

    Paragraphs are kept and the documentation's 80-column wrapping is undone;
    lists, tables and code fences keep their lines; `Class.X|Y` references and
    links become their text (a link in a hover would navigate the editor
    page); HTML the markdown renderer would print literally is stripped; and
    the whole is cut at a paragraph once it passes DESCRIPTION_MAX."""
    if not text:
        return ""
    text = text.replace("\r\n", "\n")
    text = re.sub(r"<br\s*/?>", "\n", text)
    text = re.sub(r"<sup>(.*?)</sup>", r"^\1", text)
    text = re.sub(r"<[^>\n]+>", "", text)
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text)

    blocks, prose, block, fence = [], [], [], []

    def flush_prose():
        if prose:
            blocks.append(re.sub(r"[ \t]+", " ", " ".join(prose)))
            prose.clear()

    def flush_block():
        if block:
            blocks.append("\n".join(block))
            block.clear()

    for raw in text.split("\n"):
        stripped = raw.strip()
        if stripped.startswith("```"):
            if fence:
                fence.append(stripped)
                blocks.append("\n".join(fence))
                fence.clear()
            else:
                flush_prose()
                flush_block()
                fence.append(stripped)
            continue
        if fence:
            fence.append(raw.rstrip())
            continue
        if not stripped:
            flush_prose()
            flush_block()
            continue
        heading = _HEADING.match(stripped)
        if heading:
            flush_prose()
            flush_block()
            blocks.append("**" + heading.group(1).strip() + "**")
            continue
        if _BLOCK_LINE.match(stripped):
            flush_prose()
            block.append(stripped)
            continue
        if block and raw[:1] in (" ", "\t"):
            block[-1] += " " + stripped
            continue
        flush_block()
        prose.append(stripped)
    flush_prose()
    flush_block()
    if fence:
        blocks.append("\n".join(fence))

    def ref(m):
        return "`" + (m.group(2) or m.group(1)) + "`"

    kept, total = [], 0
    for b in blocks:
        b = _LINK.sub(r"\1", _REF.sub(ref, b)).strip()
        if not b:
            continue
        if kept and total + len(b) > DESCRIPTION_MAX:
            kept.append("…")
            break
        kept.append(b)
        total += len(b)
    return "\n\n".join(kept)


# The full descriptions, for api-docs.js. Filled by the builders as they go.
DOCS_OUT = {}


def remember(key, entry, summary):
    """Keep the description for api-docs.js when it says more than the
    summary does, with the deprecation notice if there is one."""
    if key in DOCS_OUT or not isinstance(entry, dict):
        return
    desc = clean_long(entry.get("description") or "")
    if not desc or desc == summary or (len(desc) < len(summary) and summary.startswith(desc)):
        desc = ""
    dep = clean(entry.get("deprecation_message") or "")
    text = desc or summary
    if dep:
        text = (text + "\n\n" if text else "") + "**Deprecated.** " + dep
    if text and text != summary:
        DOCS_OUT[key] = text


# What the dump calls a type, against what a Luau programmer would write. The
# dump's vocabulary is the engine's reflection system, not the language's:
# `Instances` is an array, `ContentId` is a string, `CoordinateFrame` is the
# old name for CFrame. Names left out of this map are passed through, which is
# right for every class and datatype and harmless for the dozen internal types
# (QDir, OpenCloudModel) that no script can reach anyway.
ALIASES = {
    "int": "number", "int64": "number", "float": "number", "double": "number",
    "bool": "boolean", "void": "()", "null": "nil",
    "Instances": "{Instance}", "Objects": "{Instance}",
    "Array": "{any}", "Tuple": "...any", "Variant": "any",
    "Dictionary": "{[string]: any}", "Map": "{[any]: any}",
    "ContentId": "string", "BinaryString": "string", "ProtectedString": "string",
    "Function": "function", "CoordinateFrame": "CFrame",
    "OptionalCoordinateFrame": "CFrame?",
}


def type_name(t):
    """The dump's {Category, Name} as the name a Luau programmer would write."""
    if t is None:
        return "any"
    if isinstance(t, list):
        return ", ".join(type_name(x) for x in t)
    cat, name = t.get("Category"), t.get("Name", "any")
    if cat == "Enum":
        return "Enum." + name
    # An optional keeps its `?`; the name underneath is still worth resolving.
    optional = name.endswith("?")
    if optional:
        name = name[:-1]
    name = ALIASES.get(name, name)
    return name + "?" if optional and not name.endswith("?") else name


def security_code(sec):
    """One letter for the access a member needs: '' for none, R for
    RobloxScriptSecurity, P for PluginSecurity, L for LocalUserSecurity, X
    for a level no script reaches. A property has a level for reading and
    one for writing; the stricter of the two is the one that matters."""
    if isinstance(sec, dict):
        read, write = sec.get("Read") or "None", sec.get("Write") or "None"
        sec = read if read != "None" else write
    return {"None": "", "RobloxScriptSecurity": "R", "PluginSecurity": "P",
            "LocalUserSecurity": "L", "RobloxSecurity": "X", "NotAccessibleSecurity": "X",
            "RobloxPlaceSecurity": "X", "WritePlayerSecurity": ""}.get(sec, "")


# ------------------------------------------------------------ the site's pages
#
# Hugo writes the reference with one <section id=member-X> per member and one
# <div class=docs> holding whatever was said about it. The docs div nests
# (every code sample is a table inside a div), so it is cut out by counting
# divs rather than by a pattern, and only that fragment goes through the HTML
# parser - the pages are a megabyte each and most of it is navigation.

_DOCS_DIV = "<div class=docs>"
_MEMBER_SECTION = re.compile(r"<section id=member-([A-Za-z0-9_]+)(?: class=\"?[^\">]*\"?)?>")
_ENUM_ROW = re.compile(r"<tr id=member-([A-Za-z0-9_]+)(?: class=\"?[^\">]*\"?)?>(.*?)</tr>", re.S)
_DIV_EDGE = re.compile(r"<div\b|</div>")


def docs_div(html, start=0, end=None):
    """The balanced `<div class=docs>...</div>` at or after `start`, within
    `end`; None when the slice has none."""
    end = len(html) if end is None else end
    i = html.find(_DOCS_DIV, start, end)
    if i < 0:
        return None
    depth = 0
    for m in _DIV_EDGE.finditer(html, i, end):
        depth += 1 if m.group(0) == "<div" else -1
        if depth == 0:
            return html[i:m.end()]
    return html[i:end]


class DocsToMarkdown(HTMLParser):
    """The site's description HTML as the markdown a hover can show.

    Paragraphs stay paragraphs; a link to a class, enum or type becomes the
    name in backticks and any other link becomes its text (a link in a hover
    would navigate the editor page); `code`, **strong** and _em_ keep their
    marks; lists and tables become markdown lists and tables; a highlighted
    code sample - a table of spans in the page - becomes a ```lua fence."""

    ENTITY = ("/ref/class/", "/ref/enum/", "/ref/type/")

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.blocks = []          # finished blocks, joined with a blank line
        self.inline = []          # text of the block being built
        self.stack = []           # open tags
        self.code_depth = 0       # inside <code> (inline)
        self.pre = None           # collecting a code sample: list of text
        self.chroma = 0           # div depth inside a highlighted sample
        self.in_code_of_chroma = False
        self.link = None          # (href, start index into self.inline)
        self.lists = []           # [kind, count] per open list
        self.list_lines = []
        self.table = None         # {"rows": [...], "row": [...] or None, "cell": [...] or None}

    # -- helpers ---------------------------------------------------------
    def _text(self):
        return "".join(self.inline)

    def _flush(self):
        text = re.sub(r"[ \t\r\n]+", " ", self._text()).strip()
        self.inline = []
        if text:
            self.blocks.append(text)

    def _emit(self, s):
        if self.table and self.table["cell"] is not None:
            self.table["cell"].append(s)
        else:
            self.inline.append(s)

    # -- tags ------------------------------------------------------------
    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        classes = (attrs.get("class") or "").split()
        if self.pre is not None:
            if tag == "code" and self.chroma:
                self.in_code_of_chroma = True
            if tag == "div":
                self.chroma += 1 if self.chroma else 0
            return
        if tag == "div" and "_chroma" in classes:
            self._flush()
            self.pre = []
            self.chroma = 1
            self.in_code_of_chroma = False
            return
        if tag == "pre":
            self._flush()
            self.pre = []
            self.chroma = 0
            return
        if tag == "p":
            self._flush()
        elif tag in ("h1", "h2", "h3", "h4", "h5", "h6"):
            self._flush()
            self.stack.append("heading")
            return
        elif tag == "a":
            self.link = (attrs.get("href") or "", len(self.inline) if not (self.table and self.table["cell"] is not None) else -1)
        elif tag == "code" or tag == "kbd":
            self.code_depth += 1
            self._emit("`")
        elif tag in ("strong", "b"):
            self._emit("**")
        elif tag in ("em", "i"):
            self._emit("_")
        elif tag == "br":
            self._emit("\n")
        elif tag in ("ul", "ol"):
            self._flush()
            self.lists.append([tag, 0])
        elif tag == "li":
            self.inline = []
            if self.lists:
                self.lists[-1][1] += 1
        elif tag == "table":
            self._flush()
            self.table = {"rows": [], "row": None, "cell": None}
        elif tag == "tr" and self.table:
            self.table["row"] = []
        elif tag in ("td", "th") and self.table:
            self.table["cell"] = []
        self.stack.append(tag)

    def handle_endtag(self, tag):
        if self.pre is not None:
            if self.chroma:
                if tag == "code":
                    self.in_code_of_chroma = False
                if tag == "div":
                    self.chroma -= 1
                    if self.chroma == 0:
                        self._finish_pre("lua")
                return
            if tag == "pre":
                self._finish_pre("")
            return
        if tag in ("h1", "h2", "h3", "h4", "h5", "h6"):
            text = re.sub(r"\s+", " ", self._text()).strip()
            self.inline = []
            if text:
                self.blocks.append("**" + text + "**")
            if self.stack and self.stack[-1] == "heading":
                self.stack.pop()
            return
        if tag == "p":
            self._flush()
        elif tag == "a" and self.link:
            href, start = self.link
            self.link = None
            if start >= 0 and href.startswith(self.ENTITY) and not self.code_depth:
                text = "".join(self.inline[start:]).strip()
                if text and " " not in text:
                    del self.inline[start:]
                    self.inline.append("`" + text + "`")
        elif tag == "code" or tag == "kbd":
            self.code_depth = max(0, self.code_depth - 1)
            self._emit("`")
        elif tag in ("strong", "b"):
            self._emit("**")
        elif tag in ("em", "i"):
            self._emit("_")
        elif tag == "li":
            text = re.sub(r"\s+", " ", self._text()).strip()
            self.inline = []
            if text and self.lists:
                kind, count = self.lists[-1]
                indent = "  " * (len(self.lists) - 1)
                marker = f"{count}. " if kind == "ol" else "- "
                self.list_lines.append(indent + marker + text)
        elif tag in ("ul", "ol"):
            if self.lists:
                self.lists.pop()
            if not self.lists and self.list_lines:
                self.blocks.append("\n".join(self.list_lines))
                self.list_lines = []
        elif tag in ("td", "th") and self.table and self.table["cell"] is not None:
            cell = re.sub(r"\s+", " ", "".join(self.table["cell"])).strip().replace("|", "\\|")
            if self.table["row"] is not None:
                self.table["row"].append(cell)
            self.table["cell"] = None
        elif tag == "tr" and self.table and self.table["row"] is not None:
            self.table["rows"].append(self.table["row"])
            self.table["row"] = None
        elif tag == "table" and self.table:
            rows = [r for r in self.table["rows"] if r]
            self.table = None
            if rows:
                width = max(len(r) for r in rows)
                rows = [r + [""] * (width - len(r)) for r in rows]
                lines = ["| " + " | ".join(rows[0]) + " |", "|" + "---|" * width]
                lines += ["| " + " | ".join(r) + " |" for r in rows[1:]]
                self.blocks.append("\n".join(lines))
        if self.stack and self.stack[-1] == tag:
            self.stack.pop()

    def _finish_pre(self, lang):
        code = "".join(self.pre).strip("\n")
        self.pre = None
        self.chroma = 0
        if code.strip():
            self.blocks.append("```" + lang + "\n" + code + "\n```")

    def handle_data(self, data):
        if self.pre is not None:
            if not self.chroma or self.in_code_of_chroma:
                self.pre.append(data)
            return
        if self.stack and self.stack[-1] == "heading":
            self.inline.append(data)
            return
        self._emit(data)

    def markdown(self):
        self._flush()
        if self.list_lines:
            self.blocks.append("\n".join(self.list_lines))
            self.list_lines = []
        kept, total = [], 0
        for block in self.blocks:
            if kept and total + len(block) > DESCRIPTION_MAX:
                kept.append("…")
                break
            kept.append(block)
            total += len(block)
        return "\n\n".join(kept)


def html_to_markdown(fragment):
    if not fragment:
        return ""
    parser = DocsToMarkdown()
    parser.feed(fragment)
    parser.close()
    return parser.markdown()


def first_sentence(markdown):
    """The one-line summary: the first prose paragraph, clipped the way a
    documentation summary is."""
    for block in markdown.split("\n\n"):
        block = block.strip()
        # Not a code sample, a table, a list, a heading or the cut mark - but
        # a paragraph that merely opens in bold is prose.
        if not block or block.startswith(("```", "|", "- ", "…")) or re.match(r"^\d+\. ", block):
            continue
        if re.fullmatch(r"\*\*[^*]+\*\*", block):
            continue
        return clean(block)
    return ""


def section_docs(html, start, end):
    """Markdown of the docs div inside html[start:end], or ''."""
    return html_to_markdown(docs_div(html, start, end) or "")


def site_page(kind, name):
    path = SITE / kind / f"{name}.html"
    if not path.is_file():
        return None
    return path.read_text(encoding="utf-8", errors="replace")


def site_class(name):
    """{'sum', 'desc', 'members': {name: description}} from the class's page."""
    html = site_page("class", name)
    if html is None:
        return None
    out = {"sum": "", "desc": "", "members": {}}
    i = html.find("<section id=summary>")
    if i >= 0:
        out["sum"] = section_docs(html, i, html.find("</section>", i))
    i = html.find("<section id=description>")
    if i >= 0:
        out["desc"] = section_docs(html, i, html.find("</section>", i))
    hits = list(_MEMBER_SECTION.finditer(html))
    for n, m in enumerate(hits):
        end = hits[n + 1].start() if n + 1 < len(hits) else len(html)
        # The docs div comes before the member's status boxes and history;
        # bounding the search to the section keeps a blank member blank.
        out["members"][m.group(1)] = section_docs(html, m.end(), end)
    return out


def site_enum(name):
    """{'sum', 'desc', 'items': {name: description}} from the enum's page."""
    html = site_page("enum", name)
    if html is None:
        return None
    out = {"sum": "", "desc": "", "items": {}}
    i = html.find("<section id=summary>")
    if i >= 0:
        out["sum"] = section_docs(html, i, html.find("</section>", i))
    i = html.find("<section id=description>")
    if i >= 0:
        out["desc"] = section_docs(html, i, html.find("</section>", i))
    for m in _ENUM_ROW.finditer(html):
        cell = re.search(r"<td class=col-desc>(.*?)</td>", m.group(2), re.S)
        out["items"][m.group(1)] = html_to_markdown(cell.group(1)) if cell else ""
    return out


# ----------------------------------------------------------------- classes

def load_docs(section):
    """The creator-docs YAML of one section, by name."""
    out = {}
    folder = DOCS / section
    if not folder.is_dir():
        return out
    for path in sorted(folder.glob("*.yaml")):
        try:
            data = yaml_load(path.read_text(encoding="utf-8", errors="replace"))
        except Exception as e:  # a single unreadable file must not sink the build
            print(f"gen-api: could not read {path.name}: {e}", file=sys.stderr)
            continue
        if isinstance(data, dict) and data.get("name"):
            out[data["name"]] = data
    return out


def yaml_entries(doc, sections):
    """{'NumPlayers': entry} from one creator-docs file - the fallback for a
    name the site's page says nothing about."""
    out = {}
    if not doc:
        return out
    for section in sections:
        for entry in doc.get(section) or []:
            if isinstance(entry, dict):
                out[re.split(r"[.:]", str(entry.get("name", "")))[-1]] = entry
    return out


def deprecation_messages(doc, sections):
    """{'NumPlayers': 'Use GetPlayers() instead.'} from one creator-docs file:
    the one thing about a member the site does not render."""
    out = {}
    if not doc:
        return out
    message = clean(doc.get("deprecation_message") or "")
    if message:
        out[""] = message
    for section in sections:
        for entry in doc.get(section) or []:
            if not isinstance(entry, dict):
                continue
            message = clean(entry.get("deprecation_message") or "")
            if message:
                out[re.split(r"[.:]", str(entry.get("name", "")))[-1]] = message
    return out


def with_deprecation(text, message):
    if not message:
        return text
    return (text + "\n\n" if text else "") + "**Deprecated.** " + message


def keep_docs(key, summary, description, deprecation=""):
    """api-docs.js gets the description when it says more than the summary,
    and the deprecation notice whenever there is one."""
    if description == summary or (len(description) < len(summary) and summary.startswith(description)):
        description = ""
    text = with_deprecation(description or summary, deprecation)
    if text and text != summary:
        DOCS_OUT[key] = text


# What the dump writes for a property whose default it could not read.
_NO_DEFAULT = re.compile(r"^__api_dump_")


def property_default(member):
    default = member.get("Default")
    if default is None or _NO_DEFAULT.match(str(default)):
        return None
    return str(default)


def build_classes(dump, docs):
    classes = {}
    missing_pages = []
    for cls in dump["Classes"]:
        name = cls["Name"]
        page = site_class(name)
        if page is None:
            missing_pages.append(name)
            page = {"sum": "", "desc": "", "members": {}}
        doc = docs.get(name)
        sections = ("properties", "methods", "events", "callbacks")
        deprecations = deprecation_messages(doc, sections)
        fallback = yaml_entries(doc, sections)
        members = []
        for m in cls["Members"]:
            # Four entries in the dump have a space in the name; no script can
            # write them and the site leaves them out.
            if " " in m["Name"]:
                continue
            tags = m.get("Tags", []) or []
            kind = {"Property": "p", "Function": "f", "Event": "e", "Callback": "c"}[m["MemberType"]]
            entry = [m["Name"], kind]
            if kind == "p":
                entry.append(type_name(m.get("ValueType")))
                entry.append(property_default(m))
            else:
                entry.append(type_name(m.get("ReturnType")) if kind != "e" else "")
                params = []
                for p in m.get("Parameters", []) or []:
                    row = [p["Name"], type_name(p.get("Type"))]
                    if "Default" in p:
                        row.append(str(p["Default"]))
                    params.append(row)
                entry.append(params)
            entry.append(security_code(m.get("Security")))
            entry.append([t for t in tags if t in ("Deprecated", "Hidden", "ReadOnly", "Yields",
                                                   "NotReplicated", "NotScriptable")])
            description = page["members"].get(m["Name"], "")
            summary = first_sentence(description)
            if not description:
                # The site's snapshot says nothing; creator-docs may by now.
                spare = fallback.get(m["Name"]) or {}
                description = clean_long(spare.get("description") or "")
                summary = clean(spare.get("summary") or "") or first_sentence(description)
            entry.append(summary)
            keep_docs(name + "." + m["Name"], summary, description, deprecations.get(m["Name"], ""))
            members.append(entry)
        summary = page["sum"] or first_sentence(page["desc"])
        description = page["desc"]
        if not summary and doc:
            summary = clean(doc.get("summary") or "")
            description = clean_long(doc.get("description") or "")
        keep_docs(name, summary, description, deprecations.get("", ""))
        classes[name] = {
            "super": cls.get("Superclass", "<<<ROOT>>>"),
            "tags": [t for t in cls.get("Tags", []) or [] if t in ("Service", "NotCreatable", "Deprecated", "NotBrowsable")],
            "sum": summary,
            "members": members,
        }
    if missing_pages:
        print(f"gen-api: {len(missing_pages)} classes have no page in the mirror "
              f"({', '.join(missing_pages[:6])}{', …' if len(missing_pages) > 6 else ''}); "
              f"re-run tools/fetch-ref.py", file=sys.stderr)
    return classes


def build_enums(dump, docs):
    """{'Material': {sum, tags, items: [[name, value, summary, tags], ...]}}.

    The dump is the authority on the items and their values; the site's page
    says what the enum and each item mean; creator-docs adds why an item is
    deprecated; a legacy name the engine still accepts is noted too."""
    out = {}
    for e in dump["Enums"]:
        page = site_enum(e["Name"]) or {"sum": "", "desc": "", "items": {}}
        doc = docs.get(e["Name"])
        deprecations = deprecation_messages(doc, ("items",))
        fallback = yaml_entries(doc, ("items",))
        items = []
        for item in e["Items"]:
            tags = [t for t in (item.get("Tags") or []) if t == "Deprecated"]
            summary = page["items"].get(item["Name"], "")
            if not summary:
                summary = clean((fallback.get(item["Name"]) or {}).get("summary") or "")
            legacy = item.get("LegacyNames") or []
            if legacy:
                summary = (summary + " " if summary else "") + "Also accepted as " + \
                    ", ".join("`" + n + "`" for n in legacy) + "."
            summary = with_deprecation(summary, deprecations.get(item["Name"], ""))
            items.append([item["Name"], item.get("Value"), summary, tags])
        summary = page["sum"] or first_sentence(page["desc"])
        description = page["desc"]
        if not summary and doc:
            summary = clean(doc.get("summary") or "")
            description = clean_long(doc.get("description") or "")
        keep_docs("Enum." + e["Name"], summary, description, deprecations.get("", ""))
        out[e["Name"]] = {
            "sum": summary,
            "tags": [t for t in (e.get("Tags") or []) if t == "Deprecated"],
            "items": items,
        }
    return out


# ---------------------------------------------- datatypes, libraries, globals

def params_of(entry):
    params = []
    for p in entry.get("parameters") or []:
        if not isinstance(p, dict):
            continue
        row = [str(p.get("name", "")), str(p.get("type", "any"))]
        default = p.get("default")
        if default not in (None, ""):
            row.append(str(default))
        params.append(row)
    return params


def returns_of(entry):
    rets = [str(r.get("type", "")) for r in (entry.get("returns") or []) if isinstance(r, dict)]
    rets = [r for r in rets if r and r != "void"]
    return ", ".join(rets) if rets else "()"


def short_name(name):
    return re.split(r"[.:]", str(name))[-1]


def summarised(owner, e):
    """The one-line summary of a datatype, library or global entry, with its
    full description remembered for api-docs.js."""
    summary = clean(e.get("summary", ""))
    remember((owner + "." if owner else "") + short_name(e.get("name")), e, summary)
    return summary


def build_datatypes(docs):
    out = {}
    for name, doc in docs.items():
        ctors, consts, props, methods, funcs = [], [], [], [], []
        for e in doc.get("constructors") or []:
            if isinstance(e, dict):
                ctors.append([short_name(e.get("name")), params_of(e), name, summarised(name, e)])
        for e in doc.get("constants") or []:
            if isinstance(e, dict):
                consts.append([short_name(e.get("name")), str(e.get("type", name)), summarised(name, e)])
        seen_consts = {c[0] for c in consts}
        for e in doc.get("properties") or []:
            if isinstance(e, dict) and short_name(e.get("name")) not in seen_consts:
                props.append([short_name(e.get("name")), str(e.get("type", "any")), summarised(name, e)])
        for e in doc.get("methods") or []:
            if isinstance(e, dict):
                methods.append([short_name(e.get("name")), params_of(e), returns_of(e), summarised(name, e)])
        for e in doc.get("functions") or []:
            if isinstance(e, dict):
                funcs.append([short_name(e.get("name")), params_of(e), returns_of(e), summarised(name, e)])
        summary = clean(doc.get("summary", ""))
        remember(name, doc, summary)
        out[name] = {"sum": summary, "ctors": ctors, "consts": consts,
                     "props": props, "methods": methods, "funcs": funcs}
    return out


def build_libraries(docs):
    out = {}
    for name, doc in docs.items():
        props, funcs = [], []
        for e in doc.get("properties") or []:
            if isinstance(e, dict):
                props.append([short_name(e.get("name")), str(e.get("type", "any")), summarised(name, e)])
        for e in doc.get("functions") or []:
            if isinstance(e, dict):
                funcs.append([short_name(e.get("name")), params_of(e), returns_of(e), summarised(name, e)])
        summary = clean(doc.get("summary", ""))
        remember(name, doc, summary)
        out[name] = {"sum": summary, "props": props, "funcs": funcs}
    return out


def build_globals(docs):
    props, funcs = [], []
    for doc in docs.values():
        for e in doc.get("properties") or []:
            if isinstance(e, dict):
                props.append([str(e.get("name")), str(e.get("type", "any")), summarised("", e)])
        for e in doc.get("functions") or []:
            if isinstance(e, dict):
                funcs.append([str(e.get("name")), params_of(e), returns_of(e), summarised("", e)])
    return {"props": props, "funcs": funcs}


# ------------------------------------------------------- what the engine said
#
# The libraries and datatypes above come from Roblox's *documentation*, which is
# written by people and is not the engine. Measured against a live client,
# twenty-five members exist that the documentation never mentions - Vector3.max,
# Vector3.min, Vector3.fromAxis, Color3.toHSV, BrickColor.New, every member of
# SharedTable, four of debug's - and each one of them was a red squiggle under
# working code in the editor.
#
# tools/engine-tables.json is what a live client answered when asked. It is the
# authority on what exists, and it only ever adds. What each addition *means*
# cannot be read off a client, so it comes from one of four places, tried in
# this order:
#
#   - the executor's own: a `debug.*` the executor defines is described by
#     executor-docs.js, and is left out here so the resolver reaches that;
#   - a twin: `BrickColor.New` is `BrickColor.new` with a capital, and
#     `Vector3.fromAxis` is `Vector3.FromAxis` without one - the documented
#     row is cloned and the sentence says which spelling this is;
#   - a legacy global alias: `Wait` is `wait`, `Game` is `game`;
#   - tools/engine-docs.json, written by hand for the rest.
#
# A name that none of those covers keeps the placeholder below so it is not a
# squiggle, and is named on stderr so that it gets written up.

ENGINE_TABLES = ROOT / "tools" / "engine-tables.json"
ENGINE_DOCS = ROOT / "tools" / "engine-docs.json"
ENGINE_NOTE = "Present in the engine; absent from Roblox's documentation."


def executor_libraries():
    """{'debug': ['getupvalues', ...]} - the executor's own library members,
    from the table gen-globals.py wrote."""
    if not GLOBALS_JS.exists():
        return {}
    text = GLOBALS_JS.read_text(encoding="utf-8")
    try:
        return json.loads(text[text.index("{"):text.rindex("}") + 1]).get("libraries", {})
    except ValueError:
        return {}


def twin_of(name):
    """`new` for `New`, `FromAxis` for `fromAxis`: the other capitalisation."""
    if not name or not name[0].isalpha():
        return None
    first = name[0].lower() if name[0].isupper() else name[0].upper()
    return first + name[1:]


def merge_engine(libraries, datatypes, globals_):
    """Fold tools/engine-tables.json into the documented tables, described.

    Returns (added, derived, undescribed): rows written from engine-docs.json,
    rows cloned from a documented twin or alias, and the names left with the
    placeholder because nothing describes them."""
    if not ENGINE_TABLES.exists():
        print("gen-api: no engine snapshot; shipping documentation only", file=sys.stderr)
        return 0, 0, []

    snapshot = json.loads(ENGINE_TABLES.read_text(encoding="utf-8"))
    hand = json.loads(ENGINE_DOCS.read_text(encoding="utf-8")) if ENGINE_DOCS.exists() else {}
    aliases = hand.get("aliases", {})
    hand_globals = hand.get("globals", {})
    hand_namespaces = hand.get("namespaces", {})
    ours = executor_libraries()
    added, derived, undescribed = 0, 0, []

    def known(entry, *fields):
        out = set()
        for field in fields:
            for row in entry.get(field) or []:
                out.add(row[0])
        return out

    def rows_named(entry, name, *fields):
        return [row for field in fields for row in (entry.get(field) or []) if row[0] == name]

    for ns, members in snapshot.get("namespaces", {}).items():
        if ns in libraries:
            entry, is_library = libraries[ns], True
            have = known(entry, "props", "funcs")
        elif ns in datatypes:
            entry, is_library = datatypes[ns], False
            have = known(entry, "ctors", "funcs", "consts", "props", "methods")
        else:
            continue
        executor_owned = set(ours.get(ns, []))
        by_hand = hand_namespaces.get(ns, {})
        for member, kind in sorted(members.items()):
            if member in have or member in executor_owned:
                continue
            spec = by_hand.get(member)
            if spec:
                if kind == "f":
                    entry["funcs"].append([member, spec.get("params", []), spec.get("returns", "" if is_library else ns), spec["sum"]])
                elif is_library:
                    entry["props"].append([member, spec.get("type", "any"), spec["sum"]])
                else:
                    entry["consts"].append([member, spec.get("type", ns), spec["sum"]])
                added += 1
                continue
            twin = twin_of(member)
            fields = ("funcs",) if is_library else ("ctors", "funcs")
            rows = rows_named(entry, twin, *fields) if twin else []
            if rows and kind == "f":
                spelling = "capitalised" if member[0].isupper() else "lower-case"
                what = "constructor" if rows_named(entry, twin, "ctors") else "function"
                note = f"The {spelling} spelling of `{ns}.{twin}`; the same {what}."
                for row in rows:
                    entry["funcs"].append([member, row[1], row[2] or ns, note])
                derived += 1
                continue
            if kind == "f":
                entry["funcs"].append([member, [], "" if is_library else ns, ENGINE_NOTE])
            elif is_library:
                entry["props"].append([member, "any", ENGINE_NOTE])
            else:
                entry["consts"].append([member, ns, ENGINE_NOTE])
            undescribed.append(f"{ns}.{member}")

    have = {row[0] for row in globals_["props"]} | {row[0] for row in globals_["funcs"]}
    for name, kind in sorted(snapshot.get("globals", {}).items()):
        if name in have:
            continue
        target = aliases.get(name)
        if target:
            spelling = "capitalised" if name[0].isupper() else "lower-case"
            func = rows_named(globals_, target, "funcs")
            prop = rows_named(globals_, target, "props")
            if func:
                row = func[0]
                globals_["funcs"].append([name, row[1], row[2],
                                          f"The {spelling} spelling of `{target}`, kept from early Roblox; the same function."])
                derived += 1
                continue
            if prop:
                row = prop[0]
                globals_["props"].append([name, row[1],
                                          f"The {spelling} spelling of `{target}`, kept from early Roblox; the same object."])
                derived += 1
                continue
        if name in datatypes or name in libraries:
            # Described by its own table; a second row would only shadow it.
            continue
        spec = hand_globals.get(name)
        if spec and spec.get("kind") == "namespace":
            datatypes[name] = {
                "sum": spec["sum"], "ctors": [], "consts": [], "props": [], "methods": [],
                "funcs": [[m, s.get("params", []), s.get("returns", name), s["sum"]]
                          for m, s in spec.get("members", {}).items()],
            }
            added += 1
            continue
        if spec:
            if spec.get("kind") == "f":
                globals_["funcs"].append([name, spec.get("params", []), spec.get("returns", ""), spec["sum"]])
            else:
                globals_["props"].append([name, spec.get("type", ""), spec["sum"]])
            added += 1
            continue
        if kind == "f":
            globals_["funcs"].append([name, [], "", ENGINE_NOTE])
        else:
            globals_["props"].append([name, "", ENGINE_NOTE])
        undescribed.append(name)

    return added, derived, undescribed


# --------------------------------------------------------------------- main

def main(argv):
    force = "--fetch" in argv
    offline = "--offline" in argv
    try:
        refresh_cache(force, offline)
    except Exception as e:
        if not FULL_DUMP.exists() or not (SITE / "index.html").is_file():
            print(f"gen-api: the cache is short and the fetch failed: {e}", file=sys.stderr)
            return 1
        print(f"gen-api: fetch failed, using the cache: {e}", file=sys.stderr)
    if not FULL_DUMP.exists():
        print("gen-api: build/api-cache/Full-API-Dump.json is missing; run without --offline", file=sys.stderr)
        return 1
    if not (SITE / "index.html").is_file():
        print("gen-api: no mirror under build/api-cache/ref; run tools/fetch-ref.py", file=sys.stderr)
        return 1

    guid, dotted = site_version()
    dump = json.loads(FULL_DUMP.read_text(encoding="utf-8"))
    class_docs = load_docs("classes")
    enum_docs = load_docs("enums")
    datatype_docs = load_docs("datatypes")
    library_docs = load_docs("libraries")
    global_docs = load_docs("globals")

    payload = {
        "version": guid or "",
        "build": dotted or "",
        "classes": build_classes(dump, class_docs),
        "enums": build_enums(dump, enum_docs),
        "datatypes": build_datatypes(datatype_docs),
        "libraries": build_libraries(library_docs),
        "globals": build_globals(global_docs),
    }

    # What a live client answered when asked. Documentation is not the engine.
    engine_added, engine_derived, undescribed = merge_engine(
        payload["libraries"], payload["datatypes"], payload["globals"])
    for name in undescribed:
        print(f"gen-api: {name} is in the engine and nothing describes it - add it to tools/engine-docs.json",
              file=sys.stderr)

    # Handed to JSON.parse rather than written as an object literal. A
    # megabyte of literal is a megabyte the JavaScript parser has to walk as
    # source; the same bytes as one string go through the JSON reader, which is
    # several times faster and is what decides how long a new pane takes to
    # become useful. `globalThis` rather than `window` so tests/gui can load
    # this exact file under jsc.
    def emit(target, global_name, value, what):
        text = json.dumps(value, separators=(",", ":"), ensure_ascii=False)
        literal = json.dumps(text)
        literal = literal.replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
        target.write_text(
            f"// Generated by tools/gen-api.py from the Roblox API Reference (robloxapi.github.io/ref, "
            f"build {dotted or guid}): {what}. Do not edit.\n"
            f"globalThis.{global_name} = JSON.parse(" + literal + ");\n",
            encoding="utf-8",
        )

    emit(TARGET, "NEXA_API", payload, "names, signatures and summaries")
    emit(TARGET_DOCS, "NEXA_API_DOCS", DOCS_OUT, "the full descriptions, read by the hover")

    members = sum(len(c["members"]) for c in payload["classes"].values())
    documented = sum(1 for c in payload["classes"].values() for m in c["members"] if m[6])
    hidden = sum(1 for c in payload["classes"].values() for m in c["members"]
                 if "NotScriptable" in m[5] or m[4])
    enum_items = sum(len(e["items"]) for e in payload["enums"].values())
    enum_documented = sum(1 for e in payload["enums"].values() for i in e["items"] if i[2])
    print(f"gen-api: build {dotted} ({guid}): {len(payload['classes'])} classes, {members} members "
          f"({documented} with a description, {hidden} needing setscriptable or an identity), "
          f"{len(payload['enums'])} enums with {enum_items} items ({enum_documented} described), "
          f"{len(payload['datatypes'])} datatypes, {len(payload['libraries'])} libraries, "
          f"{engine_added + engine_derived} members the engine has and the docs do not "
          f"({engine_derived} derived from a twin, {len(undescribed)} undescribed) "
          f"-> {TARGET.relative_to(ROOT)} ({TARGET.stat().st_size // 1024} KB), "
          f"{len(DOCS_OUT)} full descriptions -> {TARGET_DOCS.relative_to(ROOT)} "
          f"({TARGET_DOCS.stat().st_size // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
