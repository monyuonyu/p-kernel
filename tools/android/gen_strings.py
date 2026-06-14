#!/usr/bin/env python3
# tools/android/gen_strings.py — generate the UMP entry-screen strings.xml
# for all 32 manifesto languages from ONE table (tools/android/ui_strings.tsv).
#
# Why this exists: the entry screen (MainActivity) must speak the user's
# language from the very first frame, in the SAME language set as the
# manifesto (manifesto_langs.mk : MANIFESTO_LANGS_FULL). Hand-maintaining 32
# values-*/strings.xml files is how language sets silently drift (wave-36
# lesson). So: one greppable source table -> deterministic generator.
#
# Output:
#   android/app/src/main/res/values-<qualifier>/strings.xml   (x32)
# The English/base values/strings.xml is NOT generated here (it is the
# hand-kept fallback that also holds the non-i18n strings: notif channel,
# galaxy splash, etc.). Unlisted device locales fall back to it (= English),
# matching the manifesto's own en fallback.
#
# Locale-qualifier mapping (manifesto tag -> Android res dir suffix):
#   ja, en, es, fr, ...   -> values-<tag>             (ISO-639-1, direct)
#   zh-Hans               -> values-b+zh+Hans         (BCP-47, script form)
#   zh-Hant               -> values-b+zh+Hant         (BCP-47, script form)
#   fil                   -> values-fil               (API 21+; legacy 'tl'
#                                                       handled by a copy)
#   pt                    -> values-pt                (manifesto carries plain
#                                                       'pt', no region split)
# The BCP-47 b+lang+Script form (API 21+) is REQUIRED for zh-Hans/Hant: a
# bare values-zh cannot distinguish simplified from traditional, and our
# minSdk is 26, so b+ is safe.
#
# Usage:  python3 tools/android/gen_strings.py
#         (run from anywhere; paths are resolved relative to this file.)
#
# The short strings are machine-translated (see ui_strings.tsv header); the
# tagline is human-approved manifesto text. This script does NOT translate —
# it only formats. Edit the table, rerun.

import csv
import os
import sys
import html

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TABLE = os.path.join(HERE, "ui_strings.tsv")
RES = os.path.join(ROOT, "android", "app", "src", "main", "res")

# manifesto tag -> Android resource qualifier suffix(es). A list so one tag
# can emit more than one dir (fil also gets the legacy tl alias).
QUALIFIER = {
    "zh-Hans": ["b+zh+Hans"],
    "zh-Hant": ["b+zh+Hant"],
    "fil":     ["fil", "tl"],   # tl = legacy alias for Filipino
}

# Keys that map to <string name=...> in the generated file. Order in the XML
# follows this list; 'tagline' first so it reads top-to-bottom like the screen.
KEYS = [
    ("tagline",     "entry_tagline"),
    ("start",       "btn_start"),
    ("solo_sub",    "entry_solo_sub"),
    ("node_id",     "label_node_id"),
    ("advanced",    "entry_advanced"),
    ("relay_host",  "label_relay_host"),
    ("relay_port",  "label_relay_port"),
    ("relay_key",   "label_relay_key"),
    ("stop",        "btn_stop"),
    ("galaxy",      "btn_galaxy"),
    ("lighting",    "galaxy_lighting"),   # waking splash (no tech jargon)
    ("charge",      "galaxy_charge"),     # the charge-only gate, said kindly
    ("wifi_wait",   "galaxy_wifi_wait"),  # WiFi-only pause: waiting for WiFi (en+ja; else en fallback)
    ("star_lit",    "star_lit"),          # advanced screen: state, human words
    ("star_dark",   "star_dark"),
    ("sleep",       "btn_sleep"),
    ("copy",        "btn_copy"),
    ("battery_safe","pref_battery_safe"),  # advanced screen toggle (en+ja now)
    ("floor_label", "pref_floor_label"),   # battery-floor SeekBar label (%1$d%%; en+ja now)
    ("wifi_only",   "pref_wifi_only"),     # WiFi-only toggle (en+ja now)
    ("start_on_boot","pref_start_on_boot"),# start-on-boot toggle (en+ja now)
    ("reintro",     "reintro_btn"),        # "view introduction again" (advanced)
    ("settings_header", "settings_header"),# Settings layer header (en+ja+most; else en fallback)
    ("app_version",     "app_version_fmt"),# "yurikago %1$s" (product name untranslated)
]


def quals(tag):
    return QUALIFIER.get(tag, [tag])


def esc(s):
    # Android string XML: escape XML metachars, then the apostrophe and quote
    # which are special in Android resources (must be backslash-escaped).
    s = html.escape(s, quote=False)
    s = s.replace("\\", "\\\\")   # literal backslashes (none expected, safe)
    s = s.replace("'", "\\'")
    s = s.replace('"', '\\"')
    return s


def main():
    with open(TABLE, encoding="utf-8") as f:
        rows = [ln for ln in f if not ln.lstrip().startswith("#")]
    reader = csv.DictReader(rows, delimiter="\t")
    cols = reader.fieldnames
    for col, _ in KEYS:
        if col not in cols:
            sys.exit(f"gen_strings: table is missing column '{col}'")

    written = 0
    langs = []
    for row in reader:
        tag = row["lang"].strip()
        if not tag:
            continue
        # en is the base values/strings.xml fallback; do not emit values-en
        # (it would be redundant) — but DO emit ja and every other tag.
        if tag == "en":
            langs.append(tag)
            continue
        langs.append(tag)
        body = []
        for col, name in KEYS:
            val = (row.get(col) or "").strip()
            # Skip empty cells: a missing translation must NOT emit an empty
            # <string>, which would override (blank out) the English base
            # fallback. Leaving the key out lets Android fall back to values/.
            # (This is how a key can be filled for en+ja now and the other 30
            #  langs later, in the i18n wave, without breaking the build.)
            if not val:
                continue
            body.append(f'    <string name="{name}">{esc(val)}</string>')
        xml = ('<?xml version="1.0" encoding="utf-8"?>\n'
               "<!-- GENERATED by tools/android/gen_strings.py from\n"
               "     tools/android/ui_strings.tsv. DO NOT EDIT BY HAND.\n"
               "     tagline = human-approved manifesto line 3; the rest are\n"
               "     machine-translated (native review welcome). -->\n"
               "<resources>\n" + "\n".join(body) + "\n</resources>\n")
        for q in quals(tag):
            outdir = os.path.join(RES, f"values-{q}")
            os.makedirs(outdir, exist_ok=True)
            with open(os.path.join(outdir, "strings.xml"), "w", encoding="utf-8") as out:
                out.write(xml)
            written += 1

    print(f"gen_strings: {len(langs)} languages in table, "
          f"{written} values-*/strings.xml written "
          f"(en is the base fallback, not emitted).")


if __name__ == "__main__":
    main()
