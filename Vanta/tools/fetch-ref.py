#!/usr/bin/env python3
"""
Mirrors robloxapi.github.io/ref - the Roblox API Reference - into
build/api-cache/ref, and audits the editor's tables against it.

The site is a rendering of the two things the editor's tables are already
built from: the engine's API dump (every build since 2011, so it also keeps
what was removed) and Roblox's own creator-docs. It is not a third source of
descriptions - its search.db is a search index and resources.json an icon map -
but it is the reference people actually open: one page per class, enum and
type, every member with its description, tags, security and history. This
keeps a complete local copy, and reads that copy back to check that the editor
says what the page a person would open says.

    python3 tools/fetch-ref.py            # mirror what is not yet local (~1,900 pages)
    python3 tools/fetch-ref.py --fresh    # re-download everything
    python3 tools/fetch-ref.py --audit    # compare build/api-cache/ref against api.js
    python3 tools/fetch-ref.py --audit --strict   # ... and fail on a gap

The mirror is faithful: the site's own paths under build/api-cache/ref, links
left absolute (/ref/...), so it browses like the original when the cache
directory is served as a root:

    python3 -m http.server -d build/api-cache 8000    # then http://localhost:8000/ref/

Pages are ~100 KB to 8 MB of HTML each (the navigation and the history are
inlined) but 10-80 KB over the wire, so the transfer is asked for gzipped and
sixteen at a time; a cold mirror takes a minute or two.
"""

import gzip
import json
import re
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "build" / "api-cache" / "ref"
API_JS = ROOT / "gui" / "Resources" / "monaco" / "api.js"

BASE = "https://robloxapi.github.io/ref/"
WORKERS = 16

# What the sitemap does not list: the front pages, the feeds and the two data
# files the site's own JavaScript loads.
EXTRAS = [
    "index.html", "index.xml", "sitemap.xml", "404.html",
    "about.html", "changelog.html", "search.html", "stats.html",
    "updates/index.html", "class/index.html", "enum/index.html", "type/index.html",
    "resources.json", "search.db",
]

ASSET = re.compile(r"""/ref/((?:[A-Za-z0-9_\-./]|%[0-9A-Fa-f]{2})+?\.(?:css|js|png|svg|ico|gif|jpe?g|webp|woff2?|ttf|json|db|xml|txt))(?=["'()<> ])""")


# --------------------------------------------------------------------- fetching

def fetch(path, retries=3):
    url = BASE + path
    req = urllib.request.Request(url, headers={
        "User-Agent": "nexa-fetch-ref", "Accept": "*/*", "Accept-Encoding": "gzip",
    })
    delay = 1.0
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
                if r.headers.get("Content-Encoding", "") == "gzip":
                    data = gzip.decompress(data)
                return data
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise
            last = e
        except (urllib.error.URLError, OSError, EOFError) as e:
            last = e
        time.sleep(delay)
        delay *= 2
    raise last


def page_list():
    sitemap = fetch("sitemap.xml").decode("utf-8", "replace")
    pages = []
    for loc in re.findall(r"<loc>([^<]+)</loc>", sitemap):
        if loc.startswith(BASE):
            pages.append(loc[len(BASE):])
    return pages


