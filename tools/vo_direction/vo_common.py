"""Shared helpers for the VO direction tools (standard library only).

The normalisation here defines `text_hash`, the key that lets a direction written for one
draft find the same line in a later draft. Change it only together with every directions CSV.
"""
import csv
import hashlib
import re
import unicodedata

# Columns written by extract_lines.py. Everything else in a directions CSV is a direction column.
SKELETON_COLUMNS = [
    'line_id', 'vo_id', 'text_hash', 'file', 'act', 'scene', 'scene_title', 'seq',
    'speaker', 'kind', 'variant', 'delivery_original', 'text', 'spoken_text',
    'parse_note', 'source_line',
]

# Direction columns, in schema order (design/forgotten_third/voice/line_direction_schema.md).
DIRECTION_COLUMNS = [
    'context', 'emotion_primary', 'emotion_secondary', 'intensity',
    'pace', 'pace_wpm', 'volume', 'distance', 'pitch_contour',
    'pauses', 'emphasis', 'subtext', 'breath_nonverbal',
    'hold_before_s', 'hold_after_s', 'take_note',
    'elevenlabs_text', 'elevenlabs_settings', 'notes',
]

CARRY_COLUMNS = ['carry_status', 'carry_from', 'carry_note']

_INLINE_DIR = re.compile(r'\*\((?:[^()]|\([^()]*\))*\)\*')   # *(beat)*, *(He looks up.)*
_BARE_DIR = re.compile(r'\((?:beat|a beat|pause|a breath[^)]*)\)', re.I)
_TAG = re.compile(r'\[[^\]]*\]')                               # ElevenLabs [audio tags]
_PAUSE_MARK = re.compile(r'(?<![\w/])//?(?![\w/])')            # direction pause marks / and //


def fold(s):
    """Unify typography: curly quotes, dashes, ellipsis, nbsp."""
    s = unicodedata.normalize('NFC', s or '')
    for a, b in (('‘', "'"), ('’', "'"), ('“', '"'), ('”', '"'),
                 ('…', '...'), ('—', '-'), ('–', '-'), (' ', ' ')):
        s = s.replace(a, b)
    return s


def inline_directions(text):
    """The *(...)* acting notes embedded in a script line, without their wrappers."""
    return [m.group(0)[2:-2].strip() for m in _INLINE_DIR.finditer(text or '')]


def spoken(text):
    """Script line -> the words the actor says: inline *(...)* notes removed, markdown emphasis removed."""
    s = _INLINE_DIR.sub(' ', text or '')
    s = _BARE_DIR.sub(' ', s)
    s = s.replace('**', '').replace('*', '').replace('_', ' ')
    return re.sub(r'\s+', ' ', s).strip()


def words_key(text):
    """Words-only, lowercase key. Punctuation, emphasis, [tags], pause marks and CAPS do not count."""
    s = fold(_TAG.sub(' ', text or '')).lower()
    s = s.replace('*', ' ')
    s = re.sub(r"[^a-z0-9' ]+", ' ', s)
    s = re.sub(r"(?<![a-z0-9])'|'(?![a-z0-9])", ' ', s)   # quotes, not apostrophes
    return re.sub(r'\s+', ' ', s).strip()


def surface_key(text):
    """Like words_key but keeps punctuation, so '.' -> '?' or a new dash shows up as a change."""
    s = fold(spoken(text)).lower()
    s = re.sub(r'\s+', ' ', s).strip()
    return s


def text_hash(text):
    """Stable 12-hex hash of a line's words (see words_key)."""
    return hashlib.sha1(words_key(spoken(text)).encode('utf-8')).hexdigest()[:12]


def word_count(text):
    k = words_key(spoken(text))
    return len(k.split()) if k else 0


def read_csv(path):
    with open(path, encoding='utf-8', newline='') as fh:
        r = csv.DictReader(fh)
        return list(r.fieldnames or []), list(r)


def write_csv(path, columns, rows):
    with open(path, 'w', encoding='utf-8', newline='') as fh:
        w = csv.DictWriter(fh, fieldnames=columns, extrasaction='ignore', lineterminator='\n')
        w.writeheader()
        for row in rows:
            w.writerow({c: row.get(c, '') for c in columns})


# ---------------------------------------------------------------------------
# Controlled vocabularies. line_direction_schema.md defines each term; keep the two in step.
EMOTIONS = [
    # warm side
    'warm', 'tender', 'affectionate', 'reassuring', 'hospitable', 'playful', 'teasing',
    'amused', 'wry', 'proud', 'hopeful', 'relieved', 'eager', 'curious', 'excited', 'earnest',
    # cool / controlled
    'dry', 'deadpan', 'formal', 'commanding', 'determined', 'confident', 'guarded', 'wary',
    'evasive', 'solemn', 'reverent', 'nostalgic',
    # hurt / low
    'anxious', 'afraid', 'embarrassed', 'ashamed', 'hurt', 'sad', 'grieving', 'longing',
    'resigned', 'weary', 'numb', 'stunned', 'confused', 'pleading',
    # hot
    'urgent', 'frustrated', 'irritated', 'angry', 'defiant', 'contemptuous', 'cold', 'menacing',
    # register
    'neutral', 'hollow', 'uncanny',
]
PACES = {  # band -> (wpm_low, wpm_high, speed multiplier on the character's base speed)
    'very slow': (80, 105, 0.85),
    'slow': (105, 125, 0.93),
    'normal': (125, 155, 1.00),
    'quick': (155, 180, 1.07),
    'rushed': (180, 230, 1.15),
}
VOLUMES = ['whisper', 'soft', 'conversational', 'raised', 'shout']
DISTANCES = ['intimate', 'near', 'across room', 'far']
CONTOURS = ['falling', 'rising', 'flat', 'arc', 'dipping', 'breaking', 'climbing', 'stepped',
            'trailing', 'clipped', 'lilting']
TAKE_NOTES = ['one_take', 'continuous', 'split_render', 'bark_loop', 'choice_ui', 'silence', 'nonverbal', 'overlap']
# Tags ElevenLabs documents for v3/v4. Anything else in [brackets] is allowed only as a short
# descriptive tag ("[low, gravelly]") or an EXPERIMENTAL tag, and the validator warns on it.
DOCUMENTED_TAGS = {
    'laughs', 'laughs harder', 'starts laughing', 'wheezing', 'whispers', 'sighs', 'exhales',
    'sarcastic', 'curious', 'excited', 'crying', 'snorts', 'mischievously', 'swallows', 'gulps',
    'short pause', 'long pause', 'chuckles', 'clears throat', 'exhales sharply', 'inhales deeply',
}
EXPERIMENTAL_TAGS = {'shouts', 'rushed', 'stammers', 'sniffs', 'hums', 'breathless', 'quietly'}
SILENT_KINDS = {'silence', 'nonverbal'}
TAG_RE = _TAG
