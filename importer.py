#!/usr/bin/env python3
"""
VitaDeck desktop importer — Phase 6: close the loop back into Anki.

Reads `reviews.jsonl` from a `.vitadeck` folder (written by the PS Vita review
client) and replays each grade into a live Anki collection through Anki's own
scheduler, so the next due date / interval are computed by Anki — never written
by hand.

Usage:
    python importer.py <path-to.vitadeck-folder> <path-to-collection.anki2>

IMPORTANT — which Python interpreter:
    This script imports the `anki` package. On this machine the user's default
    Python (3.14) does NOT have `anki` installed, but the Anki desktop app ships
    a managed venv that does. Run with that interpreter:

        & "$env:LOCALAPPDATA/AnkiProgramFiles/.venv/Scripts/python.exe" importer.py ...

    (confirmed: anki 25.09.4, Python 3.13.5 in that venv.)

Safety contract:
    * Anki MUST be closed. Anki holds an exclusive lock on collection.anki2;
      writing while it is open corrupts the collection. We refuse to proceed if
      an `anki.exe` process is found.
    * Grades are applied ONLY via the scheduler. We never write `due`, `ivl`,
      or `factor` directly.

----------------------------------------------------------------------------
API NOTES — confirmed against the INSTALLED anki 25.09.4 by reading
`anki/scheduler/v3.py` source, NOT inferred from training data:

    * `col.sched` is the v3 scheduler (anki.scheduler.v3.Scheduler).
    * `col.sched.answer_card(input)` takes a `CardAnswer` PROTOBUF — it is NOT
      `answer_card(card, ease)`. Calling it with (card, ease) raises TypeError.
    * `col.sched.answerCard(card, ease)`  <-- THE CORRECT (card, ease) ENTRY.
      It is the legacy compat shim that internally does:
          states = backend.get_scheduling_states(card.id)
          answer = build_answer(card, states, rating)   # rating from ease 1-4
          answer_card(answer); card.load()
      It works for New cards (reps==0), and the backend writes a revlog row,
      so Anki's "card info" history table gains a new row after import.
    * ease -> rating: 1=Again, 2=Hard, 3=Good, 4=Easy (matches FORMAT.md grades).

    The task brief said to call `col.sched.answer_card(card, ease)`; that name/
    signature is wrong for 25.09.4. We use `answerCard(card, ease)`, which is the
    real method with that signature. This is a CONFIRMED call, not a guess.

    Replay semantics: answerCard stamps the review as happening *now* and lets
    Anki compute the next state from the card's current state. The Vita's
    original `reviewed_at` and `response_ms` are preserved verbatim in
    import_log.json (see below) so no information is lost, but Anki's revlog row
    is dated at import time — which matches the "new row dated today" check in
    the verification steps.
----------------------------------------------------------------------------
"""

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone


GRADE_LABELS = {1: "Again", 2: "Hard", 3: "Good", 4: "Easy"}


def log(msg):
    print(msg, flush=True)


# --------------------------------------------------------------------------
# Step 1 — safety checks
# --------------------------------------------------------------------------

def is_anki_running():
    """Return True if an Anki desktop process appears to be running.

    Windows-first (this project's platform) via `tasklist`; best-effort `ps`
    fallback elsewhere. Stdlib only — no psutil.
    """
    try:
        if sys.platform.startswith("win"):
            # tasklist lists image names; anki.exe is the GUI process.
            out = subprocess.run(
                ["tasklist", "/FO", "CSV", "/NH"],
                capture_output=True, text=True, timeout=15,
            ).stdout.lower()
            return "anki.exe" in out
        else:
            out = subprocess.run(
                ["ps", "-A"], capture_output=True, text=True, timeout=15
            ).stdout.lower()
            # match the GUI binary, not this importer
            return any(line.endswith("anki") or "/anki" in line
                       for line in out.splitlines())
    except (OSError, subprocess.SubprocessError):
        # If we cannot determine, be conservative and say "unknown" -> not found,
        # but the collection open will still fail loudly if it is locked.
        return False


