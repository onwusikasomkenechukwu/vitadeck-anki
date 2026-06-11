# `.vitadeck` Package Format — v1

Frozen interchange format between the desktop Python exporter and the Vita review client. Both sides build against this document; neither side parses `.apkg` directly.

## Source format — what we observed in `Linux+.apkg`

Verified by inspection (`_inspect/`):

- `.apkg` is a plain zip containing: one or both of `collection.anki2` (legacy v1 stub) and `collection.anki21` (real SchedulerV2 data, `col.ver = 11`); numbered files `0`, `1`, … (raw media bytes); and a plaintext JSON file literally named `media` mapping `"<n>"` → original filename.
- In this `.apkg` the `.anki2` file has 1 placeholder row; `.anki21` has 416 notes. **Rule:** the exporter prefers `.anki21`, falls back to `.anki2` when no `.anki21` is present (older Anki 2.0 compat exports — verified: all four `eng_*_clozes.apkg` test files use this shape). `.anki21b` is zstd-compressed (Anki ≥ 2.1.50) and *not supported* in v0.1; the exporter detects and errors with the exact menu path the user must follow to re-export.
- `col.models` and `col.decks` are JSON blobs. Each model has `flds[]`, `tmpls[]` (qfmt/afmt Mustache-style), `css`, `type` (0=standard, 1=cloze).
- `notes.flds` is a single string with **U+001F (Unit Separator)** between field values. Field values contain HTML.
- Media refs in fields use the **original** filename: `<img src="img1573…png">`, `[sound:foo.mp3]`. The numeric archive entry holding the bytes is found via the inverted `media` map.
- Cloze: **not verified — no cloze model in test deck.** Documented format: model `type: 1`, single template, generates one card per distinct `cN` index. Card `ord = N - 1`. Syntax `{{cN::answer}}` or `{{cN::answer::hint}}`. Active cloze on Q rendered as `[…]` (or hint), revealed on A. Other clozes shown as plain answer text on both sides. Flagged for empirical verification before Stage 1 ships.

## Container

A **directory** named `<deckname>.vitadeck` (optionally distributable as a zip the user expands once on the SD card). Rationale: media is already compressed (png/jpg/webp/mp3); the Vita streams files lazily; zip seeks hurt random access; a folder lets the SQLite file be opened directly by vitaSDK's sqlite without a temp extract step.

## Layout

```
mydeck.vitadeck/
  manifest.json          ← required, read first
  cards.sqlite           ← all card content, indexable
  decks.json             ← deck tree
  note_types.json        ← source model metadata (informational; Vita does not render templates)
  media/                 ← flat dir, files named by content hash
  reviews.sqlite         ← empty on export; Vita appends; desktop importer drains
  export_warnings.json   ← per-card list of stripped HTML / unsupported features
```

## `manifest.json`

```json
{
  "format_version": 1,
  "exporter_version": "0.1.0",
  "created_utc": "2026-06-09T18:00:00Z",
  "source": {
    "apkg_filename": "Linux+.apkg",
    "apkg_sha256": "…",
    "inner_collection": "collection.anki21",
    "anki_schema_ver": 11
  },
  "counts": { "decks": 27, "note_types": 2, "notes": 416, "cards": 416, "media": 29 },
  "stripped_html_tag_summary": { "div": 1240, "span": 87, "font": 12 }
}
```

`format_version` is **frozen**. Additive-only changes mean v1 readers must keep working on v1 packages forever; any breaking change bumps to v2 and is signaled here.

## Format versioning contract

The current format version is **1**. The exporter writes `"format_version": 1` in `manifest.json` (it already does — `exporter.py` emits `FORMAT_VERSION = 1`). The Vita reads `format_version` on deck load and compares it against its compiled-in `VITADECK_FORMAT_VERSION` constant (`vita_app2/main.cpp`).

Rules applied at load time (after the user selects a deck):

- **Absent** — if `format_version` is missing from the manifest, treat it as version 1 and continue, logging a warning.
- **Equal** — if `format_version` equals `VITADECK_FORMAT_VERSION`, proceed normally.
- **Higher** — if `format_version` is greater than `VITADECK_FORMAT_VERSION`, the deck was made by a newer exporter this build cannot safely read. Show a **blocking error screen** — `DECK FORMAT TOO NEW` / `Re-export with a newer VitaDeck`, Bold 24px on a Hot Red hard-shadow card — wait for any button press, and return to the picker. Never proceed or crash.
- **Lower (within tolerance)** — if `format_version` is less than `VITADECK_FORMAT_VERSION` but within the reader's back-compat tolerance (currently: any version-1 deck is compatible with a version-1 reader), proceed, logging a warning.

