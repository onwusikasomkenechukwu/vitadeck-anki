"""
Anki .apkg -> .vitadeck exporter (format v1).

Stdlib only. See FORMAT.md for the package contract.

Usage:
    python exporter.py <input.apkg> <output_dir>

Output: <output_dir>/<deckname>.vitadeck/ per FORMAT.md.

Anki-format assumptions tagged INFERRED are documented in FORMAT.md
under "Open questions" or were verified empirically by inspecting
Linux+.apkg / EEI.apkg / Rubber_Gloving.apkg.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import io
import json
import re
import shutil
import sqlite3
import sys
import tempfile
import time
import zipfile
from collections import Counter
from pathlib import Path

FORMAT_VERSION = 1
EXPORTER_VERSION = "0.1.0"
FIELD_SEP = "\x1f"  # Anki's notes.flds separator (verified)
BATCH_SIZE = 500    # streaming batch into cards.sqlite


# ---------------------------------------------------------------------------
# .apkg unpacking + collection selection
# ---------------------------------------------------------------------------

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"  # standard zstd frame magic (confirmed from
                                  # real collection.anki21b / media headers).


def _require_zstandard():
    """Import zstandard, or exit with the exact install command (not a raw
    ImportError traceback)."""
    try:
        import zstandard
        return zstandard
    except ImportError:
        raise SystemExit(
            "This .apkg uses the zstd-compressed format (Anki >= 2.1.50:\n"
            "  collection.anki21b), which needs the 'zstandard' package.\n"
            "Install it into THIS Python and re-run:\n"
            "    python -m pip install zstandard\n"
            "(or: python -m pip install -r requirements.txt)"
        )


def _zstd_decompress_file(src: Path, dst: Path) -> None:
    """Stream-decompress a standalone zstd frame file to dst. Streaming avoids
    needing the frame's content-size header."""
    zstd = _require_zstandard()
    try:
        dctx = zstd.ZstdDecompressor()
        with open(src, "rb") as ifh, open(dst, "wb") as ofh:
            dctx.copy_stream(ifh, ofh)
    except zstd.ZstdError as e:
        raise SystemExit(
            f"Failed to decompress {src.name} (corrupt or truncated download?): {e}"
        )


def _parse_media_entries(raw: bytes) -> dict[str, str]:
    """Parse the new-format media map: a protobuf
        MediaEntries { repeated MediaEntry entries = 1; }
        MediaEntry   { string name = 1; uint32 size = 2; bytes sha1 = 3; }
    Returns {str(index): name} where index is the entry's order, which matches
    the numbered media files ("0", "1", ...) in the .apkg zip.

    Structure CONFIRMED by decompressing a real `media` blob (the leading bytes
    decode as field1/len-delimited entry -> field1 name string); only the field
    numbers below are taken from that observed layout, not a generated schema.
    """
    def read_varint(b: bytes, i: int) -> tuple[int, int]:
        shift = 0
        val = 0
        while i < len(b):
            byte = b[i]
            i += 1
            val |= (byte & 0x7F) << shift
            if not (byte & 0x80):
                break
            shift += 7
        return val, i

    out: dict[str, str] = {}
    i = 0
    idx = 0
    n = len(raw)
    while i < n:
        tag, i = read_varint(raw, i)
        field, wire = tag >> 3, tag & 7
        if field == 1 and wire == 2:                 # an entry sub-message
            length, i = read_varint(raw, i)
            entry = raw[i:i + length]
            i += length
            j = 0
            name = None
            while j < len(entry):
                t2, j = read_varint(entry, j)
                f2, w2 = t2 >> 3, t2 & 7
                if w2 == 2:
                    l2, j = read_varint(entry, j)
                    val = entry[j:j + l2]
                    j += l2
                    if f2 == 1:
                        name = val.decode("utf-8", "replace")
                elif w2 == 0:
                    _, j = read_varint(entry, j)
                elif w2 == 5:
                    j += 4
                elif w2 == 1:
                    j += 8
                else:
                    break
            if name is not None:
                out[str(idx)] = name
            idx += 1
        else:                                         # skip unknown top-level field
            if wire == 2:
                length, i = read_varint(raw, i)
                i += length
            elif wire == 0:
                _, i = read_varint(raw, i)
            elif wire == 5:
                i += 4
            elif wire == 1:
                i += 8
            else:
                break
    return out