def safety_checks(vitadeck_dir, collection_path):
    """Run all pre-flight checks. Returns the path to reviews.jsonl, or exits."""
    # Anki must be closed.
    if is_anki_running():
        log("ERROR: Anki appears to be running (found an anki.exe process).")
        log("       Anki locks collection.anki2 while open; writing to it now")
        log("       would corrupt your collection. Close Anki and re-run.")
        sys.exit(2)

    # vitadeck folder
    if not os.path.isdir(vitadeck_dir):
        log(f"ERROR: not a directory: {vitadeck_dir}")
        sys.exit(2)

    # reviews.jsonl present and non-empty
    reviews_path = os.path.join(vitadeck_dir, "reviews.jsonl")
    if not os.path.isfile(reviews_path):
        log(f"Nothing to import: no reviews.jsonl in {vitadeck_dir}")
        log("(The Vita creates reviews.jsonl on the first grade. None recorded.)")
        sys.exit(0)
    if os.path.getsize(reviews_path) == 0:
        log(f"Nothing to import: reviews.jsonl is empty ({reviews_path}).")
        sys.exit(0)

    # collection present and readable
    if not os.path.isfile(collection_path):
        log(f"ERROR: collection not found: {collection_path}")
        sys.exit(2)
    if not os.access(collection_path, os.R_OK):
        log(f"ERROR: collection not readable: {collection_path}")
        sys.exit(2)

    return reviews_path


# --------------------------------------------------------------------------
# Step 2 — parse + validate + dedupe reviews.jsonl (streaming)
# --------------------------------------------------------------------------

def _is_pos_int(v):
    return isinstance(v, int) and not isinstance(v, bool) and v > 0


def _is_nonneg_int(v):
    return isinstance(v, int) and not isinstance(v, bool) and v >= 0


def parse_reviews(reviews_path):
    """Stream reviews.jsonl. Returns (rows, stats).

    rows: list of validated, de-duplicated dicts in file order.
    stats: dict with counts and the list of skip reasons (for the import log).
    """
    rows = []
    seen = set()                  # (card_id, reviewed_at) dedupe keys
    skipped_validation = []       # list of {line_no, raw, reason}
    skipped_duplicate = []        # list of {line_no, card_id, reviewed_at}
    total_lines = 0

    with open(reviews_path, "r", encoding="utf-8") as fh:
        for line_no, raw in enumerate(fh, start=1):
            stripped = raw.strip()
            if not stripped:
                continue  # tolerate blank lines silently
            total_lines += 1

            try:
                obj = json.loads(stripped)
            except json.JSONDecodeError as e:
                reason = f"malformed JSON: {e}"
                log(f"  WARN line {line_no}: {reason} -- skipping")
                skipped_validation.append(
                    {"line_no": line_no, "raw": stripped[:200], "reason": reason})
                continue

            if not isinstance(obj, dict):
                reason = "not a JSON object"
                log(f"  WARN line {line_no}: {reason} -- skipping")
                skipped_validation.append(
                    {"line_no": line_no, "raw": stripped[:200], "reason": reason})
                continue

            card_id = obj.get("card_id")
            grade = obj.get("grade")
            reviewed_at = obj.get("reviewed_at")
            response_ms = obj.get("response_ms")

            problems = []
            if not _is_pos_int(card_id):
                problems.append("card_id must be a positive integer")
            if not (isinstance(grade, int) and not isinstance(grade, bool)
                    and 1 <= grade <= 4):
                problems.append("grade must be an integer 1-4")
            if not _is_pos_int(reviewed_at):
                problems.append("reviewed_at must be a positive integer")
            if not _is_nonneg_int(response_ms):
                problems.append("response_ms must be a non-negative integer")

            if problems:
                reason = "; ".join(problems)
                log(f"  WARN line {line_no}: {reason} -- skipping")
                skipped_validation.append(
                    {"line_no": line_no, "raw": stripped[:200], "reason": reason})
                continue

            key = (card_id, reviewed_at)
            if key in seen:
                log(f"  WARN line {line_no}: duplicate (card_id={card_id}, "
                    f"reviewed_at={reviewed_at}) -- skipping")
                skipped_duplicate.append(
                    {"line_no": line_no, "card_id": card_id,
                     "reviewed_at": reviewed_at})
                continue
            seen.add(key)

            rows.append({
                "line_no": line_no,
                "card_id": card_id,
                "grade": grade,
                "reviewed_at": reviewed_at,
                "response_ms": response_ms,
            })

    stats = {
        "total_lines": total_lines,
        "skipped_validation": skipped_validation,
        "skipped_duplicate": skipped_duplicate,
    }
    return rows, stats


# --------------------------------------------------------------------------
# Step 3/4 — open collection, apply grades via the scheduler, commit
# --------------------------------------------------------------------------