**Additive-only rule.** Future changes that only *add optional fields* increment the **minor** version and remain backwards compatible — older readers ignore the new fields and keep working. Changes that *alter or remove* an existing field increment the **major** version (`VITADECK_FORMAT_VERSION`) and trigger the hard "too new" incompatibility check above. The major version is the integer stored in `format_version`.

## `cards.sqlite`

One file. Vita opens it with `sqlite3_open_v2(..., READONLY)`.

```sql
CREATE TABLE cards (
  id         INTEGER PRIMARY KEY,   -- stable: Anki card id
  note_id    INTEGER NOT NULL,
  deck_id    INTEGER NOT NULL,
  note_type_id INTEGER NOT NULL,
  ord        INTEGER NOT NULL,      -- template index (0 for Basic, cloze_n-1 for cloze)
  front_html TEXT NOT NULL,         -- fully resolved, ready to render
  back_html  TEXT NOT NULL,
  tags       TEXT NOT NULL,         -- space-separated, leading+trailing space preserved (Anki convention)
  sort_key   TEXT NOT NULL          -- for "browse" ordering on Vita
);
CREATE INDEX cards_by_deck ON cards(deck_id);
CREATE INDEX cards_by_note ON cards(note_id);
```

Design choices:

- **Pre-resolved front/back.** Templates, `{{FrontSide}}`, conditional `{{#Field}}…{{/Field}}` blocks, and cloze deletions are all expanded at export time. The Vita never sees a template. Tradeoff: package is larger (full back includes the front again); upside: zero template engine, zero Mustache parser, zero cloze logic on the Vita.
- **HTML allowlist.** Kept verbatim: `<br>`, `<b>`, `<i>`, `<u>`, `<img>`, `<hr>`. Remapped: `<em>` → `<i>`, `<strong>` → `<b>`. Block-level: `<div>` and `<p>` are stripped on open, replaced by `<br>` on close (preserves paragraph breaks; verified on 4,336 `<div>` occurrences across the test decks). `<span>` is **only** kept when it carries an inline `style="font-size: Npx"`, in which case all other attributes are stripped and the tag becomes `<span data-font-size="N">…</span>` — see "Span font-size passthrough" below. All other `<span>` is stripped (tag removed, inner text kept). Everything else — `<style>`, `<script>`, `<font>`, `<table>`, `<pre>`, MathJax, anchors, headings — is removed and logged in `export_warnings.json`. Tradeoff: some decks will look plainer than in Anki; we deliberately accept this rather than ship an HTML/CSS engine on the Vita (matches PDF "Explicit Non-Goals").
- **SQLite over JSON.** A 416-card deck is fine either way, but real users will have 10k–50k cards; loading a single 20 MB JSON into Vita RAM is hostile. SQLite gives indexed access for "give me deck X's due cards" without parsing everything.
- **`sort_key`** is the cleaned plaintext of the first field (matching Anki's `sfld`), lowercased. Lets the Vita browse view sort without re-parsing HTML.

## `decks.json`

```json
{
  "decks": [
    { "id": 1686161840237, "name": "Linux+", "parent_id": null },
    { "id": 1686161840238, "name": "Linux+::Book", "parent_id": 1686161840237 },
    { "id": 1686161840239, "name": "Linux+::Book::1.1 Linux+ ch1 sec1 / A Pre-Assessment Exam", "parent_id": 1686161840238 }
  ]
}
```

Parent inferred from the `::` hierarchy in the original deck name. Full name kept verbatim so a Vita "show full path" toggle is trivial.

## `note_types.json`

Informational only — useful for the desktop importer that will eventually re-merge review logs into Anki. Vita does not consume this for rendering. Contains: model id, name, type (0/1), field names, original `qfmt`/`afmt` templates, and `css` (kept as text, never executed).

## `media/`

Flat directory. Files renamed to `<sha256-first-16-hex><.ext>` (extension lowercased, preserved from the original). Same bytes → same name → automatic dedup. Field references in `cards.sqlite.front_html/back_html` are rewritten to match: `<img src="a3f1…b7.png">`, `[sound:c2d4…ef.mp3]`. The Vita resolves `src` directly against `media/`.

Why hashes, not original names? Original names can collide across `.apkg`s the user imports, contain spaces/unicode/control chars that fight POSIX filesystems on the Vita SD card, and double up identical assets (the test deck has two pairs of identical `.png` files at sizes 646853 — likely dupes). Hashing fixes all three.

## `reviews.jsonl`

Append-only log. One JSON object per line, no trailing comma, no wrapping array. The Vita opens it in `"a"` mode and appends one line per grade. The desktop importer reads sequentially, dedupes on `(card_id, reviewed_at)`, and either deletes the file after merging or moves it aside.

```json
{"card_id":1653488159480,"reviewed_at":1781118177,"grade":3,"response_ms":133099}
```

Field contract (frozen v1):

- `card_id` — joins `cards.sqlite.cards.id`.
- `reviewed_at` — epoch seconds derived from `sceRtcGetCurrentClockLocalTime` + `sceRtcGetTime_t`. *Local-as-if-UTC*, not true UTC. Importer must compensate using the Vita's stored timezone or treat deltas as ground truth.
- `grade` — 1 = Again (X), 2 = Hard (O), 3 = Good (Square), 4 = Easy (Triangle).
- `response_ms` — milliseconds from "back shown" to grade button press, measured via `sceKernelGetProcessTimeWide` (microseconds since process start).

### Why JSONL, not SQLite

Initial spec called for `reviews.sqlite`. Verified empirically that writing through Sony's `SceSqlite` system module to a file on `ux0:data/...` hard-crashes the app (C2-12828-1). The crash is reproducible enough that the fix was a format change, not a workaround. JSONL via plain `fopen`/`fclose` (newlib → `sceIo`) is reliable, append-safe under power loss, trivial for the desktop importer to parse, and removes the cards-read vs reviews-write contention on the single SceSqlite module.

The exporter no longer pre-creates `reviews.sqlite` — the Vita creates `reviews.jsonl` on first grade. Scheduling is still **not** in this file. The Vita records raw events; desktop Anki replays them through its own scheduler (matches PDF "Preferred Model").

## `export_warnings.json`

Per-card list of `{card_id, dropped_tags: [...], notes: "..."}`. Lets the user (and Stage 1 verification) see exactly what the renderer simplification cost them.

## Audio `[sound:…]` — intentional sideload hook

`[sound:…]` references in resolved HTML are **not** dropped when the source `.apkg` ships without media (verified: all four `eng_*_clozes.apkg` test decks ship with empty media maps and 20k–29k sound references each).

The contract is:

1. At export time, every `[sound:original_name.mp3]` is rewritten to `[sound:<hash16>.mp3]` if the media archive contains the file, or **left with the original name** if it doesn't.
2. At review time, the Vita renderer looks up the filename inside the `[sound:…]` marker against `<deck>.vitadeck/media/`. If the file is present, play. If absent, skip silently.

This is **not graceful degradation** — it is the intentional sync model. It lets the user ship `.vitadeck` packages without audio (small, fast transfer) and sideload the audio bundle to `media/` later via USB or SD card. The `[sound:…]` markers serve as the binding contract between the deck and the audio assets; either filename scheme (hashed or original) resolves the same way on the Vita side, because the renderer treats `media/` as a flat content-addressable lookup.

Implications for the Vita client:
- Audio playback must be a non-fatal lookup, not an asset-required render path.
- `media_index.json` lists every file the exporter **did** rewrite to a hash name. Anything in a `[sound:…]` not present in that index is an original-name reference the renderer should look up directly (or skip if not on disk).

## Span font-size passthrough

`<span style="font-size: Npx">…</span>` is the one inline-CSS pattern preserved through the pipeline. The exporter:

1. Extracts the integer pixel value from the `font-size` declaration (case-insensitive, tolerates whitespace).
2. Drops every other span attribute (`style` body, `class`, `id`, `lang`, etc.).
3. Emits `<span data-font-size="N">…</span>` into `front_html`/`back_html`.

Rationale: 80k–118k spans across the four cloze decks carry only `font-size`. Dropping the size information makes the Vita render the native-language gloss at the same size as the target sentence, which is visually wrong. A `data-` attribute is HTML-spec valid, ignored by any renderer that doesn't care, and trivial for the Vita's text layer to read.

Frozen contract: **only `font-size`** is passed through in v1, and only when expressed in `px`. Other CSS declarations (`color`, `background`, `font-weight`, percentage/em units) are dropped. Adding more passthrough attributes is reserved for a future format version bump. A renderer that ignores `data-font-size` produces correct text at default size — no required behavior, just a hint.

## What's explicitly out of v1

- Scheduling state (due date, ease factor, interval). Belongs to desktop Anki; the Vita doesn't reschedule.
- FSRS parameters.
- CSS. The model's `css` is preserved in `note_types.json` for round-tripping but the Vita ignores it.
- LaTeX, MathJax, audio/video other than mp3/ogg/wav, `<table>`, `<iframe>`.
- Editing. The package is read-only on the Vita except for `reviews.sqlite`.

These map directly to the PDF's "Explicit Non-Goals" and "Excluded" sections.

## Open questions before Stage 1

1. **Cloze verification.** I have no cloze note in `Linux+.apkg`. Either supply a small cloze `.apkg` for testing, or I'll make one in Anki and document the round-trip in Stage 1.
2. **`.anki21b` (zstd) support.** Not present in the test file. Stage 1 will include the decompression path but it can only be unit-tested against a synthetic file unless you have a modern export handy.
3. **Tags as deck filter, or separate?** Currently I'm keeping tags on each card and not surfacing them as their own browse axis — confirm that matches your Vita UX plan.