def _load_media_map(dest: Path) -> dict:
    """Load the .apkg media map. Legacy: a JSON object {"0": name, ...}.
    New (>= 2.1.50): a zstd-compressed `MediaEntries` protobuf. Either failure
    degrades to an empty map (media simply goes unresolved) rather than crashing."""
    mfile = dest / "media"
    if not mfile.exists():
        return {}
    data = mfile.read_bytes()
    if not data:
        return {}
    if data[:4] == ZSTD_MAGIC:                        # new compressed-protobuf form
        zstd = _require_zstandard()
        try:
            reader = zstd.ZstdDecompressor().stream_reader(io.BytesIO(data))
            raw = reader.read()
        except zstd.ZstdError as e:
            print(f"warning: could not decompress media map ({e}); "
                  "media will be unresolved")
            return {}
        return _parse_media_entries(raw)
    try:                                              # legacy JSON form
        return json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        print(f"warning: media map is not JSON ({e}); media will be unresolved")
        return {}


def unpack_apkg(apkg_path: Path, dest: Path) -> dict:
    with zipfile.ZipFile(apkg_path) as zf:
        zf.extractall(dest)
    files = {p.name for p in dest.iterdir()}

    # New (Anki >= 2.1.50) format: collection.anki21b is a zstd-compressed
    # SQLite db. Decompress it and use the result; the decompressed bytes are a
    # standard SQLite database with the same schema as collection.anki21, so the
    # downstream read path is unchanged. (Modern exports also ship a tiny
    # collection.anki2 downgrade stub, which we ignore in favor of the real data.)
    if "collection.anki21b" in files:
        decompressed = dest / "collection.anki21.decompressed"
        _zstd_decompress_file(dest / "collection.anki21b", decompressed)
        chosen_path = decompressed
        chosen_name = "collection.anki21b"
    else:
        # Prefer .anki21 (real data) over .anki2 (legacy stub in modern exports).
        # Verified: .anki2 contains 1 placeholder note in all three test files.
        candidates = [n for n in ("collection.anki21", "collection.anki2")
                      if n in files]
        if not candidates:
            raise SystemExit(
                "no collection.anki21b / .anki21 / .anki2 in .apkg")
        chosen = candidates[0]
        # Sanity: if the "preferred" file is suspiciously small and the fallback
        # has more rows, prefer the fallback. Defensive against malformed exports.
        if len(candidates) > 1:
            size_primary = (dest / candidates[0]).stat().st_size
            size_secondary = (dest / candidates[1]).stat().st_size
            if size_primary < size_secondary / 4:
                chosen = candidates[1]
        chosen_path = dest / chosen
        chosen_name = chosen

    media_map = _load_media_map(dest)

    return {
        "collection_path": chosen_path,
        "collection_name": chosen_name,
        "media_map": media_map,
        "media_root": dest,
    }


# ---------------------------------------------------------------------------
# Template rendering: Mustache-style {{Field}}, {{FrontSide}}, conditionals
# ---------------------------------------------------------------------------

_COND_RE = re.compile(r"\{\{([#^/])([^}]+)\}\}")
_FIELD_RE = re.compile(r"\{\{([^}#^/][^}]*)\}\}")


def render_template(tmpl: str, fields: dict[str, str], front_side: str | None = None) -> str:
    """Resolve a single Anki template against a note's fields.

    Handles:
      - {{FieldName}}, {{text:FieldName}}, {{hint:FieldName}} (strip filter, keep value)
      - {{FrontSide}}  (substitutes the already-rendered front, for back templates)
      - {{#Field}}...{{/Field}}  (show block if Field is non-empty)
      - {{^Field}}...{{/Field}}  (show block if Field is empty)
    Does not implement: arbitrary filter chains, cloze inside conditionals
    beyond the standard cases. Sufficient for stock note types in test data.
    """
    # Process conditional blocks first. Iterative because they can nest.
    out = tmpl
    while True:
        new = _expand_conditionals(out, fields)
        if new == out:
            break
        out = new

    def field_sub(m: re.Match) -> str:
        name = m.group(1).strip()
        if name == "FrontSide":
            return front_side or ""
        # filter prefixes like "text:", "type:", "hint:" — keep raw value.
        if ":" in name:
            name = name.split(":", 1)[1]
        return fields.get(name, "")

    out = _FIELD_RE.sub(field_sub, out)
    return out