def apply_grades(collection_path, rows):
    """Open the collection, replay grades, save & close.

    Returns a dict of results for the import log. Never raises on a per-card
    miss; only a failure to open the collection is fatal.
    """
    # Imported here (not at top) so that Step 1/2 can run and report even if the
    # anki package is missing in this interpreter.
    try:
        # CONFIRMED correct import for v25.09.4 (anki.storage.Collection is the
        # deprecated path; we deliberately do not use it).
        from anki.collection import Collection
    except Exception as e:  # pragma: no cover - environment specific
        log("")
        log("ERROR: could not import the `anki` package in THIS interpreter.")
        log(f"       {type(e).__name__}: {e}")
        log("       Run pip show anki with this interpreter to confirm. On this")
        log("       machine the working interpreter is Anki's managed venv:")
        log(r'         %LOCALAPPDATA%\AnkiProgramFiles\.venv\Scripts\python.exe')
        sys.exit(3)

    try:
        from anki.errors import NotFoundError
    except Exception:
        NotFoundError = Exception  # fallback; we catch broadly below anyway

    log("")
    log(f"Opening collection: {collection_path}")
    try:
        col = Collection(collection_path)
    except Exception as e:
        log("ERROR: failed to open the collection.")
        log(f"       {type(e).__name__}: {e}")
        log("       If this is a lock error, make sure Anki is fully closed.")
        sys.exit(3)

    # Sanity: confirm we are on the SM-2 / non-FSRS scheduler the project targets.
    # (Informational only; we do not change behaviour on it.)
    try:
        fsrs = col.get_config("fsrs")
        log(f"  scheduler check: get_config('fsrs') = {fsrs!r} "
            f"(None/False => SM-2, as expected)")
    except Exception:
        pass

    applied = []          # per-card success records
    not_found = []        # card_id not in collection

    try:
        for row in rows:
            card_id = row["card_id"]
            grade = row["grade"]
            label = GRADE_LABELS[grade]

            # Look up the card; missing id -> log and skip, never crash.
            try:
                card = col.get_card(card_id)
            except NotFoundError:
                card = None
            except Exception as e:
                # Any other lookup failure: treat as not found but record why.
                log(f"  SKIP card {card_id}: lookup failed ({type(e).__name__}: {e})")
                not_found.append({"card_id": card_id, "reviewed_at": row["reviewed_at"],
                                  "reason": f"lookup error: {e}"})
                continue
            if card is None:
                log(f"  SKIP card {card_id}: not in this collection")
                not_found.append({"card_id": card_id, "reviewed_at": row["reviewed_at"],
                                  "reason": "card_id not found"})
                continue

            # Apply the grade THROUGH THE SCHEDULER (confirmed method, see header).
            # 1-4 maps directly to Again/Hard/Good/Easy.
            #
            # answerCard -> build_answer reads card.time_taken(), which requires a
            # started timer. The GUI calls start_timer() when it fetches a card via
            # getCard(); we fetched via get_card(), so we must start it ourselves or
            # time_taken() hits `time.time() - None` -> TypeError. (Confirmed by
            # reading anki/cards.py time_taken in 25.09.4.) The resulting recorded
            # answer time is ~0ms; the Vita's real response_ms is preserved in
            # import_log.json instead.
            card.start_timer()
            col.sched.answerCard(card, grade)

            # Re-read to report the freshly computed schedule. answerCard already
            # calls card.load(), but we re-fetch defensively.
            card = col.get_card(card_id)
            new_ivl = card.ivl                 # interval in days (>0 after review)
            # Anki stores `due` as a day-number (days since collection creation)
            # for review cards, or a position for new/learning. Translate review
            # cards to a calendar date for a human-readable log.
            due_human = _due_to_human(col, card)

            log(f"  OK   card {card_id}: graded {label} "
                f"-> due {due_human}, interval {new_ivl}d")
            applied.append({
                "card_id": card_id,
                "grade": grade,
                "grade_label": label,
                # original Vita data preserved here even though Anki stamps "now":
                "vita_reviewed_at": row["reviewed_at"],
                "vita_response_ms": row["response_ms"],
                "new_due": due_human,
                "new_interval_days": new_ivl,
                "new_reps": card.reps,
            })

        log("")
        log("Saving collection...")
        col.save()
    finally:
        col.close()
        log("Collection closed.")

    return {"applied": applied, "not_found": not_found}


def _due_to_human(col, card):
    """Best-effort human-readable due value.

    Review cards: `due` is days since collection epoch -> calendar date.
    New/learning cards: `due` is a queue position or epoch-seconds; report raw.
    """
    try:
        # queue 2 = review, type 2 = review; days since crt.
        if card.type == 2 or card.queue == 2:
            crt = col.crt  # collection creation time, epoch seconds
            due_epoch = crt + card.due * 86400
            return datetime.fromtimestamp(due_epoch, tz=timezone.utc).strftime(
                "%Y-%m-%d") + f" (day {card.due})"
        # learning cards: due is epoch seconds
        if card.queue in (1, 3):
            return datetime.fromtimestamp(card.due, tz=timezone.utc).strftime(
                "%Y-%m-%d %H:%M") + " (learning)"
    except Exception:
        pass
    return f"raw due={card.due}"


