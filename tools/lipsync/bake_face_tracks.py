#!/usr/bin/env python3
"""Bake VO lines into `.nface` viseme tracks and measure them against F1-F4 (docs/rigging_plan.md §3, §6).

Pipeline (plan §6.2): phones (from an aligner) -> 15 visemes (face_spec.PHONE_TO_VISEME) -> dominance-blend
coarticulation solver -> ~20 mouth/jaw/tongue channels at 60 Hz -> u8 `.nface` (format in
engine/src/anim/FacePlayer.cpp) + an events/direction sidecar JSON.

Alignment backends (pluggable, all optional, all run offline):
  espeak     espeak-ng's own phoneme events for the audio it synthesised (an oracle alignment: it measures
             the solver alone, not an aligner)
  mfa        Montreal Forced Aligner TextGrids (`align-mfa` runs it; `--mfa-cmd` is the command prefix)
  rhubarb    Rhubarb Lip Sync mouth cues (A-H, X) mapped to visemes (face_spec.RHUBARB_TO_VISEME)
  elevenlabs ElevenLabs `with-timestamps` character alignment JSON, if present; G2P per word via espeak
  textgrid   hand-marked Praat TextGrids (the plan's gold-set reference, once someone marks them)

No VO audio exists yet, so `synth` makes the gold set's audio with espeak-ng (TTS). The phone timings espeak
reports for its own audio are the *reference* for `eval`: a machine reference, not the hand-marked TextGrids the
plan's F1/F2/F4 gate asks for. `eval` says which reference it used in every report.

Only the Python standard library is needed, plus `espeakng_loader` (pip) for the espeak backend.

  python3 tools/lipsync/bake_face_tracks.py gold  --csv design/.../draft56_04a.csv --out tools/lipsync/gold_set.json
  python3 tools/lipsync/bake_face_tracks.py synth --gold tools/lipsync/gold_set.json --work W
  python3 tools/lipsync/bake_face_tracks.py align-mfa --work W --mfa-cmd "micromamba run -p ENV mfa" \
          --dict english_us_arpa.dict --model english_us_arpa.zip
  python3 tools/lipsync/bake_face_tracks.py align-rhubarb --work W --rhubarb /path/to/rhubarb
  python3 tools/lipsync/bake_face_tracks.py bake --work W --backend espeak --out W/face/espeak
  python3 tools/lipsync/bake_face_tracks.py eval --work W --backends espeak mfa rhubarb elevenlabs --report W/report.json
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import json
import math
import os
import re
import shlex
import statistics
import struct
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender"))
import face_spec as fs  # noqa: E402  (pure Python, no bpy)

FPS = 60
DT = 1.0 / FPS
TONGUE_RANGE_DEG = 40.0  # engine kTongueTrackRangeDeg: u8 0..255 -> -40..+40 deg
NFACE_MAGIC = b"NFACE\0"
NFACE_VERSION = 1

# Channels the solver writes: every shape any viseme uses, plus the tongue bones and the speech weight.
SHAPE_CHANNELS = sorted({s for v in fs.VISEMES.values() for s in v["shapes"]})
TONGUE_CHANNELS = ["@tongue_01", "@tongue_02", "@tongue_03"]
CHANNELS = SHAPE_CHANNELS + TONGUE_CHANNELS + ["@speech"]
CLOSURE_CHANNELS = {"mouthClose", "mouthRollLower", "mouthPressLeft", "mouthPressRight", "lipsTogetherSoft"}
VOWEL_VISEMES = {"aa", "E", "ih", "oh", "ou", "RR"}

# ---- solver parameters (plan §6.2 step 3: attack/release 40-80 ms, closures hard, rounding look-ahead 150 ms) ----
DOMINANCE = {}
DOMINANCE_VOWEL = 1.0
TAU = {"PP": 0.025, "FF": 0.025}
TAU_DEFAULT = 0.025
ROUND_LOOKAHEAD = 0.150
LEAD = 0.0
ROUND_GAIN = 0.5
FLOOR_MARGIN = 0.12
FLOOR_TAU = 0.038
FLOOR_INSET = 0.012
SLEW = 0.30           # |dw| per frame the limiter allows (F3 limit 0.35)
SLEW_CLOSURE = 0.50   # closures (F3 limit 0.6)
VOLUME_JAW = {"whisper": 0.5, "soft": 0.8, "conversational": 1.0, "raised": 1.15, "shout": 1.3}
VOLUME_STRETCH = {"raised": 0.1, "shout": 0.2}

# ---- F-metric limits (plan §3) ----
F1_MEDIAN = 0.010
F1_P95 = 1.0 / 60.0
F1_LATE = 1.0 / 60.0
F1_LEAD = 2.0 / 60.0
F2_FRAMES = 0.90
F3_DW = 0.35
F3_DW_CLOSURE = 0.60
F3_D2W = 0.25
F3_HOLD = 2
F4_ROUND_LEAD = 0.100
F4_SHORT = 0.050

# ------------------------------------------------------------------------------------------------------------
# espeak-ng (ctypes)
# ------------------------------------------------------------------------------------------------------------
# espeak mnemonic -> ARPAbet (one or more phones; multi-phone mnemonics split the duration).
ESPEAK_TO_ARPA = {
    "a": ["AE"], "a#": ["AH"], "A:": ["AA"], "A@": ["AA", "R"], "aa": ["AE"], "aI": ["AY"], "aI@": ["AY", "AH"],
    "aI3": ["AY", "ER"], "aU": ["AW"], "aU@": ["AW", "AH"], "@": ["AH"], "@2": ["AH"], "@5": ["AH"],
    "@L": ["AH", "L"], "@-": ["AH"], "3": ["ER"], "3:": ["ER"], "e": ["EY"], "e@": ["EH", "R"], "eI": ["EY"],
    "E": ["EH"], "E2": ["EH"], "E3": ["EH"], "i": ["IY"], "i:": ["IY"], "i@": ["IY", "AH"], "i@3": ["IY", "R"],
    "I": ["IH"], "I#": ["IH"], "I2": ["IH"], "0": ["AA"], "0@": ["AO", "R"], "O": ["AO"], "O:": ["AO"],
    "O@": ["AO", "R"], "o@": ["AO", "R"], "OI": ["OY"], "oU": ["OW"], "oU#": ["OW"], "U": ["UH"],
    "U@": ["UH", "R"], "u:": ["UW"], "V": ["AH"], "r-": ["R"], "x": ["HH"],
    "p": ["P"], "b": ["B"], "t": ["T"], "t#": ["T"], "d": ["D"], "d#": ["D"], "?": ["T"], "k": ["K"],
    "g": ["G"], "m": ["M"], "n": ["N"], "n-": ["N"], "N": ["NG"], "f": ["F"], "v": ["V"], "T": ["TH"],
    "D": ["DH"], "s": ["S"], "z": ["Z"], "S": ["SH"], "Z": ["ZH"], "tS": ["CH"], "dZ": ["JH"], "h": ["HH"],
    "l": ["L"], "l/": ["L"], "r": ["R"], "j": ["Y"], ";": ["Y"], "w": ["W"],
}


class _EspeakId(ctypes.Union):
    _fields_ = [("number", ctypes.c_int), ("name", ctypes.c_char_p), ("string", ctypes.c_char * 8)]


class _EspeakEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("unique_identifier", ctypes.c_uint), ("text_position", ctypes.c_int),
                ("length", ctypes.c_int), ("audio_position", ctypes.c_int), ("sample", ctypes.c_int),
                ("user_data", ctypes.c_void_p), ("id", _EspeakId)]


_SYNTH_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_short), ctypes.c_int,
                             ctypes.POINTER(_EspeakEvent))
EV_WORD, EV_PHONEME = 1, 7


class Espeak:
    """espeak-ng in synchronous mode with phoneme events (espeakINITIALIZE_PHONEME_EVENTS)."""

    def __init__(self):
        import espeakng_loader  # untrusted third-party package: installed in a scratch venv, never committed
        self.lib = ctypes.CDLL(espeakng_loader.get_library_path())
        self.rate = self.lib.espeak_Initialize(2, 0, espeakng_loader.get_data_path().encode(), 1)
        if self.rate <= 0:
            raise RuntimeError("espeak_Initialize failed")
        self._cb = _SYNTH_CB(self._on_synth)
        self.lib.espeak_SetSynthCallback(self._cb)
        self._pcm = []
        self._events = []

    def _on_synth(self, wav, n, ev):
        if n > 0:
            self._pcm.append(ctypes.string_at(wav, n * 2))
        i = 0
        while ev[i].type != 0:
            e = ev[i]
            if e.type == EV_PHONEME:
                self._events.append(("ph", e.sample, bytes(e.id.string).split(b"\0")[0].decode("ascii", "replace")))
            elif e.type == EV_WORD:
                self._events.append(("word", e.sample, e.text_position, e.length))
            i += 1
        return 0

    def synth(self, text, voice="en-us", wpm=150, pitch=50):
        """-> (pcm bytes int16 mono, sample rate, phones [(ARPA, start_s, end_s)], words [(text, start_s, end_s)])"""
        self._pcm, self._events = [], []
        self.lib.espeak_SetVoiceByName(voice.encode())
        self.lib.espeak_SetParameter(1, max(80, min(450, int(wpm))), 0)   # espeakRATE
        self.lib.espeak_SetParameter(3, int(pitch), 0)                     # espeakPITCH
        raw = text.encode("utf-8")
        buf = ctypes.create_string_buffer(raw)
        self.lib.espeak_Synth(buf, len(raw) + 1, 0, 1, 0, 1, None, None)  # POS_CHARACTER, espeakCHARS_UTF8
        self.lib.espeak_Synchronize()
        pcm = b"".join(self._pcm)
        total = len(pcm) / 2 / self.rate
        phones, unknown = [], []
        ph_events = [e for e in self._events if e[0] == "ph"]
        if ph_events and ph_events[0][1] > 0:
            phones.append(("SIL", 0.0, ph_events[0][1] / self.rate))  # leading audio before the first event
        for k, (_, sample, mnem) in enumerate(ph_events):
            s = sample / self.rate
            e = ph_events[k + 1][1] / self.rate if k + 1 < len(ph_events) else total
            if e <= s:
                continue
            m = mnem.lstrip("'',=")
            if m.startswith("_") or m == "":
                arpa = ["SIL"]
            else:
                arpa = ESPEAK_TO_ARPA.get(m)
                if arpa is None:
                    unknown.append(mnem)
                    arpa = ["AH"]
            share = [0.6, 0.4] if len(arpa) == 2 else [1.0 / len(arpa)] * len(arpa)
            t = s
            for a, w in zip(arpa, share):
                t1 = t + (e - s) * w
                phones.append((a, t, t1))
                t = t1
        words = []
        w_events = [e for e in self._events if e[0] == "word"]
        for k, (_, sample, pos, length) in enumerate(w_events):
            s = sample / self.rate
            word = raw[pos - 1:pos - 1 + length].decode("utf-8", "replace")
            # a word ends where its last non-silence phone ends
            nxt = w_events[k + 1][1] / self.rate if k + 1 < len(w_events) else total
            e = max([p[2] for p in phones if p[1] >= s - 1e-6 and p[2] <= nxt + 1e-6 and p[0] != "SIL"] or [nxt])
            words.append((word, s, e))
        return pcm, self.rate, merge_sil(phones), words, unknown


def merge_sil(phones):
    out = []
    for p in phones:
        if out and p[0] == "SIL" and out[-1][0] == "SIL":
            out[-1] = ("SIL", out[-1][1], p[2])
        else:
            out.append(p)
    return out


def write_wav(path, pcm, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def wav_duration(path):
    with wave.open(path, "rb") as w:
        return w.getnframes() / w.getframerate()


# ------------------------------------------------------------------------------------------------------------
# TextGrid (Praat long + short text format) and the gold set
# ------------------------------------------------------------------------------------------------------------
def parse_textgrid(path):
    """-> {tier name: [(label, xmin, xmax)]} for interval tiers."""
    text = open(path, encoding="utf-8", errors="replace").read()
    text = re.sub(r"\[\s*\d+\s*\]", "", text)  # long format: drop "item [1]" / "intervals [3]" indices
    tokens = re.findall(r'"(?:[^"]|"")*"|[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?', text)
    vals = [t[1:-1].replace('""', '"') if t.startswith('"') else float(t) for t in tokens]
    # vals: "ooTextFile" "TextGrid" xmin xmax <exists> ntiers { class name xmin xmax n { xmin xmax text }* }*
    i = 0
    while i < len(vals) and vals[i] != "IntervalTier" and vals[i] != "TextTier":
        i += 1
    tiers = {}
    while i < len(vals):
        cls, name = vals[i], vals[i + 1]
        n = int(vals[i + 4])
        i += 5
        items = []
        for _ in range(n):
            if cls == "IntervalTier":
                items.append((str(vals[i + 2]), float(vals[i]), float(vals[i + 1])))
                i += 3
            else:
                i += 2
        if cls == "IntervalTier":
            tiers[name] = items
        while i < len(vals) and vals[i] not in ("IntervalTier", "TextTier"):
            i += 1
    return tiers


def textgrid_phones(path):
    tiers = parse_textgrid(path)
    ph = next((v for k, v in tiers.items() if k.lower().endswith("phones")), None)
    wd = next((v for k, v in tiers.items() if k.lower().endswith("words")), [])
    if ph is None:
        raise ValueError(path + ": no phones tier")
    phones = [(norm_phone(lab), s, e) for lab, s, e in ph]
    words = [(lab, s, e) for lab, s, e in wd if lab.strip()]
    return merge_sil(phones), words


def norm_phone(label):
    p = re.sub(r"\d", "", label.strip().upper())
    return "SIL" if p in ("", "SP", "SIL", "SPN", "<EPS>") else p


def clean_text(t):
    t = re.sub(r"\[[^\]]*\]", " ", t)          # nonverbal tags
    t = t.replace("…", ", ").replace("—", ", ").replace("–", ", ")
    t = t.replace("’", "'").replace("‘", "'").replace("“", "").replace("”", "")
    return re.sub(r"\s+", " ", t).strip()


def lab_text(t):
    t = clean_text(t).lower()
    t = re.sub(r"[^a-z' ]+", " ", t)
    return re.sub(r"\s+", " ", t).strip()


def line_key(row):
    if row.get("vo_id"):
        return row["vo_id"]
    spk = re.sub(r"[^a-z0-9]+", "_", row["speaker"].lower()).strip("_")
    return f"{row['text_hash']}_{spk}"


def phone_features(text):
    """Crude orthographic coverage flags for picking the gold set."""
    t = " " + lab_text(text) + " "
    return {
        "bilabial": bool(re.search(r"[pbm]", t)),
        "labiodental": bool(re.search(r"[fv]|ph", t)),
        "rounded": bool(re.search(r"oo|ou|ow|\bwh?|w[aeio]|o[^aeiou ]?e\b|\bno\b|\bgo\b|\bso\b", t)),
        "th": "th" in t,
        "ch": bool(re.search(r"ch|sh|j", t)),
    }


GOLD_REQUIRED = ["SC-P0-01#001", "SC-P0-01#002"]


def cmd_gold(a):
    rows = list(csv.DictReader(open(a.csv, encoding="utf-8")))
    by_id = {r["line_id"]: r for r in rows}
    pick = [by_id[i] for i in GOLD_REQUIRED]
    pool = [r for r in rows if r["kind"] == "line" and r["line_id"] not in GOLD_REQUIRED
            and 3 <= len(lab_text(r["spoken_text"]).split()) <= 14]
    # greedy: cover phone classes, volumes and speakers; deterministic (rows are in script order)
    need = {"bilabial": 8, "labiodental": 6, "rounded": 8, "th": 5, "ch": 4}
    speakers, volumes = set(), set()
    for r in pick:
        speakers.add(r["speaker"])
        volumes.add(r["volume"])
    while len(pick) < a.count:
        best, best_score = None, -1.0
        for r in pool:
            if r in pick:
                continue
            f = phone_features(r["spoken_text"])
            have = {k: sum(phone_features(p["spoken_text"])[k] for p in pick) for k in need}
            score = sum(1.0 for k, n in need.items() if f[k] and have[k] < n)
            score += 0.8 * (r["speaker"] not in speakers) + 0.8 * (r["volume"] not in volumes)
            if score > best_score:
                best, best_score = r, score
        pick.append(best)
        speakers.add(best["speaker"])
        volumes.add(best["volume"])
    keep = ["line_id", "vo_id", "text_hash", "speaker", "spoken_text", "emotion_primary", "emotion_secondary",
            "intensity", "pace", "pace_wpm", "volume", "pitch_contour", "pauses", "emphasis"]
    out = {"source": os.path.relpath(a.csv, os.path.join(HERE, "..", "..")),
           "note": "20-line lip-sync gold set (rigging plan §6, §13.2). The first two lines are required by the plan "
                   "(a minor's human line and an UNKNOWN speaker). Hand-marked TextGrids do not exist yet; eval uses "
                   "espeak's phone timings for its own synthetic audio as a machine reference.",
           "lines": [{k: r.get(k, "") for k in keep} | {"key": line_key(r)} for r in pick]}
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"wrote {a.out}: {len(pick)} lines, {len(speakers)} speakers, volumes {sorted(volumes)}")


def load_gold(path):
    return json.load(open(path, encoding="utf-8"))["lines"]


def voice_for(line):
    spk = line["speaker"].upper()
    if spk == "CHILD":
        return "en-us+f4", 80
    if spk.startswith("UNKNOWN"):
        return "en-us+m3", 40
    return "en-us", 50


# ------------------------------------------------------------------------------------------------------------
# synth + aligners
# ------------------------------------------------------------------------------------------------------------
def cmd_synth(a):
    lines = load_gold(a.gold)
    es = Espeak()
    os.makedirs(os.path.join(a.work, "wav"), exist_ok=True)
    os.makedirs(os.path.join(a.work, "ref"), exist_ok=True)
    os.makedirs(os.path.join(a.work, "corpus"), exist_ok=True)
    for ln in lines:
        text = clean_text(ln["spoken_text"])
        voice, pitch = voice_for(ln)
        wpm = int(ln["pace_wpm"] or 150)
        pcm, rate, phones, words, unknown = es.synth(text, voice, wpm, pitch)
        # pad 0.25 s of silence at both ends so aligners and the solver see onsets and releases
        pad = int(0.25 * rate)
        pcm = b"\0\0" * pad + pcm + b"\0\0" * pad
        sh = 0.25
        phones = [("SIL", 0.0, sh)] + [(p, s + sh, e + sh) for p, s, e in phones] + \
                 [("SIL", phones[-1][2] + sh, len(pcm) / 2 / rate)]
        phones = merge_sil(phones)
        words = [(w, s + sh, e + sh) for w, s, e in words]
        k = ln["key"]
        write_wav(os.path.join(a.work, "wav", k + ".wav"), pcm, rate)
        write_wav(os.path.join(a.work, "corpus", k + ".wav"), pcm, rate)
        with open(os.path.join(a.work, "corpus", k + ".lab"), "w") as f:
            f.write(lab_text(text) + "\n")
        with open(os.path.join(a.work, "corpus", k + ".txt"), "w") as f:
            f.write(text + "\n")
        ref = {"key": k, "line_id": ln["line_id"], "source": "espeak-ng phoneme events (synthetic audio)",
               "voice": voice, "wpm": wpm, "duration": len(pcm) / 2 / rate,
               "phones": phones, "words": words, "unmapped_mnemonics": unknown}
        json.dump(ref, open(os.path.join(a.work, "ref", k + ".json"), "w"), indent=0)
        print(f"{ln['line_id']:<16} {k:<28} {ref['duration']:5.2f}s {len(phones):3d} phones"
              + (f"  unmapped {unknown}" if unknown else ""))


def cmd_align_mfa(a):
    out = os.path.join(a.work, "mfa")
    corpus = os.path.join(a.work, "mfa_corpus")
    os.makedirs(corpus, exist_ok=True)
    for f in os.listdir(os.path.join(a.work, "corpus")):
        if f.endswith((".wav", ".lab")):
            src = os.path.join(a.work, "corpus", f)
            dst = os.path.join(corpus, f)
            if not os.path.exists(dst):
                os.link(src, dst) if hasattr(os, "link") else None
    cmd = shlex.split(a.mfa_cmd) + ["align", corpus, a.dict, a.model, out, "--clean", "--single_speaker",
                                    "-j", str(a.jobs), "--beam", "100", "--retry_beam", "400"]
    print("running:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    open(os.path.join(a.work, "mfa.log"), "w").write(r.stdout + "\n" + r.stderr)
    print("mfa exit", r.returncode, "->", out)
    if r.returncode != 0:
        print(r.stderr[-2000:])
    return r.returncode


def cmd_align_rhubarb(a):
    out = os.path.join(a.work, "rhubarb")
    os.makedirs(out, exist_ok=True)
    rc = 0
    for f in sorted(os.listdir(os.path.join(a.work, "corpus"))):
        if not f.endswith(".wav"):
            continue
        k = f[:-4]
        dialog = os.path.join(a.work, "corpus", k + ".txt")
        cmd = [a.rhubarb, "-q", "-f", "json", "--extendedShapes", "GHX", "-d", dialog,
               "-o", os.path.join(out, k + ".json"), os.path.join(a.work, "corpus", f)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            rc = r.returncode
            print(k, "rhubarb failed:", r.stderr.strip()[-400:])
        else:
            print(k, "ok")
    return rc


# ------------------------------------------------------------------------------------------------------------
# alignment -> viseme segments
# ------------------------------------------------------------------------------------------------------------
def phone_viseme(phones, k):
    v = fs.PHONE_TO_VISEME.get(phones[k][0])
    if v is None:
        return "sil"
    if v == "next":
        for j in range(k + 1, len(phones)):
            nv = fs.PHONE_TO_VISEME.get(phones[j][0], "sil")
            if nv != "next":
                return nv
        return "sil"
    return v


def segments_from_phones(phones):
    """[(viseme, start, end, [phones])], consecutive identical visemes merged."""
    segs = []
    for k, (p, s, e) in enumerate(phones):
        v = phone_viseme(phones, k)
        if segs and segs[-1][0] == v:
            segs[-1] = (v, segs[-1][1], e, segs[-1][3] + [p])
        else:
            segs.append((v, s, e, [p]))
    return segs


def segments_from_rhubarb(path, duration):
    cues = json.load(open(path))["mouthCues"]
    segs = []
    for c in cues:
        v = fs.RHUBARB_TO_VISEME[c["value"]]
        if segs and segs[-1][0] == v:
            segs[-1] = (v, segs[-1][1], c["end"], [])
        else:
            segs.append((v, c["start"], c["end"], []))
    if segs and segs[-1][2] < duration:
        segs.append(("sil", segs[-1][2], duration, []))
    return segs


def phones_from_elevenlabs(path, es, duration):
    """ElevenLabs `with-timestamps` response -> phones. Characters are grouped into words; each word's phones come
    from espeak's G2P (synthesising the word alone) and are scaled into the word's aligned span."""
    j = json.load(open(path))
    al = j.get("alignment") or j.get("normalized_alignment") or j
    chars, st, en = al["characters"], al["character_start_times_seconds"], al["character_end_times_seconds"]
    words, cur = [], None
    for c, s, e in zip(chars, st, en):
        if c.isalpha() or c == "'":
            cur = [c, s, e] if cur is None else [cur[0] + c, cur[1], e]
        elif cur is not None:
            words.append(tuple(cur))
            cur = None
    if cur is not None:
        words.append(tuple(cur))
    phones, t = [], 0.0
    for w, s, e in words:
        if s > t:
            phones.append(("SIL", t, s))
        _, _, wp, _, _ = es.synth(w, "en-us", 150, 50)
        wp = [p for p in wp if p[0] != "SIL"]
        if not wp:
            continue
        a0, a1 = wp[0][1], wp[-1][2]
        for p, ps, pe in wp:
            phones.append((p, s + (ps - a0) / (a1 - a0) * (e - s), s + (pe - a0) / (a1 - a0) * (e - s)))
        t = e
    if t < duration:
        phones.append(("SIL", t, duration))
    return merge_sil(phones), [(w, s, e) for w, s, e in words]


# ------------------------------------------------------------------------------------------------------------
# coarticulation solver
# ------------------------------------------------------------------------------------------------------------
def preset_vector(vis, volume="conversational"):
    p = fs.VISEMES[vis]
    v = {c: 0.0 for c in CHANNELS}
    for s, w in p["shapes"].items():
        v[s] = w
    for i, d in enumerate(p["tongue"]):
        v[TONGUE_CHANNELS[i]] = d / TONGUE_RANGE_DEG  # signed -1..1 internally
    jaw = VOLUME_JAW.get(volume, 1.0)
    for s in ("jawOpen", "mouthLowerDownLeft", "mouthLowerDownRight"):
        if s in v:
            v[s] = min(1.0, v[s] * jaw)
    if vis not in fs.CLOSURE_VISEMES and vis != "sil" and vis != "nn":
        for s in ("mouthStretchLeft", "mouthStretchRight"):
            v[s] = min(1.0, v[s] + VOLUME_STRETCH.get(volume, 0.0))
    return v


def dominance(vis):
    return DOMINANCE.get(vis, DOMINANCE_VOWEL)


def smoothstep(x):
    x = min(1.0, max(0.0, x))
    return x * x * (3 - 2 * x)


def solve(segs, duration, volume="conversational", floors=None):
    """Dominance blending (Cohen-Massaro style) -> {channel: [float per frame]} at 60 Hz.

    floors: [(channel, value, start, end)] hard constraints (closures, F2)."""
    n = int(math.ceil(duration * FPS)) + 1
    # the mouth leads the sound slightly (F1 allows up to 2 frames of lead, at most 1 frame late)
    segs = [(v, s - LEAD, e - LEAD, p) for v, s, e, p in segs]
    floors = [(c, val, s - LEAD, e - LEAD) for c, val, s, e in floors or []]
    presets = {v: preset_vector(v, volume) for v in fs.VISEMES}
    out = {c: [0.0] * n for c in CHANNELS}
    for i in range(n):
        t = i * DT
        num = {c: 0.0 for c in CHANNELS if c != "@speech"}
        den = 0.0
        for vis, s, e, _ in segs:
            d = s - t if t < s else (t - e if t > e else 0.0)
            tau = TAU.get(vis, TAU_DEFAULT)
            if d > 4 * tau:
                continue
            w = dominance(vis) * math.exp(-(d / tau) ** 2)
            den += w
            for c in num:
                num[c] += w * presets[vis][c]
        for c in num:
            out[c][i] = num[c] / den if den > 0 else 0.0
    # anticipatory rounding (F4): spread the rounded vowel's lip channels back across transparent consonants
    for k, (vis, s, e, phones) in enumerate(segs):
        if vis not in ("ou", "oh") or (phones and not (set(phones) & fs.ROUNDED_VOWELS)):
            continue
        start = round_window_start(segs, k)
        L = s - start
        if L < DT:
            continue
        for i in range(max(0, int(start * FPS)), min(n, int(s * FPS) + 2)):
            kk = smoothstep((i * DT - start) / L) * ROUND_GAIN
            for c in ("mouthPucker", "mouthFunnel"):
                out[c][i] = max(out[c][i], presets[vis][c] * kk)
    floor_frames = []
    for ch, val, s, e in floors or []:
        idx = [i for i in range(n) if s <= i * DT <= e] or [min(n - 1, max(0, int(round((s + e) / 2 * FPS))))]
        # a smooth plateau (Gaussian shoulders) rather than a spike, so the hard constraint stays inside F3
        # F2 needs the value *reached* inside the phone, so the plateau sits inset from the phone's edges
        inner = [i for i in idx if s + FLOOR_INSET <= i * DT <= e - FLOOR_INSET] or \
            [min(idx, key=lambda i: abs(i * DT - 0.5 * (s + e)))]
        floor_frames.append((ch, val, inner))
        lo, hi = inner[0] * DT, inner[-1] * DT
        top = min(1.0, val + FLOOR_MARGIN)
        for i in range(max(0, idx[0] - 5), min(n, idx[-1] + 6)):
            t = i * DT
            d = lo - t if t < lo else (t - hi if t > hi else 0.0)
            out[ch][i] = max(out[ch][i], top * math.exp(-(d / FLOOR_TAU) ** 2))
    for c in CHANNELS:
        if c != "@speech":
            out[c] = limit(out[c], SLEW_CLOSURE if c in CLOSURE_CHANNELS else SLEW)
    for ch, val, idx in floor_frames:  # safety net: the smoothing pass must not break a hard constraint
        if max(out[ch][i] for i in idx) < val + 1.0 / 255:
            i = max(idx, key=lambda i: out[ch][i])
            out[ch][i] = val + 1.0 / 255
            out[ch] = slew_up(out[ch], SLEW_CLOSURE)
    enforce_hold(out, segs, volume, floor_frames)
    out["@speech"] = speech_channel(segs, n)
    return out


# F3 minimum hold, enforced after the limiter. The displayed viseme of a frame is its nearest preset (as F3 measures
# it). A run shorter than F3_HOLD frames is repaired when it is
#   a phone's viseme (a segment of that viseme covers the frame, +-1 frame): the run is extended into the
#     neighbouring frame by blending that frame toward the run's frame, so the short phone holds for F3_HOLD frames
#     (F4: short phones are reinforced, never dropped);
#   an A-B-A pop of a class no phone asks for: the frame is blended toward the mean of its neighbours.
# A repair is kept only if, with HOLD_MARGIN to spare for 8-bit quantisation, no channel breaks the F3 velocity /
# acceleration limits, no closure floor is lost, and the hold violations near it go down. Transit frames (a class
# passed through between two others that no phone asks for) are left alone; see evaluate_line.
HOLD_MARGIN = 0.012
HOLD_ALPHAS = (0.25, 0.4, 0.55, 0.7, 0.85, 1.0)
HOLD_PASSES = 4


def _hold_runs(cls):
    runs = []
    for i, c in enumerate(cls):
        if runs and runs[-1][0] == c:
            runs[-1][2] += 1
        else:
            runs.append([c, i, 1])
    return runs


def enforce_hold(out, segs, volume, floor_frames):
    n = len(out[CHANNELS[0]])
    chans = [c for c in CHANNELS if c != "@speech"]
    pvecs = {v: preset_dist_vec(v, volume) for v in fs.VISEMES}

    def cls_at(i):
        return classify(out, i, pvecs)

    def is_phone(c, i0, ln):
        return any(v == c and s - DT <= i * DT < e + DT for i in range(i0, i0 + ln) for v, s, e, _ in segs)

    def violations(cls, lo, hi):
        """Hold violations among the runs that touch [lo, hi)."""
        runs = _hold_runs(cls)
        bad = 0
        for k in range(1, len(runs) - 1):
            c, i0, ln = runs[k]
            if ln >= F3_HOLD or i0 + ln <= lo or i0 >= hi:
                continue
            bad += runs[k - 1][0] == runs[k + 1][0] or is_phone(c, i0, ln)
        return bad

    def smooth_ok(lo, hi):
        for c in chans:
            col = out[c]
            lim = (F3_DW_CLOSURE if c in CLOSURE_CHANNELS else F3_DW) - HOLD_MARGIN
            for i in range(max(1, lo), min(n, hi + 1)):
                if abs(col[i] - col[i - 1]) > lim:
                    return False
                if i + 1 < n and abs(col[i + 1] - 2 * col[i] + col[i - 1]) > F3_D2W - HOLD_MARGIN:
                    return False
        return all(max(out[ch][i] for i in idx) >= val for ch, val, idx in floor_frames)

    def try_blend(j, target, cls, want=None):
        """Blends frame j toward the vector `target`, smallest step first; keeps the first step that passes."""
        lo, hi = max(0, j - 8), min(n, j + 9)
        before = violations(cls, lo, hi)
        old = {c: out[c][j] for c in chans}
        for a in HOLD_ALPHAS:
            for c in chans:
                out[c][j] = old[c] + a * (target[c] - old[c])
            nc = cls_at(j)
            if want is not None and nc != want:
                continue
            trial = cls[:j] + [nc] + cls[j + 1:]
            if smooth_ok(j - 2, j + 2) and violations(trial, lo, hi) < before:
                cls[j] = nc
                return True
        for c in chans:
            out[c][j] = old[c]
        return False

    for _ in range(HOLD_PASSES):
        cls = [cls_at(i) for i in range(n)]
        changed = False
        for k, (c, i0, ln) in enumerate(_hold_runs(cls)):
            if ln >= F3_HOLD or i0 == 0 or i0 + ln >= n:
                continue
            if cls[i0] != c:  # an earlier repair in this pass already touched it
                continue
            if is_phone(c, i0, ln):
                src = {ch: out[ch][i0] for ch in chans}
                # extend into the neighbour that is already closer to this viseme
                cand = sorted((i0 - 1, i0 + ln), key=lambda j: dist(frame_vec(out, j), pvecs[c]))
                changed |= any(try_blend(j, src, cls, want=c) for j in cand)
            elif cls[i0 - 1] == cls[i0 + ln]:
                mid = {ch: 0.5 * (out[ch][i0 - 1] + out[ch][i0 + ln]) for ch in chans}
                changed |= try_blend(i0, mid, cls)
        if not changed:
            break


def round_window_start(segs, k):
    """Earliest time rounding may start before segs[k]: back across transparent consonants (and silence),
    at most ROUND_LOOKAHEAD."""
    s = segs[k][1]
    start = s
    for j in range(k - 1, -1, -1):
        vis, js, je, phones = segs[j]
        if vis == "sil" or (phones and all(p in fs.ROUNDING_TRANSPARENT for p in phones)):
            start = js
        else:
            break
    return max(start, s - ROUND_LOOKAHEAD)


def slew_up(x, r):
    """Upper envelope with slope <= r per frame (only raises values: hard floors survive)."""
    y = list(x)
    for i in range(1, len(y)):
        y[i] = max(y[i], y[i - 1] - r)
    for i in range(len(y) - 2, -1, -1):
        y[i] = max(y[i], y[i + 1] - r)
    return y


def limit(x, r):
    """Slew-limit, then a [1 2 1]/4 pass to bound the second difference (F3)."""
    y = slew_up(x, r)
    z = list(y)
    for i in range(1, len(y) - 1):
        z[i] = 0.25 * y[i - 1] + 0.5 * y[i] + 0.25 * y[i + 1]
    return z


def speech_channel(segs, n):
    sp = [0.0] * n
    spans = [(s, e) for v, s, e, _ in segs if v != "sil"]
    if not spans:
        return sp
    merged = [list(spans[0])]
    for s, e in spans[1:]:
        if s - merged[-1][1] < 0.5:
            merged[-1][1] = e
        else:
            merged.append([s, e])
    for i in range(n):
        t = i * DT
        v = 0.0
        for s, e in merged:
            v = max(v, smoothstep((t - (s - 0.15)) / 0.12) * smoothstep(((e + 0.15) - t) / 0.12))
        sp[i] = v
    return sp


def floors_for(segs_or_phones, from_phones=True):
    fl = []
    if from_phones:
        for p, s, e in segs_or_phones:
            v = fs.PHONE_TO_VISEME.get(p)
            for ch, val in fs.VISEME_FLOORS.get(v, {}).items():
                fl.append((ch, val, s, e))
    else:
        for vis, s, e, _ in segs_or_phones:
            for ch, val in fs.VISEME_FLOORS.get(vis, {}).items():
                fl.append((ch, val, s, e))
    return fl


# ------------------------------------------------------------------------------------------------------------
# .nface I/O (matches engine/src/anim/FacePlayer.cpp parseFaceTrack)
# ------------------------------------------------------------------------------------------------------------
def quantize(track):
    n = len(track[CHANNELS[0]])
    data = bytearray(n * len(CHANNELS))
    for c_i, c in enumerate(CHANNELS):
        col = track[c]
        for i in range(n):
            v = (col[i] + 1.0) * 0.5 if c in TONGUE_CHANNELS else col[i]
            data[i * len(CHANNELS) + c_i] = max(0, min(255, int(round(v * 255))))
    return n, bytes(data)


def write_nface(path, track):
    n, data = quantize(track)
    with open(path, "wb") as f:
        f.write(NFACE_MAGIC + struct.pack("<HHIH", NFACE_VERSION, FPS, n, len(CHANNELS)))
        for c in CHANNELS:
            b = c.encode()
            f.write(bytes([len(b)]) + b)
        f.write(data)
    return n


def read_nface(path):
    b = open(path, "rb").read()
    if b[:6] != NFACE_MAGIC:
        raise ValueError(path + ": not .nface")
    ver, rate, frames, nch = struct.unpack_from("<HHIH", b, 6)
    pos, names = 16, []
    for _ in range(nch):
        ln = b[pos]
        names.append(b[pos + 1:pos + 1 + ln].decode())
        pos += 1 + ln
    data = b[pos:]
    if len(data) != frames * nch:
        raise ValueError(path + ": size mismatch")
    tr = {}
    for ci, c in enumerate(names):
        col = [data[i * nch + ci] / 255.0 for i in range(frames)]
        tr[c] = [v * 2 - 1 for v in col] if c in TONGUE_CHANNELS else col
    return rate, tr


# ------------------------------------------------------------------------------------------------------------
# bake
# ------------------------------------------------------------------------------------------------------------
def alignment_for(work, backend, key, duration, es=None):
    """-> (segments, floors, words, note) or None when the backend has no data for this line."""
    if backend == "espeak":
        ref = json.load(open(os.path.join(work, "ref", key + ".json")))
        ph = [tuple(p) for p in ref["phones"]]
        return segments_from_phones(ph), floors_for(ph), ref["words"], ""
    if backend in ("mfa", "textgrid"):
        tg = os.path.join(work, backend, key + ".TextGrid")
        if not os.path.exists(tg):
            return None
        ph, words = textgrid_phones(tg)
        if ph and ph[-1][2] < duration:
            ph.append(("SIL", ph[-1][2], duration))
        note = ""
        if any(e - s < 0.030 for _, s, e in words):
            note = "word < 30 ms (plan §6.2: fall back to Rhubarb)"
        return segments_from_phones(ph), floors_for(ph), words, note
    if backend == "rhubarb":
        js = os.path.join(work, "rhubarb", key + ".json")
        if not os.path.exists(js):
            return None
        segs = segments_from_rhubarb(js, duration)
        return segs, floors_for(segs, from_phones=False), [], ""
    if backend == "elevenlabs":
        js = os.path.join(work, "elevenlabs", key + ".json")
        if not os.path.exists(js):
            return None
        ph, words = phones_from_elevenlabs(js, es or Espeak(), duration)
        return segments_from_phones(ph), floors_for(ph), words, ""
    raise ValueError(backend)


def bake_backend(work, gold, backend, outdir):
    os.makedirs(outdir, exist_ok=True)
    done, skipped = [], []
    es = None
    for ln in gold:
        k = ln["key"]
        duration = wav_duration(os.path.join(work, "wav", k + ".wav"))
        if backend == "elevenlabs" and es is None and os.path.isdir(os.path.join(work, "elevenlabs")):
            es = Espeak()
        al = alignment_for(work, backend, k, duration, es)
        if al is None:
            skipped.append(k)
            continue
        segs, floors, words, note = al
        tr = solve(segs, duration, ln.get("volume") or "conversational", floors)
        frames = write_nface(os.path.join(outdir, k + ".nface"), tr)
        side = {"key": k, "line_id": ln["line_id"], "backend": backend, "frames": frames, "rate": FPS,
                "words": words, "flag": note,
                "direction": {f: ln.get(f, "") for f in ("emotion_primary", "emotion_secondary", "intensity",
                                                         "volume", "pace", "pace_wpm", "pitch_contour", "pauses",
                                                         "emphasis")}}
        json.dump(side, open(os.path.join(outdir, k + ".events.json"), "w"), indent=0, ensure_ascii=False)
        done.append(k)
    return done, skipped


def cmd_bake(a):
    gold = load_gold(a.gold)
    done, skipped = bake_backend(a.work, gold, a.backend, a.out)
    print(f"{a.backend}: baked {len(done)}, no alignment for {len(skipped)}")


# ------------------------------------------------------------------------------------------------------------
# F1-F4
# ------------------------------------------------------------------------------------------------------------
MOUTH_DIST_CHANNELS = SHAPE_CHANNELS + TONGUE_CHANNELS
TONGUE_DIST_WEIGHT = 0.5


def frame_vec(tr, i):
    return [tr[c][i] * (TONGUE_DIST_WEIGHT if c in TONGUE_CHANNELS else 1.0) for c in MOUTH_DIST_CHANNELS]


def preset_dist_vec(vis, volume):
    p = preset_vector(vis, volume)
    return [p[c] * (TONGUE_DIST_WEIGHT if c in TONGUE_CHANNELS else 1.0) for c in MOUTH_DIST_CHANNELS]


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def classify(tr, i, pvecs):
    f = frame_vec(tr, i)
    return min(pvecs, key=lambda v: dist(f, pvecs[v]))


def distinctive_channel(vis, volume):
    """The channel that best tells this viseme from the others (key channel for 'reduced, not dropped')."""
    me = preset_vector(vis, volume)
    others = [preset_vector(v, volume) for v in fs.VISEMES if v not in (vis, "sil")]
    best, score = None, -1e9
    for c in MOUTH_DIST_CHANNELS:
        m = sum(abs(me[c] - o[c]) for o in others) / len(others)
        if abs(me[c]) > 0.05 and m > score:
            best, score = c, m
    return best


def evaluate_line(tr, ref_phones, volume):
    n = len(tr["jawOpen"])
    pvecs = {v: preset_dist_vec(v, volume) for v in fs.VISEMES}
    ph = [tuple(p) for p in ref_phones]
    vis = [phone_viseme(ph, k) for k in range(len(ph))]
    res = {"f1_err": [], "f1_vis": [], "f1_unreached": 0, "f2_frames": 0, "f2_ok": 0, "f2_bilabial": [], "f2_labiodental": [],
           "f3_dw_viol": 0, "f3_d2w_viol": 0, "f3_hold_viol": 0, "f3_hold_flash": 0, "f3_hold_missed": 0, "f3_hold_transit": 0, "f3_max_dw": 0.0, "f3_max_dw_closure": 0.0,
           "f3_max_d2w": 0.0, "f4_round": [], "f4_short": []}
    # rounding look-ahead windows of the reference (a coarticulated neighbour for F2)
    segs = segments_from_phones(ph)
    round_windows = []
    for k, (v, s, e, phs) in enumerate(segs):
        if v in ("ou", "oh") and set(phs) & fs.ROUNDED_VOWELS:
            round_windows.append((round_window_start(segs, k), s, v))

    def sample(c, t):
        f = min(max(t * FPS, 0.0), n - 1.0)
        i0 = int(f)
        i1 = min(i0 + 1, n - 1)
        a = f - i0
        return tr[c][i0] * (1 - a) + tr[c][i1] * a

    def dvec_at(t, target):
        f = min(max(t * FPS, 0.0), n - 1.0)
        i0 = int(f)
        i1 = min(i0 + 1, n - 1)
        a = f - i0
        v0, v1 = frame_vec(tr, i0), frame_vec(tr, i1)
        return dist([x * (1 - a) + y * a for x, y in zip(v0, v1)], pvecs[target])

    # F1: onset = the moment the mouth is closer to the new viseme's preset than to the previous one's
    # (the midpoint of the transition), searched from the previous phone's centre, sampled at 1 ms.
    for k in range(1, len(ph)):
        p, s, e = ph[k]
        v, pv = vis[k], vis[k - 1]
        if v == "sil" or v == pv or e - s < 0.040:
            continue
        if dist(pvecs[v], pvecs[pv]) < 0.10:   # presets too alike to time a change between them
            continue
        t0 = max(0.5 * (ph[k - 1][1] + ph[k - 1][2]), s - 0.120)
        t, onset = t0, None
        while t <= e:
            if dvec_at(t, v) <= dvec_at(t, pv):
                onset = t
                break
            t += 0.001
        if onset is None:
            res["f1_unreached"] += 1
        else:
            res["f1_err"].append(onset - s)
            res["f1_vis"].append((pv, v))
    # F2: frames inside phones show the phone's viseme or a coarticulated neighbour; closure floors
    for k, (p, s, e) in enumerate(ph):
        v = vis[k]
        idx = [i for i in range(n) if s <= i * DT < e]
        if v != "sil":
            ok = {v}
            if k > 0:
                ok.add(vis[k - 1])
            if k + 1 < len(ph):
                ok.add(vis[k + 1])
            for i in idx:
                rw = {rv for (rs, re_, rv) in round_windows if rs <= i * DT <= re_}
                res["f2_frames"] += 1
                res["f2_ok"] += classify(tr, i, pvecs) in (ok | rw)
        if fs.PHONE_TO_VISEME.get(p) in fs.VISEME_FLOORS:
            pick = idx or [min(n - 1, int(round((s + e) / 2 * FPS)))]
            for ch, val in fs.VISEME_FLOORS[fs.PHONE_TO_VISEME[p]].items():
                peak = max(tr[ch][i] for i in pick)
                res["f2_bilabial" if ch == "mouthClose" else "f2_labiodental"].append(peak >= val - 1e-6)
    # F3: every channel, every frame
    for c in CHANNELS:
        col = tr[c]
        lim = F3_DW_CLOSURE if c in CLOSURE_CHANNELS else F3_DW
        for i in range(1, n):
            dw = abs(col[i] - col[i - 1])
            res["f3_max_dw_closure" if c in CLOSURE_CHANNELS else "f3_max_dw"] = max(
                res["f3_max_dw_closure" if c in CLOSURE_CHANNELS else "f3_max_dw"], dw)
            res["f3_dw_viol"] += dw > lim + 1e-6
            if i + 1 < n:
                d2 = abs(col[i + 1] - 2 * col[i] + col[i - 1])
                res["f3_max_d2w"] = max(res["f3_max_d2w"], d2)
                res["f3_d2w_viol"] += d2 > F3_D2W + 1e-6
    # F3 minimum hold. The displayed viseme of a frame is its dominant class (nearest preset). A hold is a run
    # of one class lasting >= F3_HOLD frames. A shorter interior run is one of:
    #   flash   - A-B-A: the mouth leaves a class and comes straight back (a pop);
    #   missed  - the class is the viseme of a reference phone at that frame (+-1 frame), so a phone's viseme
    #             was shown for fewer than F3_HOLD frames (it did not hold);
    #   transit - a class the mouth passes through between two other classes that is no nearby phone's
    #             viseme. Not a hold, so not a violation: with |dw| <= 0.35 per frame and presets 0.15-0.4
    #             apart, blending A -> C has to spend a frame inside some third preset's nearest region, so
    #             counting those frames would make the hold rule unsatisfiable under F3's own velocity limit.
    # Violations = flash + missed. (The previous count took every 1-frame run, transit included: 92/91/119.)
    cls = [classify(tr, i, pvecs) for i in range(n)]
    runs = []
    for i in range(n):
        if runs and runs[-1][0] == cls[i]:
            runs[-1][2] += 1
        else:
            runs.append([cls[i], i, 1])
    for k in range(1, len(runs) - 1):  # interior runs only
        c, i0, ln = runs[k]
        if ln >= F3_HOLD:
            continue
        if runs[k - 1][0] == runs[k + 1][0]:
            res["f3_hold_flash"] += 1
            res["f3_hold_viol"] += 1
        elif any(vis[j] == c for i in range(i0, i0 + ln) for j, (_p, s0, e0) in enumerate(ph)
                 if s0 - DT <= i * DT < e0 + DT):
            res["f3_hold_missed"] += 1
            res["f3_hold_viol"] += 1
        else:
            res["f3_hold_transit"] += 1
    # F4a: rounding lead before /uw ow w/ (plan: "when the preceding consonants allow it")
    for k, (p, s, e) in enumerate(ph):
        if p not in ("UW", "OW", "W"):
            continue
        sk = next(j for j, sg in enumerate(segs) if sg[1] <= s + 1e-9 < sg[2] + 1e-9)
        avail = segs[sk][1] - round_window_start(segs, sk) if segs[sk][1] >= s - 1e-9 else 0.0
        if avail < F4_ROUND_LEAD:
            continue
        rv = vis[k]
        ch = "mouthPucker" if rv == "ou" else "mouthFunnel"
        target = preset_vector(rv, volume)[ch]
        t = s
        while t > s - 0.400 and sample(ch, t) > 0.1 * target:
            t -= 0.001
        res["f4_round"].append(s - t)
    # F4b: short visemes reduced, not dropped
    for k, (p, s, e) in enumerate(ph):
        v = vis[k]
        if v == "sil" or e - s >= F4_SHORT or v in fs.CLOSURE_VISEMES:
            continue
        ch = distinctive_channel(v, volume)
        tgt = preset_vector(v, volume)[ch]
        nb = [ph[j] for j in (k - 1, k + 1) if 0 <= j < len(ph)]
        base = sum(sample(ch, (a + b) / 2) for _, a, b in nb) / max(1, len(nb))
        if abs(tgt - base) < 0.05:
            continue
        ts = [s - DT + (e - s + 2 * DT) * j / 10 for j in range(11)]
        vals = [sample(ch, t) for t in ts]
        peak = max(vals) if tgt > base else min(vals)
        res["f4_short"].append((peak - base) / (tgt - base))
    return res


def pct(xs, q):
    if not xs:
        return float("nan")
    s = sorted(xs)
    k = (len(s) - 1) * q
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def summarize(results):
    f1 = [x for r in results for x in r["f1_err"]]
    absf1 = [abs(x) for x in f1]
    fr = sum(r["f2_frames"] for r in results)
    ok = sum(r["f2_ok"] for r in results)
    bil = [x for r in results for x in r["f2_bilabial"]]
    lab = [x for r in results for x in r["f2_labiodental"]]
    rnd = [x for r in results for x in r["f4_round"]]
    sht = [x for r in results for x in r["f4_short"]]
    s = {
        "lines": len(results),
        "F1": {"onsets": len(f1), "median_abs_ms": 1000 * pct(absf1, 0.5), "p95_abs_ms": 1000 * pct(absf1, 0.95),
               "max_late_ms": 1000 * max(f1, default=float("nan")), "max_lead_ms": -1000 * min(f1, default=float("nan")),
               "late_over_1_frame": sum(x > F1_LATE + 1e-9 for x in f1),
               "lead_over_2_frames": sum(-x > F1_LEAD + 1e-9 for x in f1),
               "never_reached": sum(r["f1_unreached"] for r in results)},
        "F2": {"frames": fr, "in_class_pct": 100.0 * ok / fr if fr else float("nan"),
               "bilabials": len(bil), "bilabials_closed": sum(bil), "labiodentals": len(lab),
               "labiodentals_rolled": sum(lab)},
        "F3": {"max_dw": max(r["f3_max_dw"] for r in results), "max_dw_closure": max(r["f3_max_dw_closure"] for r in results),
               "max_d2w": max(r["f3_max_d2w"] for r in results), "dw_violations": sum(r["f3_dw_viol"] for r in results),
               "d2w_violations": sum(r["f3_d2w_viol"] for r in results),
               "hold_violations": sum(r["f3_hold_viol"] for r in results),
               "hold_flashes": sum(r["f3_hold_flash"] for r in results),
               "hold_missed": sum(r["f3_hold_missed"] for r in results),
               "transit_frames": sum(r["f3_hold_transit"] for r in results)},
        "F4": {"rounding_cases": len(rnd), "rounding_lead_min_ms": 1000 * min(rnd, default=float("nan")),
               "rounding_lead_ok": sum(x >= F4_ROUND_LEAD - 1e-9 for x in rnd), "short_cases": len(sht),
               "short_reduced_not_dropped": sum(x >= 0.25 for x in sht),
               "short_min_fraction": min(sht, default=float("nan"))},
    }
    f = s["F1"]
    s["F1"]["pass"] = bool(f1) and f["median_abs_ms"] <= 1000 * F1_MEDIAN and f["p95_abs_ms"] <= 1000 * F1_P95 \
        and f["late_over_1_frame"] == 0 and f["lead_over_2_frames"] == 0
    s["F2"]["pass"] = fr > 0 and s["F2"]["in_class_pct"] >= 100 * F2_FRAMES and sum(bil) == len(bil) and sum(lab) == len(lab)
    s["F3"]["pass"] = s["F3"]["dw_violations"] == 0 and s["F3"]["d2w_violations"] == 0 and \
        s["F3"]["hold_violations"] == 0
    s["F4"]["pass"] = s["F4"]["rounding_lead_ok"] == len(rnd) and s["F4"]["short_reduced_not_dropped"] == len(sht)
    return s


def cmd_eval(a):
    gold = load_gold(a.gold)
    report = {"reference": "espeak-ng phoneme events for its own synthetic audio (machine reference). "
                           "NOT the hand-marked TextGrids the plan's F1/F2/F4 gate requires.",
              "fps": FPS, "backends": {}}
    for be in a.backends:
        outdir = os.path.join(a.work, "face", be)
        done, skipped = bake_backend(a.work, gold, be, outdir)
        if not done:
            report["backends"][be] = {"status": "not run", "reason": f"no alignment data in {os.path.join(a.work, be)}"}
            continue
        per = []
        for ln in gold:
            if ln["key"] not in done:
                continue
            ref = json.load(open(os.path.join(a.work, "ref", ln["key"] + ".json")))
            _, tr = read_nface(os.path.join(outdir, ln["key"] + ".nface"))   # measure the quantised track
            r = evaluate_line(tr, ref["phones"], ln.get("volume") or "conversational")
            r["key"] = ln["key"]
            per.append(r)
        summ = summarize(per)
        summ["status"] = "run"
        summ["baked"] = len(done)
        summ["missing"] = skipped
        summ["per_line"] = [{"key": r["key"], "f1_median_ms": 1000 * pct([abs(x) for x in r["f1_err"]], 0.5),
                             "f2_pct": 100.0 * r["f2_ok"] / r["f2_frames"] if r["f2_frames"] else None,
                             "f3_viol": r["f3_dw_viol"] + r["f3_d2w_viol"] + r["f3_hold_viol"]} for r in per]
        report["backends"][be] = summ
    with open(a.report, "w") as f:
        json.dump(report, f, indent=1, default=lambda x: None)
    print_table(report)


def print_table(report):
    print("reference:", report["reference"])
    rows = [("backend", "F1 med/p95 ms", "late>1f/lead>2f", "F2 frames%", "PBM / FV", "F3 maxdw/cl/d2w",
             "F3 viol dw/d2/hold(flash+missed) transit", "F4 round ok", "F4 short ok")]
    notes = []
    for be, s in report["backends"].items():
        if s.get("status") != "run":
            notes.append(f"{be}: {s.get('status')} ({s.get('reason', '')})")
            rows.append((be, s.get("status")) + ("",) * 7)
            continue
        rows.append((be, f"{s['F1']['median_abs_ms']:.1f}/{s['F1']['p95_abs_ms']:.1f} (n={s['F1']['onsets']})",
                     f"{s['F1']['late_over_1_frame']}/{s['F1']['lead_over_2_frames']}",
                     f"{s['F2']['in_class_pct']:.1f}",
                     f"{s['F2']['bilabials_closed']}/{s['F2']['bilabials']} {s['F2']['labiodentals_rolled']}/{s['F2']['labiodentals']}",
                     f"{s['F3']['max_dw']:.2f}/{s['F3']['max_dw_closure']:.2f}/{s['F3']['max_d2w']:.2f}",
                     f"{s['F3']['dw_violations']}/{s['F3']['d2w_violations']}/{s['F3']['hold_violations']}({s['F3']['hold_flashes']}+{s['F3']['hold_missed']}) {s['F3']['transit_frames']}",
                     f"{s['F4']['rounding_lead_ok']}/{s['F4']['rounding_cases']}",
                     f"{s['F4']['short_reduced_not_dropped']}/{s['F4']['short_cases']}"))
    w = [max(len(str(r[i])) for r in rows) for i in range(len(rows[0]))]
    for r in rows:
        print("  ".join(str(x).ljust(w[i]) for i, x in enumerate(r)).rstrip())
    for n in notes:
        print(n)


# ------------------------------------------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    default_gold = os.path.join(HERE, "gold_set.json")
    g = sub.add_parser("gold", help="pick the 20-line gold set from a direction CSV")
    g.add_argument("--csv", required=True)
    g.add_argument("--out", default=default_gold)
    g.add_argument("--count", type=int, default=20)
    s = sub.add_parser("synth", help="synthesise gold-set audio with espeak-ng + reference phones")
    s.add_argument("--gold", default=default_gold)
    s.add_argument("--work", required=True)
    m = sub.add_parser("align-mfa", help="run Montreal Forced Aligner on the synthesised corpus")
    m.add_argument("--work", required=True)
    m.add_argument("--mfa-cmd", default=os.environ.get("NE_MFA", "mfa"))
    m.add_argument("--dict", required=True)
    m.add_argument("--model", required=True)
    m.add_argument("--jobs", type=int, default=2)
    r = sub.add_parser("align-rhubarb", help="run Rhubarb Lip Sync (with dialog text) on the corpus")
    r.add_argument("--work", required=True)
    r.add_argument("--rhubarb", default=os.environ.get("NE_RHUBARB", "rhubarb"))
    b = sub.add_parser("bake", help="write .nface tracks for one backend")
    b.add_argument("--gold", default=default_gold)
    b.add_argument("--work", required=True)
    b.add_argument("--backend", required=True, choices=["espeak", "mfa", "rhubarb", "elevenlabs", "textgrid"])
    b.add_argument("--out", required=True)
    e = sub.add_parser("eval", help="bake + measure F1-F4 for several backends side by side")
    e.add_argument("--gold", default=default_gold)
    e.add_argument("--work", required=True)
    e.add_argument("--backends", nargs="+", default=["espeak", "mfa", "rhubarb", "elevenlabs", "textgrid"])
    e.add_argument("--report", required=True)
    a = ap.parse_args(argv)
    return {"gold": cmd_gold, "synth": cmd_synth, "align-mfa": cmd_align_mfa, "align-rhubarb": cmd_align_rhubarb,
            "bake": cmd_bake, "eval": cmd_eval}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