def _expand_conditionals(s: str, fields: dict[str, str]) -> str:
    # Find first matching pair and process it.
    m_open = _COND_RE.search(s)
    if not m_open:
        return s
    kind, name = m_open.group(1), m_open.group(2).strip()
    if kind == "/":
        return s  # orphan close, leave it; will be stripped later
    # Find the matching close for this name at the same nesting level.
    depth = 1
    pos = m_open.end()
    while pos < len(s):
        m = _COND_RE.search(s, pos)
        if not m:
            return s  # unbalanced; bail out
        k, n = m.group(1), m.group(2).strip()
        if n == name:
            if k in ("#", "^"):
                depth += 1
            elif k == "/":
                depth -= 1
                if depth == 0:
                    inner = s[m_open.end():m.start()]
                    val = fields.get(name, "").strip()
                    show = bool(val) if kind == "#" else not val
                    replacement = inner if show else ""
                    return s[:m_open.start()] + replacement + s[m.end():]
        pos = m.end()
    return s


# ---------------------------------------------------------------------------
# Cloze handling
# ---------------------------------------------------------------------------

_CLOZE_RE = re.compile(r"\{\{c(\d+)::(.*?)(?:::(.*?))?\}\}", re.DOTALL)


def apply_cloze(field_text: str, active_ord: int, reveal: bool) -> str:
    """Render a cloze field for one card.

    active_ord is the card's `ord` (= cloze_number - 1).
    If reveal is False (front): active cloze shown as [...] or [hint], others revealed.
    If reveal is True (back):   all clozes revealed.

    Verified against a SYNTHETIC cloze note (no real cloze model in test decks);
    flagged in manifest.cloze_verified.
    """
    active_n = active_ord + 1

    def repl(m: re.Match) -> str:
        n = int(m.group(1))
        answer = m.group(2)
        hint = m.group(3)
        if reveal:
            return answer
        if n == active_n:
            return f"[{hint}]" if hint else "[...]"
        return answer

    return _CLOZE_RE.sub(repl, field_text)


# ---------------------------------------------------------------------------
# HTML sanitization
# ---------------------------------------------------------------------------

# Keep these as-is. <img> is kept but its src is rewritten downstream.
# <hr> kept because Anki's stock back template uses <hr id=answer> as the
# front/back divider on every card — stripping it merges Q and A visually.
_KEEP_TAGS = {"br", "b", "i", "u", "img", "hr"}
# Remap to a kept tag with equivalent semantics.
_REMAP_TAGS = {"em": "i", "strong": "b"}
# These get converted to <br> on close (paragraph-ish containers).
_BLOCK_TAGS = {"div", "p"}

_TAG_RE = re.compile(r"<(/?)([a-zA-Z][a-zA-Z0-9]*)([^>]*)>", re.DOTALL)
_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)
_SCRIPT_RE = re.compile(r"<script.*?</script>", re.DOTALL | re.IGNORECASE)
_STYLE_RE = re.compile(r"<style.*?</style>", re.DOTALL | re.IGNORECASE)
_IMG_SRC_RE = re.compile(r'src\s*=\s*"([^"]+)"|src\s*=\s*\'([^\']+)\'', re.IGNORECASE)
_SOUND_RE = re.compile(r"\[sound:([^\]]+)\]")
_FONT_SIZE_RE = re.compile(r"font-size\s*:\s*(\d+)\s*px", re.IGNORECASE)

# Table / <pre> structured conversion (Patch 2). Line breaks use literal "\n",
# NOT <br>: the Vita's strip_html removes <br> as a tag, so only real newlines
# survive to its word-wrap renderer.
_TABLE_RE = re.compile(r"<table[^>]*>(.*?)</table>", re.DOTALL | re.IGNORECASE)
_TR_RE    = re.compile(r"<tr[^>]*>(.*?)</tr>", re.DOTALL | re.IGNORECASE)
_CELL_RE  = re.compile(r"<(th|td)[^>]*>(.*?)</(?:th|td)>", re.DOTALL | re.IGNORECASE)
_PRE_RE   = re.compile(r"<pre[^>]*>(.*?)</pre>", re.DOTALL | re.IGNORECASE)
_BR_RE    = re.compile(r"<br\s*/?>", re.IGNORECASE)


def _upper_outside_tags(s: str) -> str:
    """Uppercase text but leave anything inside <...> (tag names, img src,
    attributes) untouched — so uppercasing a <th> can't corrupt a media src."""
    out = []
    in_tag = False
    for ch in s:
        if ch == "<":
            in_tag = True
            out.append(ch)
        elif ch == ">":
            in_tag = False
            out.append(ch)
        else:
            out.append(ch if in_tag else ch.upper())
    return "".join(out)


