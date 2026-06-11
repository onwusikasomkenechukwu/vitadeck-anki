# VitaDeck

**Anki-compatible flashcard review client for the PlayStation Vita, with a desktop exporter/importer.**

VitaDeck turns a PS Vita into a dedicated spaced-repetition machine. You export your Anki
decks on the desktop into a frozen, Vita-friendly package format, review them on the go with
a neo-brutalist UI (gamification, per-deck stats, audio, images, session resume), and replay
your grades back into your real Anki collection through Anki's own scheduler.

It runs fully offline — no AnkiWeb, no network, no account.

---

## How it works

```
 .apkg ──exporter.py──►  <deck>.vitadeck  ──USB──►  ux0:data/vitadeck/  ──►  Vita reviewer
                          (cards.sqlite, media,                                   │ grades
                           manifest.json, ...)                                    ▼
 Anki collection  ◄──importer.py──  reviews.jsonl  ◄──USB──  ux0:data/vitadeck/<deck>/
```

Two codebases meet at the **`.vitadeck`** package format (see [`FORMAT.md`](FORMAT.md), the
frozen v1 spec):

- **Desktop (Python, stdlib + `zstandard`):**
  - `exporter.py` — reads `.apkg` (incl. modern zstd `.anki21b`), resolves templates/cloze/HTML
    to ready-to-render front/back, hashes media, and writes a `.vitadeck` folder.
  - `importer.py` — drains `reviews.jsonl` and replays each grade into a live `collection.anki2`
    via `col.sched.answerCard()`.
- **Vita (C++ / [vitaSDK](https://vitasdk.org)):** `vita_app2/` — opens `cards.sqlite` read-only,
  renders cards, records grades to `reviews.jsonl`.

## Features

- **Deck picker** — scans `ux0:data/vitadeck/` for `.vitadeck` folders; D-pad / left-stick nav.
- **Reviewer** — cloze rendering, word-wrapped text, image cards (32-entry texture LRU),
  `<pre>` code blocks, table→text, long-answer scrolling, and an on-screen grade-button row.
- **Gamification** — streaks (with freeze tokens), XP/levels, daily missions, achievements,
  a session-summary screen (`player.json`).
- **Per-deck stats** — today's summary cards + a 90-day activity heatmap with a day cursor.
- **Session resume** — `progress.json` lets you pick up mid-deck.
- **Audio** — SFX + streaming background music (user-supplied; see *Assets* below).
- **Neo-brutalist UI** — cream + halftone background, hard shadows, Space Grotesk via freetype.

## Build

**Desktop**
```bash
pip install -r requirements.txt   # zstandard (exporter), for .anki21b support
python exporter.py path/to/deck.apkg out/
# ...review on the Vita, then:
python importer.py out/deck.vitadeck "%APPDATA%/Anki2/<profile>/collection.anki2"
```
> `importer.py` needs the `anki` package — run it with Anki's bundled venv
> (`%LOCALAPPDATA%/AnkiProgramFiles/.venv/Scripts/python.exe`), and **close Anki first**.

**Vita** (requires vitaSDK)
```bash
cd vita_app2 && cmake -S . -B build && cmake --build build
# -> build/vitadeck_p3.vpk  (install with VitaShell)
```

## Install on the Vita

1. Install `vitadeck_p3.vpk` via VitaShell.
2. Copy your exported `<deck>.vitadeck` folders to `ux0:data/vitadeck/`.
3. (Optional) drop the BGM streams (`menu0/menu1/rev0/rev1.raw`, mono 48k s16) in
   `ux0:data/vitadeck/`.

## Assets not in this repo

To keep the repo clean and avoid redistributing third-party content, the `.gitignore`
excludes: `.apkg` decks and their exports, all audio (the background music and SFX are
**user-supplied**), build artifacts, and crash dumps. The bundled **Space Grotesk** fonts are
included under the SIL Open Font License.

## Repo layout

| Path | What |
|------|------|
| `exporter.py`, `importer.py` | desktop pipeline |
| `FORMAT.md` | the frozen `.vitadeck` v1 spec |
| `vita_app2/` | the Vita app — `main.cpp`, `font.h`, `draw.h`, `audio.h`, `CMakeLists.txt` |
| `vita_app2/assets/fonts/` | Space Grotesk (OFL) |
| `PROJECT_RECAP.md` | full development history / architecture tour |

## Status

Confirmed working on hardware. One known cosmetic limitation: the custom LiveArea splash
background doesn't display on install-over (the bubble icon and name apply correctly). See
[`PROJECT_RECAP.md`](PROJECT_RECAP.md) for the full story.

## License

[MIT](LICENSE) © 2026 Somkene Onwusika. The bundled Space Grotesk fonts are under the SIL
Open Font License (see [`LICENSE`](LICENSE)).

## Credits

- [Space Grotesk](https://github.com/floriankarsten/space-grotesk) — SIL Open Font License.
- Built with [vitaSDK](https://vitasdk.org) and [vita2d](https://github.com/xerpi/libvita2d).
