"""
Build a SYNTHETIC cloze .apkg by mutating a copy of an existing test deck.

We do this because none of the three real .apkg files (Linux+, EEI,
Rubber_Gloving) contain a cloze model (type:1) — all three are Basic only.
The synthetic deck lets us exercise the cloze code path end-to-end.

Strategy:
  1. Copy Rubber_Gloving.apkg contents into a temp dir.
  2. Open collection.anki21, inject a cloze note type and a cloze note
     into the existing col.models / notes / cards tables.
  3. Re-zip into synthetic_cloze.apkg.

The injected note has 2 clozes -> generates 2 cards (ord 0 and 1), plus
a hint on c2 and a tag, so the tag pipeline gets exercised too.
"""

from __future__ import annotations
import json, shutil, sqlite3, sys, tempfile, time, zipfile, os
from pathlib import Path

PROJECT = Path(__file__).parent
SOURCE_APKG = PROJECT / "Rubber_Gloving.apkg"
OUT_APKG = PROJECT / "synthetic_cloze.apkg"

CLOZE_MID = 9999000001  # made-up model id, must not clash
CLOZE_NID = 9999000002
CLOZE_CARD_ID_BASE = 9999000100
SYNTH_DECK_ID = 9999000003  # new deck so we don't pollute existing ones

CLOZE_MODEL_TMPL_QFMT = "{{cloze:Text}}<br><br>{{Extra}}"
CLOZE_MODEL_TMPL_AFMT = "{{cloze:Text}}<br><br>{{Extra}}"

CLOZE_FIELD_TEXT = (
    "The mitochondrion is the {{c1::powerhouse}} of the cell, "
    "and the nucleus stores {{c2::DNA::genetic material}}."
)
EXTRA_FIELD = "Source: synthetic test"
TAGS = "biology::cell synthetic"

def main():
    if not SOURCE_APKG.exists():
        print(f"ERROR: {SOURCE_APKG} not found")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        with zipfile.ZipFile(SOURCE_APKG) as zf:
            zf.extractall(tmp)

        coll = tmp / "collection.anki21"
        c = sqlite3.connect(coll)
        row = c.execute("SELECT models, decks FROM col").fetchone()
        models = json.loads(row[0])
        decks = json.loads(row[1])

        # ---- inject cloze model ----
        now_s = int(time.time())
        models[str(CLOZE_MID)] = {
            "id": CLOZE_MID,
            "name": "SyntheticCloze",
            "type": 1,
            "mod": now_s,
            "usn": -1,
            "sortf": 0,
            "did": SYNTH_DECK_ID,
            "tmpls": [{
                "name": "Cloze",
                "ord": 0,
                "qfmt": CLOZE_MODEL_TMPL_QFMT,
                "afmt": CLOZE_MODEL_TMPL_AFMT,
                "bqfmt": "", "bafmt": "",
                "did": None, "bfont": "", "bsize": 0,
            }],
            "flds": [
                {"name": "Text", "ord": 0, "sticky": False, "rtl": False,
                 "font": "Arial", "size": 20, "media": []},
                {"name": "Extra", "ord": 1, "sticky": False, "rtl": False,
                 "font": "Arial", "size": 20, "media": []},
            ],
            "css": ".card { font-family: arial; font-size: 20px; }\n.cloze { font-weight: bold; color: blue; }",
            "latexPre": "", "latexPost": "",
            "latexsvg": False,
            "req": [[0, "any", [0]]],
        }

        # ---- inject deck ----
        decks[str(SYNTH_DECK_ID)] = {
            "id": SYNTH_DECK_ID,
            "name": "SyntheticCloze",
            "mod": now_s, "usn": -1,
            "lrnToday": [0, 0], "revToday": [0, 0],
            "newToday": [0, 0], "timeToday": [0, 0],
            "collapsed": False, "browserCollapsed": False,
            "desc": "synthetic deck for cloze verification",
            "dyn": 0, "conf": 1, "extendNew": 0, "extendRev": 0,
        }

        c.execute(
            "UPDATE col SET models=?, decks=?",
            (json.dumps(models), json.dumps(decks)),
        )

        # ---- inject note ----
        flds = CLOZE_FIELD_TEXT + "\x1f" + EXTRA_FIELD
        sfld = CLOZE_FIELD_TEXT
        c.execute(
            "INSERT INTO notes (id,guid,mid,mod,usn,tags,flds,sfld,csum,flags,data) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?)",
            (CLOZE_NID, "synthcloze1", CLOZE_MID, now_s, -1,
             " " + TAGS + " ", flds, sfld, 0, 0, ""),
        )

        # ---- inject cards (one per cloze index) ----
        for ord_ in (0, 1):
            c.execute(
                "INSERT INTO cards "
                "(id,nid,did,ord,mod,usn,type,queue,due,ivl,factor,reps,lapses,left,odue,odid,flags,data) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (CLOZE_CARD_ID_BASE + ord_, CLOZE_NID, SYNTH_DECK_ID, ord_,
                 now_s, -1, 0, 0, ord_, 0, 0, 0, 0, 0, 0, 0, 0, ""),
            )

        c.commit()
        c.close()

        # ---- re-zip ----
        if OUT_APKG.exists():
            OUT_APKG.unlink()
        with zipfile.ZipFile(OUT_APKG, "w", zipfile.ZIP_DEFLATED) as zf:
            for p in sorted(tmp.iterdir()):
                zf.write(p, p.name)

        print(f"wrote {OUT_APKG}  ({OUT_APKG.stat().st_size} bytes)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