def _convert_tables(text: str, warnings: Counter) -> str:
    """Convert <table> to a plain-text grid the Vita word-wrapper can show:
    cells joined by ' | ', rows by newline, a '---' divider after the last row,
    then a blank line. <th> cell text is uppercased. Not a real table renderer;
    a readable approximation that preserves cell content and row structure."""
    def repl(m: re.Match) -> str:
        warnings["converted_table"] += 1
        rows = []
        for rm in _TR_RE.finditer(m.group(1)):
            cells = []
            for cm in _CELL_RE.finditer(rm.group(1)):
                content = cm.group(2).strip()
                if cm.group(1).lower() == "th":
                    content = _upper_outside_tags(content)
                cells.append(content)
            rows.append(" | ".join(cells))   # join => no trailing ' | '
        if not rows:
            return ""
        return "\n" + "\n".join(rows) + "\n---\n\n"
    return _TABLE_RE.sub(repl, text)


def _convert_pre(text: str, warnings: Counter) -> str:
    """Preserve <pre> line structure (convert its <br> to real newlines and keep
    any literal newlines) and wrap it in [PRE]...[/PRE] sentinels the Vita
    reviewer renders as a bordered monospace-ish box. Inner formatting tags are
    left for the normal sanitizer (the Vita drops them to plain text)."""
    def repl(m: re.Match) -> str:
        warnings["converted_pre"] += 1
        return "[PRE]" + _BR_RE.sub("\n", m.group(1)) + "[/PRE]"
    return _PRE_RE.sub(repl, text)


def sanitize_html(text: str, media_rewrite: dict[str, str], warnings: Counter) -> str:
    """Strip to Vita allowlist. Returns sanitized HTML; mutates warnings."""
    # Drop scripts/styles/comments entirely (warn).
    if _SCRIPT_RE.search(text):
        warnings["dropped_script"] += 1
    if _STYLE_RE.search(text):
        warnings["dropped_style"] += 1
    text = _SCRIPT_RE.sub("", text)
    text = _STYLE_RE.sub("", text)
    text = _COMMENT_RE.sub("", text)

    # Structured conversions before generic tag handling (Patch 2): tables ->
    # plain-text grid, <pre> -> [PRE]...[/PRE] with preserved newlines.
    text = _convert_tables(text, warnings)
    text = _convert_pre(text, warnings)

    # Tracks each <span> open in order: True if we kept it (must emit </span>),
    # False if we stripped it (must also strip the matching close).
    span_open_kept: list[bool] = []

    def tag_sub(m: re.Match) -> str:
        closing = m.group(1) == "/"
        name = m.group(2).lower()
        attrs = m.group(3) or ""

        if name in _REMAP_TAGS:
            name = _REMAP_TAGS[name]

        if name == "span":
            # Passthrough font-size as a data attribute the Vita renderer
            # can read or ignore. Strip every other span attr and tag itself.
            if closing:
                if span_open_kept and span_open_kept.pop():
                    return "</span>"
                return ""  # matched a stripped open; drop silently
            fs = _FONT_SIZE_RE.search(attrs)
            if fs:
                span_open_kept.append(True)
                warnings["kept_span_font_size"] += 1
                return f'<span data-font-size="{fs.group(1)}">'
            span_open_kept.append(False)
            warnings["stripped_span"] += 1
            return ""

        if name in _KEEP_TAGS:
            if name == "img":
                # Rewrite src to hashed media name; drop other attrs.
                src_m = _IMG_SRC_RE.search(attrs)
                if not src_m:
                    warnings["img_without_src"] += 1
                    return ""
                src = src_m.group(1) or src_m.group(2)
                new_name = media_rewrite.get(src)
                if not new_name:
                    warnings["img_missing_media"] += 1
                    return f'<img src="{html.escape(src)}">'  # leave original for debug
                return f'<img src="{new_name}">'
            return f"</{name}>" if closing else f"<{name}>"

        if name in _BLOCK_TAGS:
            # Convert closing block tag to <br> to preserve paragraph breaks.
            warnings[f"converted_{name}"] += 1
            return "<br>" if closing else ""

        warnings[f"stripped_{name}"] += 1
        return ""

    text = _TAG_RE.sub(tag_sub, text)
    # Collapse runs of <br> down to at most two.
    text = re.sub(r"(?:<br>\s*){3,}", "<br><br>", text)
    return text.strip()