def mirror(fresh):
    DEST.mkdir(parents=True, exist_ok=True)
    started = time.time()
    pages = page_list()
    wanted = list(dict.fromkeys(pages + EXTRAS))
    print(f"fetch-ref: {len(pages)} pages in the sitemap, {len(wanted)} files to mirror")

    failed = []
    fetched = [0]
    total = [0]

    def one(path):
        dest = DEST / path
        if not fresh and dest.is_file() and dest.stat().st_size > 0:
            return
        try:
            data = fetch(path)
        except Exception as e:  # noqa: BLE001 - one bad page must not sink the mirror
            failed.append((path, str(e)))
            return
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        fetched[0] += 1
        total[0] += len(data)
        if fetched[0] % 200 == 0:
            print(f"fetch-ref: {fetched[0]} files, {total[0] // (1 << 20)} MB")

    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        list(pool.map(one, wanted))

    # Second pass: everything the pages refer to - the hashed stylesheet and
    # scripts, the favicons, the class icons (and the hashed variants Hugo
    # made of them). Found by reading the mirror, so it stays complete when the
    # site changes its asset names.
    assets = set()
    for path in DEST.rglob("*"):
        if path.suffix not in (".html", ".css", ".json", ".xml"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for hit in ASSET.findall(text):
            if not hit.endswith(".html"):
                assets.add(hit)
    assets = sorted(a for a in assets if a not in wanted)
    print(f"fetch-ref: {len(assets)} assets referenced")
    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        list(pool.map(one, assets))

    manifest = {
        "source": BASE,
        "fetched": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "pages": len(pages),
        "files": len(wanted) + len(assets),
        "downloaded": fetched[0],
        "bytes": total[0],
        "seconds": round(time.time() - started, 1),
        "failed": failed,
    }
    (DEST / "MANIFEST.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"fetch-ref: {fetched[0]} downloaded ({total[0] // (1 << 20)} MB) in "
          f"{manifest['seconds']}s, {len(failed)} failed -> {DEST.relative_to(ROOT)}")
    for path, why in failed[:20]:
        print(f"fetch-ref:   failed {path}: {why}", file=sys.stderr)
    return 1 if failed else 0


# ---------------------------------------------------------------------- audit

def load_api():
    text = API_JS.read_text(encoding="utf-8")
    inner = text[text.index('JSON.parse("') + len('JSON.parse("'):text.rindex('")')]
    return json.loads(json.loads('"' + inner + '"'))


MEMBER = re.compile(r'<section id=member-([A-Za-z0-9_]+)(?: class="?([^">]*)"?)?>')
ENUM_ITEM = re.compile(r'<tr id=member-([A-Za-z0-9_]+)(?: class="?([^">]*)"?)?>(.*?)</tr>', re.S)
# A link to the entity's own page, not to a member of it: a removed *member*
# link carries `removed` too, and would mark its class removed.
NAV_LINK = re.compile(r'<a class="entity-link([^"]*)" href=/ref/(class|enum|type)/([A-Za-z0-9_]+)\.html(?=[ >"])')


def sections(html):
    """Each member section of a class page: name, status classes, its HTML."""
    hits = list(MEMBER.finditer(html))
    out = []
    for i, m in enumerate(hits):
        end = hits[i + 1].start() if i + 1 < len(hits) else len(html)
        out.append((m.group(1), m.group(2) or "", html[m.start():end]))
    return out


def item_rows(html):
    """Each item row of an enum page: name, status classes, its HTML."""
    return [(m.group(1), m.group(2) or "", m.group(3)) for m in ENUM_ITEM.finditer(html)]


def has_docs(section_html):
    """Whether the member's own docs block says anything. It is the first
    docs div in the section; a member with nothing carries an empty one."""
    first = re.search(r"<div class=docs>(.*?)</div>", section_html, re.S)
    return bool(first and "<p>" in first.group(1) and "no-desc" not in first.group(1))


def has_item_docs(row_html):
    cell = re.search(r"<td class=col-desc>(.*?)</td>", row_html, re.S)
    return bool(cell and cell.group(1).strip() and "no-desc" not in cell.group(1))


def audit(strict):
    if not (DEST / "index.html").is_file():
        print("fetch-ref: no mirror under build/api-cache/ref; run without --audit first", file=sys.stderr)
        return 1
    api = load_api()
    index = (DEST / "index.html").read_text(encoding="utf-8", errors="replace")

    # The site keeps every class and enum it ever saw; the navigation marks the
    # removed ones, and those are the ones the editor is right not to know.
    removed = {"class": set(), "enum": set(), "type": set()}
    present = {"class": set(), "enum": set(), "type": set()}
    for classes, kind, name in NAV_LINK.findall(index):
        (removed if "removed" in classes.split() else present)[kind].add(name)
    for kind in present:
        present[kind] -= removed[kind]
    # An entity with a page but no link on the front page (the site lists
    # every present class and enum there) is not present.

    report = {}

    # Classes and their members.
    missing_classes = sorted(c for c in present["class"] if c not in api["classes"] and c != "ROOT")
    missing_members, undocumented_here, site_members, site_documented = [], [], 0, 0
    for name in sorted(present["class"]):
        page = DEST / "class" / f"{name}.html"
        if name not in api["classes"] or not page.is_file():
            continue
        ours = {row[0]: row for row in api["classes"][name]["members"]}
        for member, status, body in sections(page.read_text(encoding="utf-8", errors="replace")):
            if "removed" in status.split():
                continue
            site_members += 1
            documented = has_docs(body)
            site_documented += documented
            if member not in ours:
                missing_members.append(f"{name}.{member}")
            elif documented and not ours[member][6]:
                undocumented_here.append(f"{name}.{member}")
    report["classes on the site"] = len(present["class"])
    report["classes missing here"] = missing_classes
    report["members on the site"] = site_members
    report["members the site describes"] = site_documented
    report["members missing here"] = missing_members
    report["described there, blank here"] = undocumented_here

    # Enums and their items.
    missing_enums = sorted(e for e in present["enum"] if e not in api["enums"])
    missing_items, blank_items, site_items = [], [], 0
    for name in sorted(present["enum"]):
        page = DEST / "enum" / f"{name}.html"
        if name not in api["enums"] or not page.is_file():
            continue
        entry = api["enums"][name]
        ours = {row[0]: row for row in entry["items"]} if isinstance(entry, dict) else {n: None for n in entry}
        for item, status, body in item_rows(page.read_text(encoding="utf-8", errors="replace")):
            if "removed" in status.split():
                continue
            site_items += 1
            if item not in ours:
                missing_items.append(f"Enum.{name}.{item}")
            elif has_item_docs(body) and ours[item] is not None and not ours[item][2]:
                blank_items.append(f"Enum.{name}.{item}")
    report["enums on the site"] = len(present["enum"])
    report["enums missing here"] = missing_enums
    report["enum items on the site"] = site_items
    report["enum items missing here"] = missing_items
    report["enum items described there, blank here"] = blank_items

    # Types: the site lists the reflection system's whole vocabulary (int,
    # bool, Tuple, QDir...). Only the ones a script can name matter, and those
    # are the datatypes; the rest is reported for the record, not as a gap.
    known_types = set(api["datatypes"]) | set(api["classes"]) | set(api["libraries"])
    other_types = sorted(t for t in present["type"] if t not in known_types)
    report["types on the site"] = len(present["type"])
    report["types that are not datatypes here"] = other_types

    gaps = 0
    for key, value in report.items():
        if isinstance(value, list):
            shown = ", ".join(value[:12]) + (", …" if len(value) > 12 else "")
            print(f"{key}: {len(value)}" + (f"  [{shown}]" if value else ""))
            if key.endswith("missing here") or key.endswith("blank here"):
                gaps += len(value)
        else:
            print(f"{key}: {value}")
    print(f"fetch-ref: audit against {DEST.relative_to(ROOT)}: {gaps} gaps")
    return 1 if (strict and gaps) else 0


def main(argv):
    if "--audit" in argv:
        return audit("--strict" in argv)
    return mirror("--fresh" in argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