# --------------------------------------------------------------------------
# Step 4 — import log
# --------------------------------------------------------------------------

def write_import_log(vitadeck_dir, reviews_path, rows, parse_stats, apply_result):
    record = {
        "import_run_utc": datetime.now(timezone.utc).isoformat(),
        "importer_version": "0.1.0",
        "vitadeck": os.path.abspath(vitadeck_dir),
        "reviews_file": os.path.abspath(reviews_path),
        "summary": {
            "rows_in_file": parse_stats["total_lines"],
            "rows_valid_unique": len(rows),
            "rows_applied": len(apply_result["applied"]),
            "rows_skipped_not_found": len(apply_result["not_found"]),
            "rows_skipped_validation": len(parse_stats["skipped_validation"]),
            "rows_skipped_duplicate": len(parse_stats["skipped_duplicate"]),
        },
        "applied": apply_result["applied"],
        "skipped_not_found": apply_result["not_found"],
        "skipped_validation": parse_stats["skipped_validation"],
        "skipped_duplicate": parse_stats["skipped_duplicate"],
    }
    out_path = os.path.join(vitadeck_dir, "import_log.json")
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=2, ensure_ascii=False)
    return out_path, record


# --------------------------------------------------------------------------
# Step 5 — verification instructions (printed, not automated)
# --------------------------------------------------------------------------

def print_verification(apply_result):
    log("")
    log("=" * 70)
    log("VERIFY IN ANKI (manual - this script cannot drive the GUI):")
    log("  1. Open Anki.")
    if apply_result["applied"]:
        sample = apply_result["applied"][0]["card_id"]
    else:
        sample = "<card_id>"
    log(f"  2. Open the card Browser and find card id {sample}.")
    log("  3. Press Ctrl+Shift+I (Card Info) and confirm:")
    log("       - a NEW row in the Review History table, dated today;")
    log('       - the Due date has changed (no longer "New #1");')
    log("       - Interval is no longer 0.")
    log("=" * 70)


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Import Vita review grades (reviews.jsonl) into Anki.")
    ap.add_argument("vitadeck", help="path to the .vitadeck folder")
    ap.add_argument("collection", help="path to collection.anki2")
    args = ap.parse_args()

    vitadeck_dir = args.vitadeck
    collection_path = args.collection

    log("VitaDeck importer 0.1.0")
    log(f"  vitadeck:   {os.path.abspath(vitadeck_dir)}")
    log(f"  collection: {os.path.abspath(collection_path)}")
    log("")

    # Step 1
    log("Step 1: safety checks...")
    reviews_path = safety_checks(vitadeck_dir, collection_path)
    log(f"  OK: Anki not running; reviews.jsonl present; collection readable.")

    # Step 2
    log("")
    log("Step 2: parsing reviews.jsonl...")
    rows, parse_stats = parse_reviews(reviews_path)
    log(f"  rows in file:        {parse_stats['total_lines']}")
    log(f"  skipped (bad data):  {len(parse_stats['skipped_validation'])}")
    log(f"  skipped (duplicate): {len(parse_stats['skipped_duplicate'])}")
    log(f"  valid to import:     {len(rows)}")

    if not rows:
        log("")
        log("No valid rows to import. Nothing applied.")
        # still record the run
        empty = {"applied": [], "not_found": []}
        out_path, _ = write_import_log(vitadeck_dir, reviews_path, rows,
                                       parse_stats, empty)
        log(f"Wrote import log: {out_path}")
        sys.exit(0)

    # Step 3/4
    log("")
    log("Step 3: applying grades through the scheduler...")
    apply_result = apply_grades(collection_path, rows)

    # Step 4 — report + log
    s = {
        "found": len(apply_result["applied"]) + len(apply_result["not_found"]),
    }
    log("")
    log("Step 4: summary")
    log(f"  grades applied:          {len(apply_result['applied'])}")
    log(f"  skipped (not found):     {len(apply_result['not_found'])}")
    log(f"  skipped (validation):    {len(parse_stats['skipped_validation'])}")
    log(f"  skipped (duplicate):     {len(parse_stats['skipped_duplicate'])}")
    out_path, _ = write_import_log(vitadeck_dir, reviews_path, rows,
                                   parse_stats, apply_result)
    log(f"  import log written:      {out_path}")

    # Step 5
    print_verification(apply_result)


if __name__ == "__main__":
    main()