def rewrite_sound_refs(text: str, media_rewrite: dict[str, str], warnings: Counter) -> str:
    def repl(m: re.Match) -> str:
        orig = m.group(1)
        new = media_rewrite.get(orig)
        if not new:
            warnings["sound_missing_media"] += 1
            return m.group(0)
        return f"[sound:{new}]"
    return _SOUND_RE.sub(repl, text)


# ---------------------------------------------------------------------------
# Media: copy referenced files, hash-rename, return rewrite map
# ---------------------------------------------------------------------------

def collect_media_refs(conn: sqlite3.Connection) -> set[str]:
    refs: set[str] = set()
    img_attr_re = re.compile(r'<img[^>]*src=["\']([^"\']+)["\']', re.IGNORECASE)
    for (flds,) in conn.execute("SELECT flds FROM notes"):
        for m in img_attr_re.finditer(flds):
            refs.add(m.group(1))
        for m in _SOUND_RE.finditer(flds):
            refs.add(m.group(1))
    return refs


def build_media(
    media_root: Path,
    media_map: dict,
    referenced: set[str],
    out_media_dir: Path,
) -> tuple[dict[str, str], dict[str, str]]:
    """Returns (rewrite_map: orig_name -> hashed_name, index: hashed_name -> orig_name)."""
    out_media_dir.mkdir(parents=True, exist_ok=True)
    # Invert: original filename -> archive key (string of int).
    inverted = {v: k for k, v in media_map.items()}
    rewrite: dict[str, str] = {}
    index: dict[str, str] = {}
    missing: list[str] = []
    for orig in referenced:
        key = inverted.get(orig)
        if key is None:
            missing.append(orig)
            continue
        src = media_root / str(key)
        if not src.exists():
            missing.append(orig)
            continue
        h = hashlib.sha256()
        with open(src, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        ext = Path(orig).suffix.lower()
        new_name = h.hexdigest()[:16] + ext
        dst = out_media_dir / new_name
        if not dst.exists():
            shutil.copy2(src, dst)
        rewrite[orig] = new_name
        index[new_name] = orig
    if missing:
        # Surfaced via warnings counter at sanitize-time too; this is a top-level note.
        pass
    return rewrite, index


# ---------------------------------------------------------------------------
# New-schema (Anki col.ver >= 18, the .anki21b format) collection metadata.
#
# In the old schema, col.models / col.decks are JSON blobs. In the new schema
# those columns are empty and the data lives in relational tables (notetypes,
# fields, templates, decks) whose `config` columns are protobuf blobs. We pull
# the few values the exporter needs out of those blobs and rebuild the same
# dict shapes the rest of export() already consumes. Field numbers below were
# read from REAL DAT Bootcamp blobs (dumped, not taken from a compiled .proto):
#   notetype config: field 1 = kind (1=cloze), field 3 = css
#   template config: field 1 = qfmt,           field 2 = afmt
# ---------------------------------------------------------------------------

def _unicase_cmp(a: str, b: str) -> int:
    """Stand-in for Anki's custom `unicase` collation, which the new-schema
    indexes reference (sqlite refuses to query those tables without it). A
    casefold comparison is good enough — the exporter does its own ORDER BY."""
    a, b = a.casefold(), b.casefold()
    return (a > b) - (a < b)


def _pb_top_fields(blob: bytes) -> dict:
    """Top-level protobuf fields of `blob` as {field_number: int|bytes}.
    Varint -> int, length-delimited -> bytes; repeated fields keep the last."""
    out: dict[int, object] = {}
    i, n = 0, len(blob)

    def varint(b: bytes, i: int) -> tuple[int, int]:
        s = v = 0
        while i < len(b):
            x = b[i]
            i += 1
            v |= (x & 0x7F) << s
            if not x & 0x80:
                break
            s += 7
        return v, i

    while i < n:
        tag, i = varint(blob, i)
        f, w = tag >> 3, tag & 7
        if w == 2:
            ln, i = varint(blob, i)
            out[f] = blob[i:i + ln]
            i += ln
        elif w == 0:
            out[f], i = varint(blob, i)
        elif w == 5:
            i += 4
        elif w == 1:
            i += 8
        else:
            break
    return out


def _pb_str(v) -> str:
    return v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray)) else ""


def _models_from_tables(conn: sqlite3.Connection) -> dict:
    models: dict[str, dict] = {}
    for nt in conn.execute("SELECT id, name, config FROM notetypes"):
        cfg = _pb_top_fields(nt["config"])
        kind = cfg.get(1, 0)              # field 1 varint: 1 = cloze (read from blob)
        css = _pb_str(cfg.get(3, b""))    # field 3 bytes: css        (read from blob)
        flds = [{"name": r["name"]} for r in conn.execute(
            "SELECT name FROM fields WHERE ntid=? ORDER BY ord", (nt["id"],))]
        tmpls = []
        for t in conn.execute(
                "SELECT name, config FROM templates WHERE ntid=? ORDER BY ord",
                (nt["id"],)):
            tc = _pb_top_fields(t["config"])
            tmpls.append({                # field 1 = qfmt, field 2 = afmt (read from blob)
                "name": t["name"],
                "qfmt": _pb_str(tc.get(1, b"")),
                "afmt": _pb_str(tc.get(2, b"")),
            })
        models[str(nt["id"])] = {
            "name": nt["name"],
            "type": int(kind),
            "flds": flds,
            "tmpls": tmpls,
            "css": css,
        }
    return models


def _decks_from_tables(conn: sqlite3.Connection) -> dict:
    # New-schema deck names use \x1f as the hierarchy separator; the legacy JSON
    # used "::". Convert so derive_deck_tree's "::" split still works.
    decks: dict[str, dict] = {}
    for r in conn.execute("SELECT id, name FROM decks"):
        decks[str(r["id"])] = {"name": r["name"].replace("\x1f", "::")}
    return decks


def load_models_and_decks(conn: sqlite3.Connection, col_row) -> tuple[dict, dict]:
    """Return (models, decks) in the legacy dict shape, for either schema."""
    if col_row["models"]:                      # legacy: JSON blobs in col
        return json.loads(col_row["models"]), json.loads(col_row["decks"])
    return _models_from_tables(conn), _decks_from_tables(conn)  # new relational schema


# ---------------------------------------------------------------------------
# Main export
# ---------------------------------------------------------------------------

def derive_deck_tree(decks: dict) -> list[dict]:
    by_name = {d["name"]: int(did) for did, d in decks.items()}
    out = []
    for did, d in decks.items():
        name = d["name"]
        parent = None
        if "::" in name:
            parent_name = name.rsplit("::", 1)[0]
            parent = by_name.get(parent_name)
        out.append({"id": int(did), "name": name, "parent_id": parent})
    out.sort(key=lambda x: x["name"])
    return out


def export(apkg_path: Path, output_dir: Path, cloze_verified: bool = False) -> dict:
    apkg_path = apkg_path.resolve()
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    apkg_sha = _file_sha256(apkg_path)
    deck_stem = apkg_path.stem
    out_root = output_dir / f"{deck_stem}.vitadeck"
    if out_root.exists():
        shutil.rmtree(out_root)
    out_root.mkdir()
    (out_root / "media").mkdir()

    with tempfile.TemporaryDirectory(prefix="vitadeck_") as tmp:
        tmp_path = Path(tmp)
        info = unpack_apkg(apkg_path, tmp_path)
        coll_path = info["collection_path"]
        media_map = info["media_map"]
        media_root = info["media_root"]

        conn = sqlite3.connect(coll_path)
        conn.row_factory = sqlite3.Row
        # New-schema (.anki21b) tables reference a custom `unicase` collation;
        # register a stand-in so those tables can be queried. Harmless on the
        # legacy schema (which never invokes it).
        conn.create_collation("unicase", _unicase_cmp)

        col_row = conn.execute(
            "SELECT ver, crt, mod, scm, models, decks FROM col"
        ).fetchone()
        anki_ver = col_row["ver"]
        # Legacy schema: JSON blobs in col. New schema (ver>=18): rebuild from
        # the notetypes/fields/templates/decks tables. See load_models_and_decks.
        models, decks = load_models_and_decks(conn, col_row)

        # ---- media ----
        referenced = collect_media_refs(conn)
        media_rewrite, media_index = build_media(
            media_root, media_map, referenced, out_root / "media"
        )

        # ---- cards.sqlite + tags.sqlite ----
        cards_db_path = out_root / "cards.sqlite"
        cards_db = sqlite3.connect(cards_db_path)
        cards_db.execute("PRAGMA journal_mode=WAL")
        cards_db.executescript("""
            CREATE TABLE cards (
                id INTEGER PRIMARY KEY,
                note_id INTEGER NOT NULL,
                deck_id INTEGER NOT NULL,
                note_type_id INTEGER NOT NULL,
                ord INTEGER NOT NULL,
                front_html TEXT NOT NULL,
                back_html TEXT NOT NULL,
                tags TEXT NOT NULL,
                sort_key TEXT NOT NULL
            );
            CREATE INDEX cards_by_deck ON cards(deck_id);
            CREATE INDEX cards_by_note ON cards(note_id);
        """)

        tags_db_path = out_root / "tags.sqlite"
        tags_db = sqlite3.connect(tags_db_path)
        tags_db.execute("PRAGMA journal_mode=WAL")
        tags_db.executescript("""
            CREATE TABLE tags (
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL UNIQUE
            );
            CREATE TABLE card_tags (
                card_id INTEGER NOT NULL,
                tag_id INTEGER NOT NULL,
                PRIMARY KEY (card_id, tag_id)
            );
            CREATE INDEX card_tags_by_tag ON card_tags(tag_id);
            CREATE INDEX card_tags_by_card ON card_tags(card_id);
            CREATE INDEX tags_by_name ON tags(name);
        """)

        warnings_per_card: dict[int, dict[str, int]] = {}
        stripped_summary: Counter = Counter()
        tag_cache: dict[str, int] = {}
        cards_inserted = 0

        # Stream notes joined to cards in batches.
        # We process per-note (a note may have multiple cards in cloze models).
        note_q = conn.execute(
            "SELECT id, guid, mid, tags, flds FROM notes ORDER BY id"
        )
        card_q_sql = (
            "SELECT id, nid, did, ord FROM cards WHERE nid = ? ORDER BY ord"
        )

        batch_cards: list[tuple] = []
        batch_card_tags: list[tuple[int, int]] = []

        def flush():
            nonlocal batch_cards, batch_card_tags
            if batch_cards:
                cards_db.executemany(
                    "INSERT INTO cards VALUES (?,?,?,?,?,?,?,?,?)",
                    batch_cards,
                )
                cards_db.commit()
                batch_cards = []
            if batch_card_tags:
                tags_db.executemany(
                    "INSERT OR IGNORE INTO card_tags VALUES (?,?)",
                    batch_card_tags,
                )
                tags_db.commit()
                batch_card_tags = []

        def tag_id(name: str) -> int:
            if name in tag_cache:
                return tag_cache[name]
            cur = tags_db.execute(
                "INSERT OR IGNORE INTO tags(name) VALUES (?)", (name,)
            )
            if cur.lastrowid and cur.rowcount:
                tid = cur.lastrowid
            else:
                tid = tags_db.execute(
                    "SELECT id FROM tags WHERE name=?", (name,)
                ).fetchone()[0]
            tag_cache[name] = tid
            return tid

        for note in note_q:
            mid = note["mid"]
            model = models[str(mid)]
            field_names = [f["name"] for f in model["flds"]]
            field_values = note["flds"].split(FIELD_SEP)
            fields = dict(zip(field_names, field_values))
            is_cloze = model.get("type", 0) == 1
            tags_str = note["tags"].strip()
            tag_list = tags_str.split() if tags_str else []

            cards_for_note = conn.execute(card_q_sql, (note["id"],)).fetchall()
            for card in cards_for_note:
                wcount: Counter = Counter()
                ord_ = card["ord"]

                if is_cloze:
                    # Single template; apply cloze to each field referenced in it.
                    tmpl = model["tmpls"][0]
                    qfmt = tmpl["qfmt"]
                    afmt = tmpl["afmt"]
                    front_fields = {k: apply_cloze(v, ord_, reveal=False)
                                    for k, v in fields.items()}
                    back_fields = {k: apply_cloze(v, ord_, reveal=True)
                                   for k, v in fields.items()}
                    front_raw = render_template(qfmt, front_fields)
                    back_raw = render_template(afmt, back_fields, front_side=front_raw)
                else:
                    if ord_ >= len(model["tmpls"]):
                        wcount["template_ord_out_of_range"] += 1
                        continue
                    tmpl = model["tmpls"][ord_]
                    front_raw = render_template(tmpl["qfmt"], fields)
                    back_raw = render_template(
                        tmpl["afmt"], fields, front_side=front_raw
                    )

                # Media: rewrite [sound:] in raw text, sanitize takes care of <img>.
                front_raw = rewrite_sound_refs(front_raw, media_rewrite, wcount)
                back_raw = rewrite_sound_refs(back_raw, media_rewrite, wcount)
                front_html = sanitize_html(front_raw, media_rewrite, wcount)
                back_html = sanitize_html(back_raw, media_rewrite, wcount)

                sort_key = _plain_text(field_values[0] if field_values else "")[:200].lower()

                batch_cards.append((
                    card["id"], note["id"], card["did"], mid, ord_,
                    front_html, back_html, tags_str, sort_key,
                ))
                for t in tag_list:
                    batch_card_tags.append((card["id"], tag_id(t)))

                if wcount:
                    warnings_per_card[card["id"]] = dict(wcount)
                    stripped_summary.update(wcount)
                cards_inserted += 1

                if len(batch_cards) >= BATCH_SIZE:
                    flush()

        flush()

        # ---- decks.json ----
        deck_tree = derive_deck_tree(decks)
        (out_root / "decks.json").write_text(
            json.dumps({"decks": deck_tree}, indent=2), encoding="utf-8"
        )

        # ---- note_types.json (informational) ----
        nt_out = {}
        for mid, m in models.items():
            nt_out[mid] = {
                "id": int(mid),
                "name": m["name"],
                "type": m["type"],
                "fields": [f["name"] for f in m["flds"]],
                "templates": [
                    {"name": t["name"], "qfmt": t["qfmt"], "afmt": t["afmt"]}
                    for t in m["tmpls"]
                ],
                "css": m.get("css", ""),
            }
        (out_root / "note_types.json").write_text(
            json.dumps(nt_out, indent=2), encoding="utf-8"
        )

        # ---- media_index.json ----
        (out_root / "media_index.json").write_text(
            json.dumps(media_index, indent=2), encoding="utf-8"
        )

        # reviews.jsonl is created lazily by the Vita on first grade — see
        # FORMAT.md sec "reviews.jsonl". No exporter-side artifact needed.

        # ---- export_warnings.json ----
        (out_root / "export_warnings.json").write_text(
            json.dumps({
                "summary": dict(stripped_summary),
                "per_card": warnings_per_card,
            }, indent=2),
            encoding="utf-8",
        )

        # ---- manifest.json ----
        n_notes = conn.execute("SELECT COUNT(*) FROM notes").fetchone()[0]
        manifest = {
            "format_version": FORMAT_VERSION,
            "exporter_version": EXPORTER_VERSION,
            "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "deck_name": deck_stem,
            "source": {
                "apkg_filename": apkg_path.name,
                "apkg_sha256": apkg_sha,
                "inner_collection": info["collection_name"],
                "anki_schema_ver": anki_ver,
            },
            "counts": {
                "decks": len(deck_tree),
                "note_types": len(models),
                "notes": n_notes,
                "cards": cards_inserted,
                "media": len(media_index),
                "media_referenced_but_missing":
                    len(referenced) - len(media_rewrite),
            },
            "cloze_verified": cloze_verified,
            "stripped_html_tag_summary": dict(stripped_summary),
        }
        (out_root / "manifest.json").write_text(
            json.dumps(manifest, indent=2), encoding="utf-8"
        )

        # Flip journal_mode back to DELETE before close. Vita's SceSqlite
        # is older and rejects WAL-format files (header byte 18 == 2)
        # with SQLITE_NOTADB at prepare time. DELETE writes header byte 18=1.
        for db in (cards_db, tags_db):
            db.execute("PRAGMA journal_mode=DELETE")
            db.commit()
        cards_db.close(); tags_db.close(); conn.close()

    return {"out_root": str(out_root), "manifest": manifest}


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _file_sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _plain_text(s: str) -> str:
    s = _TAG_RE.sub("", s)
    s = html.unescape(s)
    return re.sub(r"\s+", " ", s).strip()


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("apkg", type=Path)
    p.add_argument("out", type=Path)
    p.add_argument("--cloze-verified", action="store_true",
                   help="set manifest.cloze_verified=true (only after running the cloze test)")
    args = p.parse_args(argv)
    result = export(args.apkg, args.out, cloze_verified=args.cloze_verified)
    print(f"wrote {result['out_root']}")
    print(json.dumps(result["manifest"]["counts"], indent=2))
    if result["manifest"]["stripped_html_tag_summary"]:
        print("stripped/converted HTML tags:",
              result["manifest"]["stripped_html_tag_summary"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
