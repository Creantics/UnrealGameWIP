#!/usr/bin/env python3
"""story_import.py - turn the Forgotten Third design package into the runtime story JSON.

    python3 -I tools/story_import/story_import.py [--build-dir DIR] [--out-dir DIR] [--check]

Reads (all under --build-dir, default design/forgotten_third/build):
    draft55/04a..04e_Script_*.md   the script, parsed with scriptparse.py
    draft55/VO_Master_List.csv     VO ids
    meta/meta_*.csv                one slate row per scene
    meta/inserts_*.csv             GAMEPLAY / CHOICE / HOLD / OPTIONAL / TIMED / VARIANT rows
    flags.csv, cues_final.csv

Writes (under --out-dir, default assets/story):
    story.json, scenes/SC-*.json, loc/en.json

The schema is docs/story_format.md. --check rebuilds in memory, validates the spec's rules on the files
that are on disk, and fails if the files differ from a fresh build (stale or hand-edited output).
Output is deterministic: UTF-8, LF, ensure_ascii=False, indent 1, fixed key order, sorted where order is free.
"""
import argparse
import collections
import csv
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)   # python3 -I drops the script dir from sys.path
import scriptparse as sp   # noqa: E402

FORMAT = 1
TITLE = 'The Forgotten Third'
WORLDS = ['Solhaven', 'Lattice', 'Unwritten', 'Ink-Solhaven', 'Cleft pocket', 'House', 'Restored', 'Interstice']
GAMEPLAY_KINDS = ['CMB', 'ENG', 'PLAY', 'MINI']
BEAT_TYPES = ['line', 'direction', 'choice', 'gameplay', 'cue', 'flag', 'hold', 'end']

# Hold ladder (docs/story_format.md, rulings_advisor_draft55.md section D), seconds.
HOLD_LADDER = {'SC-06-U2': 4.0, 'SC-11-Q3': 3.0, 'SC-14-Q3': 5.0, 'SC-15-Q1': 5.0, 'SC-22-Q1': 8.0, 'SC-EP-03': 12.0}
# Where a ladder hold with no [HOLD] insert goes: after the first direction matching this (default: TELL_RX).
LADDER_ANCHOR = {'SC-EP-03': re.compile(r'^\*They hold\.\*$')}
TELL_SECONDS = 1.75       # ordinary Tell: "two taps and a hold" (ladder says 1.5 to 2.0 s)
INPUT_HOLD_SECONDS = 2.0  # [HOLD: ...] input holds that the ladder does not name
# a Tell: a tap and a hold in the same sentence ("taps twice, and holds")
TELL_RX = re.compile(r'\btap(?:s|ping)?\b[^.]{0,80}?\bhold(?:s|ing)?\b', re.I)

# --------------------------------------------------------------------------------------------------
# Hand-authored choice structure for the 16 [CHOICE: ...] rows (the notation is free text).
# effects: [op, flag, value]. 'self' as next means "back to this prompt".
# --------------------------------------------------------------------------------------------------
def _o(text, effects=(), nxt=None, selectable=True, miss=False):
    return {'text': text, 'effects': [list(e) for e in effects], 'next': nxt, 'selectable': selectable,
            'miss': miss}

CHOICES = {
    'SC-05-D1': [('Offer Torrin the stool', True, [_o('Pull out the third stool', [('set', 'F_CH_TORRIN_STOOL', True)])])],
    'SC-06-U1': [('Name the third term', True, [_o('Name the third term', [('set', 'F_CH_APEX_NAMED_THIRD', True)])])],
    'SC-06-U2': [("Refuse Nil's offer", True, [_o('...No.', [('set', 'F_CH_NIL_OFFER_REFUSED', True)])])],
    'SC-10-Q2': [("The tone of Duy's refusal", False, [
        _o('Thank you. I mean that. I decline.', [('set', 'F_CH_YSMERE_TONE', 'GENTLE')]),
        _o("No. I decline, and I'd ask you not to ask again.", [('set', 'F_CH_YSMERE_TONE', 'FIRM')]),
        _o("I'd love to. Unfortunately, I've read the terms.", [('set', 'F_CH_YSMERE_TONE', 'DRY')])])],
    'SC-11-Q1': [('Declare a door for one', True, [_o('Declare a door for one')])],
    'SC-11-Q2': [('Break the Hush with a line you mean', True, [_o('Stand and say it')])],
    'SC-12-Q2': [('Who does Devin turn to?', False, [_o('Ask Mina', selectable=False), _o('Ask Duy', selectable=False)])],
    'SC-13-Q2': [('Sit with Granite a while, or leave him', False, [
        _o('Sit with Granite', [('set', 'F_CH_GRANITE_SIT', 'SAT')]),
        _o('Walk on', [('set', 'F_CH_GRANITE_SIT', 'WALKED_ON')])])],
    'SC-15-HE1': [('Which friend do you support first?', False, [
        _o('Toby', [('set', 'F_CH_HE1_FIRST_SUPPORT', 'TOBY')]),
        _o('Tamsin Ode and Aurelio', [('set', 'F_CH_HE1_FIRST_SUPPORT', 'QUIET_HANDS')])])],
    'SC-15-HE2': [('Which friend do you support first?', False, [
        _o('Basil', [('set', 'F_CH_HE2_FIRST_SUPPORT', 'BASIL')]),
        _o('Ward', [('set', 'F_CH_HE2_FIRST_SUPPORT', 'WARD')])])],
    'SC-19-Q4': [('Both heroes speak', True, [_o('Duy speaks, then Devin')])],
    'SC-20-Q4': [('Which hero speaks first?', False, [_o('Devin speaks first'), _o('Duy speaks first')])],
    'SC-21-Q2': [('The vote', True, [_o('Raise your hands')])],
    'SC-21-Q3': [
        ("Devin's Pledge: what he leaves in his Door", False, [
            _o("Odette's cloth", [('set', 'F_PL_DEVIN_TOKEN', 'ODETTES_CLOTH')]),
            _o('The ladle', [('set', 'F_PL_DEVIN_TOKEN', 'LADLE')]),
            _o('The lentils', [('set', 'F_PL_DEVIN_TOKEN', 'LENTILS')])]),
        ("Duy's Pledge: what he leaves beside his torn pages", False, [
            _o('The torn pages only', [('set', 'F_PL_DUY_TOKEN', 'PAGES_ONLY')]),
            _o('The pages and the tin of tea', [('set', 'F_PL_DUY_TOKEN', 'PAGES_AND_TEA')]),
            _o('The pages and the last token candy', [('set', 'F_PL_DUY_TOKEN', 'PAGES_AND_CANDY')])])],
    'SC-22-Q2': [('Make the offer', True, [_o('Make the offer')])],
    'SC-25-Q4': [("Will you still want me there?", False, [
        _o('Yes.', nxt='self'),
        _o('No.', nxt='self'),
        _o("I don't know.", [('set', 'F_CH_HONEST_ANSWER', 'I_DONT_KNOW')])])],
}


# --------------------------------------------------------------------------------------------------
# Hand-authored dialogue exchanges for the 7 MINI [GAMEPLAY -> ...] rows (docs/story_mini_dialogue.md).
# The story never leaves the scene: a companion talks the player through the old mini-game as choices.
#
#   MINI[scene] = [(anchor, items), ...]
#   anchor  None: right after the insert's hidden MINI direction (the insert's own anchor); else a unique
#           substring of a body line: the items go just before that line (the notation stays at its anchor).
#   items   _L(speaker, text, delivery?)  invented line, id PH-<scene>-<nnn>
#           _D(text)                      shown direction (filmable action only)
#           _H(seconds, note)             hold
#           _F(op, flag, value)           flag beat (set / clear / require / end)
#           _LB(name)                     a jump target for option `next` (no beat)
#           _C(label, options, scripted)  choice; an option's `next` may name a label
#
# A wrong pick is a `miss` option. The exchange is a linear ladder, so it can never loop:
#   ask (miss -> audit A) -> ask again (miss -> audit B) -> ask a third time (miss -> the companion supplies
#   the answer) -> the right pick (or the supplied answer) joins the concede lines.
# --------------------------------------------------------------------------------------------------
def _L(who, text, how=None):
    return ('line', who, text, how)


def _D(text):
    return ('dir', text)


def _H(seconds, note):
    return ('hold', seconds, note)


def _F(op, flag, value=None):
    return ('flag', op, flag, value)


def _LB(name):
    return ('label', name)


def _C(label, opts, scripted=False):
    return ('choice', label, scripted, opts)


def _ladder(name, ask, opts, audit_a, audit_b, supply, concede=(), first=None, accept=None, bonus=()):
    """opts: [(text, right)]. `first`: where the right pick on the first try goes (default: the concede
    lines). `bonus` beats sit between the supplied answer and the concede lines, reached only through `first`;
    they need `accept` (a scripted pick after the supplied answer that jumps over them)."""
    ok = name + '.ok'
    assert not bonus or accept, name

    def prompt(right_next):
        return _C(ask, [_o(t, nxt=right_next) if right else _o(t, miss=True) for t, right in opts])
    out = [prompt(first or ok)] + list(audit_a) + [prompt(ok)] + list(audit_b) + [prompt(ok)] + list(supply)
    if accept:
        out.append(_C(ask, [_o(accept, nxt=ok)], scripted=True))
    return out + list(bonus) + [_LB(ok)] + list(concede)


_SCORE = 'F_HE_LANTERN_GUARD_SCORE'
# one more gold smudge (0..3): a descending require chain, since a flag beat can only set a value
_SCORE_UP = [_F('require', _SCORE, 2), _F('set', _SCORE, 3), _F('end', _SCORE),
             _F('require', _SCORE, 1), _F('set', _SCORE, 2), _F('end', _SCORE),
             _F('require', _SCORE, 0), _F('set', _SCORE, 1), _F('end', _SCORE)]


def _guard(name, ask, opts, audit_a, audit_b, supply, accept, concede):
    """SC-13-HE1: a right pick on the first try adds a gold smudge."""
    return _ladder(name, ask, opts, audit_a, audit_b, supply, concede, first=name + '.score', accept=accept,
                   bonus=[_LB(name + '.score')] + _SCORE_UP)


def _react(flag, value, *items):
    return [_F('require', flag, value)] + list(items) + [_F('end', flag)]


_PINS = 'F_CH_DIAGRAM_PINS'

MINI = {
    # Lumi decodes Duy's fragments into the grammar; Zee and Basil correct. Lumi reads the result aloud next.
    'SC-09-HE1': [(None, [
        _D('Lumi lays the torn fragments in a row on the floorboards and pushes them around with one finger.'),
        _L('ZEE', 'Start with the seat. What did he call it?'),
    ] + _ladder('seat', '"Seat III is not a seat. It\'s a…"', [
        ('"…a throne."', False), ('"…a place."', True), ('"…Ward\'s."', False)],
        [_L('BASIL', "He'd have underlined a person. Or furniture. It isn't underlined.")],
        [_L('ZEE', 'Somewhere, not someone. Read the next fragment.')],
        [_L('ZEE', 'A place. Somewhere you go, not something you sit on.')],
        [_L('BASIL', "…That's worse, somehow.")]) + _ladder('play', '"Two may…"', [
            ('"…sit."', False), ('"…rule."', False), ('"…play."', True)],
        [_L('ZEE', "It's a board, Lumi. Duy only writes about boards one way.")],
        [_L('BASIL', "What do two people do at a board? I'm asking. I don't know.")],
        [_L('ZEE', "Play. Two may play. It's always a game with him.")],
        [_L('ZEE', '…Of course it is.')]) + _ladder('watch', '"The third…"', [
            ('"…plays."', False), ('"…watches."', True), ('"…waits."', False)],
        [_L('ZEE', 'Look at the third chair.', 'not looking at it')],
        [_L('BASIL', "It never touches anything. It just looks.", 'quietly')],
        [_L('ZEE', 'Watches. The third watches.')],
        [_D('Nil turns the untouched cup a quarter-turn on its saucer.')]) + [
        _F('set', 'F_HE_MARGINALIA_DONE', True)])],

    # Rennie cuts the clip; no wrong answers, Pop reacts to each cut (one choice per F_CH_RECUT_* flag).
    'SC-10-HE1': [(None, [
        _L('RENNIE', 'Okay. Where does it start?', 'to the screen'),
        _C('Where does the clip begin?', [
            _o('"Cold open. Start on the freeze."', [('set', 'F_CH_RECUT_START', 'FROM_THE_FREEZE')]),
            _o('"Start on the match. Let them see the board first."', [('set', 'F_CH_RECUT_START', 'FROM_THE_MATCH')])]),
    ] + _react('F_CH_RECUT_START', 'FROM_THE_FREEZE', _L('POP', 'Starting on a stopped tram. Bold.'))
      + _react('F_CH_RECUT_START', 'FROM_THE_MATCH', _L('POP', 'Copper Ring. Start where it starts.')) + [
        _C("Duy's semifinal speech?", [
            _o('"Cut in the speech. \'I don\'t have a plan.\'"', [('set', 'F_CH_RECUT_SPEECH', True)]),
            _o('"No speech. Let the face do it."', [('set', 'F_CH_RECUT_SPEECH', False)])]),
    ] + _react('F_CH_RECUT_SPEECH', True, _L('POP', 'He said that to a full house. Good for him.'))
      + _react('F_CH_RECUT_SPEECH', False, _L('POP', 'Fewer words. He would approve.')) + [
        _C('How long on the frozen face?', [
            _o('"Short. In and out."', [('set', 'F_CH_RECUT_HOLD', 'SHORT')]),
            _o('"Hold on the face. Let them laugh."', [('set', 'F_CH_RECUT_HOLD', 'LONG_ON_FACE')]),
            _o('"Hold on the hand."', [('set', 'F_CH_RECUT_HOLD', 'LONG_ON_HAND')])]),
    ] + _react('F_CH_RECUT_HOLD', 'SHORT', _L('POP', 'Quick stop. Nobody gets off.'))
      + _react('F_CH_RECUT_HOLD', 'LONG_ON_FACE', _L('POP', 'Harrow Gate.', 'not looking up'))
      + _react('F_CH_RECUT_HOLD', 'LONG_ON_HAND', _L('POP', "…What's he doing with his hand?", 'leaning in')) + [
        _C('What goes under it?', [
            _o('"Cold hum."', [('set', 'F_CH_RECUT_SOUNDBED', 'COLD_HUM')]),
            _o('"Warm hum."', [('set', 'F_CH_RECUT_SOUNDBED', 'WARM_HUM')]),
            _o('"Silence."', [('set', 'F_CH_RECUT_SOUNDBED', 'SILENCE')])]),
    ] + _react('F_CH_RECUT_SOUNDBED', 'COLD_HUM', _L('POP', 'Sounds like the depot at night.'))
      + _react('F_CH_RECUT_SOUNDBED', 'WARM_HUM', _L('POP', 'Sounds like the tram with the heater on.'))
      + _react('F_CH_RECUT_SOUNDBED', 'SILENCE', _L('POP', 'A conductor would announce it. (a beat) Silence is better.')) + [
        _F('set', 'F_HE_RECUT_DONE', True)])],

    # Mina sends the threes; Toby corrects. A right first pick adds a gold smudge (F_HE_LANTERN_GUARD_SCORE).
    'SC-13-HE1': [(None, [
        _F('clear', _SCORE),
        _D('At the table, a big man in a baker\'s apron bends his knees to sit.'),
    ] + _guard('heavy', 'The big one is sitting down. Who goes?', [
        ('"Spotter. Eyes on him."', False), ('"Anchors. Hold him up."', True), ('"Medic. Check him."', False)],
        [_L('TOBY', "He's still going down!")],
        [_L('TOBY', "He's heavy, Mina. You said: carry someone else's weight.")],
        [_L('TOBY', 'Anchors! He needs weight on him, not eyes!')],
        '"…Anchors. Go."',
        [_D('Two volunteers get under his arms. He stands.')]) + [
        _D('A woman at the end of the row drifts sideways, toward an empty chair.'),
    ] + _guard('drift', "She's drifting off the row.", [
        ('"Toby. Anchor."', False), ('"Medic."', False), ('"Toby. Spotter."', True)],
        [_L('TOBY', 'Anchor? I weigh about as much as the chair.')],
        [_L('TOBY', "She's not falling. She's wandering. Somebody just has to see where.")],
        [_L('TOBY', "Spotter. I'll watch her. I'm good at watching.")],
        '"…Spotter. Bait with eyes."',
        [_D('Toby runs alongside the woman, waving both arms. She turns her head to follow him.')]) + [
        _D('In the second row an old man sways, eyes open. His knees start to go.'),
    ] + _guard('faint', "He's going down.", [
        ('"Anchors."', False), ('"Medic. Now."', True), ('"Spotter."', False)],
        [_L('TOBY', "Too slow! He's not walking anymore!")],
        [_L('TOBY', "He doesn't need holding, Mina. He needs catching.")],
        [_L('TOBY', 'Medic! Somebody catch him!')],
        '"…Medic. Go."',
        [_D('A volunteer with a satchel catches the old man and lowers him to the cobbles, away from the table.')]) + [
        _D('Along the road, small gold smudges spread across the grey paving.'),
        _F('set', 'F_HE_LANTERN_GUARD_DONE', True)])],

    # Mina links the Order and the Guard; Fenn and Toby correct.
    'SC-15-HE1': [(None, [
        _L('FENN', 'The Order is yours tonight, Takeda. Where do you want us?'),
    ] + _ladder('front', 'Who holds the front?', [
        ('"The Order. Shields up front."', False), ('"The Quiet Hands. They know these streets."', True),
        ('"Me. I\'ll hold it."', False)],
        [_L('FENN', 'Think, Takeda. Who knows these streets in the dark?')],
        [_L('TOBY', "The people nobody counted, Mina. They've been here the whole time.")],
        [_L('FENN', "The Quiet Hands. They've done it in the dark for years. Let them do it in the light.")],
        [_D('The Quiet Hands step out of the doorways and form a line across the street.')]) + _ladder(
        'cape', 'Who carries the cape?', [
            ('"One knight. The fastest."', False), ('"Fenn. It\'s her order."', False),
            ('"Tamsin Ode and Aurelio. Together."', True)],
        [_L('FENN', 'Count the hands on that cape.')],
        [_L('TOBY', "It's a big cape. It's a two-people cape.")],
        [_L('FENN', "Two. Tamsin Ode and Aurelio. Don't make me say it twice.")],
        [_D('Tamsin Ode and Aurelio lift the cape between them and walk it down the steps.')]) + [
        _F('set', 'F_HE_CARRY_DONE', True)])],

    # Lumi fits the clause (stated, symmetric, testable, on the first page); Ward corrects.
    'SC-15-HE2': [(None, [
        _L('WARD', 'Then write it. I will sign what holds.', 'holding out the quill'),
    ] + _ladder('stated', '"Let it be…"', [
        ('"…presumed that any rule may change."', False), ('"…hoped that the rules may be kinder."', False),
        ('"…written that any rule may be amended…"', True)],
        [_L('WARD', 'No. If it is not stated, someone presumes. Page ninety-one presumed.')],
        [_L('WARD', 'Plainly, Miss Amato. A sentence anyone can point to.')],
        [_L('WARD', 'Written. Stated plainly, where anyone can point to it.')],
        [_L('WARD', 'Stated.', 'writing')]) + _ladder('binds', '"…amended by…"', [
            ('"…the Arbiters."', False), ('"…those it binds."', True), ('"…the champion."', False)],
        [_L('WARD', 'That binds somebody else. A rule that binds others and not its writer is a decree.', 'a rueful breath')],
        [_L('WARD', 'Symmetric. Whoever it binds must hold the pen.')],
        [_L('WARD', 'By those it binds. Ysmere wrote that in a margin forty years ago.')],
        [_L('WARD', 'Symmetric. Testable.', 'writing')]) + _ladder('page', 'Where does it go?', [
            ('"Page ninety-one. Under the footnote."', False), ('"The last page. After everything."', False),
            ('"The first page."', True)],
        [_L('WARD', 'Then it is a correction. Corrections get skipped. I skipped one.')],
        [_L('BASIL', 'Where does anybody start reading?')],
        [_L('WARD', 'The first page. Before the first rule.')],
        [_L('LUMI', 'On the first page. Not the footnotes.'), _L('WARD', '…Not the footnotes.')]) + [
        _F('set', 'F_HE_AMENDMENT_DONE', True)])],

    # Devin and Duy fill Duy's board (R11's worked example). The tapping is pre-pinned when the player pinned
    # clues in SC-14-Q3 (a help, not a branch: a scripted pick instead of the ladder); the existing VO lines are
    # the concede beats.
    'SC-18-Q1': [
        ('And he laughed late', [
            _L('DUY', 'What did yours do when he lied?', 'pen ready'),
        ] + _ladder('laugh', 'What did Torrin do when he lied?', [
            ('"He went quiet."', False), ('"He drank his tea."', False), ('"He laughed."', True)],
            [_L('DUY', "Everyone does that. I've got a column for things only he does.")],
            [_L('DUY', 'Devin. Friday. Your mother\'s table.')],
            [_L('DUY', "The laugh. You told me at your mother's table. I'm just reading it back.", 'writing it in himself')])),
        ('A long silence', [
            _L('DUY', "One more column. The hands. Torrin's hands, before the freeze."),
            _F('require', _PINS, True),
            _D("The card is already pinned to the board, in Devin's handwriting: TWO TAPS. A HOLD."),
            _C("Torrin's hands, before the freeze.", [_o('"Two taps. Then he holds."', nxt='taps.ok')], scripted=True),
            _F('end', _PINS),
        ] + _ladder('taps', "Torrin's hands, before the freeze.", [
            ('"He drums all five fingers."', False), ('"Two taps. Then he holds."', True),
            ('"He grips the sabre."', False)],
            [_L('DUY', 'Smaller than that. I nearly missed it with Nil.')],
            [_L('DUY', 'Count them. I counted Nil\'s. It was a number.')],
            [_L('DUY', 'Two taps and a hold. Nil does it on the cup. Torrin does it on the table.')],
            [_L('DUY', 'Two taps and a hold. Both columns.', 'pinning it')])),
    ],

    # Two segments, each placed inside its own half of the scene: Mina breaks the column's rhythm (Pia
    # corrects, timed by a hold), then Rennie calls Pop awake (Pop answers in his sleep).
    'SC-21-HE1': [
        ('Odette is in the column', [
            _D('The column steps past the bakery: left, right, left, right. Every foot lands on the same beat.'),
            _H(1.5, 'MINI: the column\'s rhythm before the call'),
        ] + _ladder('rhythm', 'When do you call it?', [
            ('"On the beat! LEFT! RIGHT!"', False), ('"Off the beat. Left… right-right. Left."', True),
            ('"Don\'t call. Wait for a gap."', False)],
            [_L('PIA', "They're marching to you, Mina!")],
            [_L('PIA', "Give them a step that isn't theirs!")],
            [_L('PIA', 'Off the beat! Make them trip on it!')],
            [_D('Three walkers at the back of the column stumble out of step. They stop.')]) + [
            _F('set', 'F_HE_HOME_RESISTS_MINA_DONE', True)]),
        ('Copper Ring! Ropewalk!', [
            _H(1.0, 'MINI: Pop in the row before the call'),
        ] + _ladder('stops', 'What do you call?', [
            ('"Home! Pop, we\'re home!"', False), ('"Pop! Wake up!"', False), ('"Copper Ring!"', True)],
            [_L('POP', '…Next stop.', 'eyes open, not seeing her')],
            [_L('POP', '…Start at the start.', 'murmuring')],
            [_L('POP', '…Copper Ring.', 'murmuring')]) + [
            _F('set', 'F_HE_HOME_RESISTS_LATTICE_DONE', True)]),
    ],
}


# --------------------------------------------------------------------------------------------------
# text helpers
# --------------------------------------------------------------------------------------------------
def fold(t):
    """Fold quotes, ellipsis, dashes and whitespace so script text and CSV text compare equal."""
    t = (t.replace('“', '"').replace('”', '"').replace('‘', "'").replace('’', "'")
          .replace('…', '...'))
    return re.sub(r'\s+', ' ', t).strip()


def norm(t):
    """VO / script comparison form: markdown emphasis stripped, then folded."""
    return fold(t.replace('*', ''))


def norm_nodir(t):
    return norm(sp.INLINE_DIR.sub(' ', t))


def titlecase(s):
    small = {'and', 'of', 'the', 'a', 'in', 'to', 'for', 'at', 'on'}
    out = []
    for i, w in enumerate(s.lower().split(' ')):
        out.append(w if (i and w in small) or not w else w[0].upper() + w[1:])
    return ' '.join(out)


def direction_text(line):
    """Stage-direction line -> display text. A line that is one whole *...* span loses the outer stars."""
    s = line.strip()
    if s.startswith('**') and s.endswith('**') and s.count('**') == 2:
        return s[2:-2].strip()
    if s.startswith('*') and s.endswith('*') and not s.startswith('**') and s.count('*') % 2 == 0:
        inner = s[1:-1]
        if re.sub(r'\*\([^)]*\)\*', '', inner).count('*') % 2 == 0:
            return inner.strip()
    return s


def chapter_id(cid):
    return cid.zfill(2) if cid.isdigit() else cid


def chapter_title(sc):
    cid = sc['chapter_id']
    base = titlecase(sc['chapter_title'])
    if cid.startswith('P') and cid[1:].isdigit():
        head = 'Prologue'
    elif cid == 'EP':
        head = 'Epilogue'
    else:
        head = 'Chapter %d' % int(cid)
    note = re.sub(r'^\*|\*$', '', sc['chapter_note']).strip()
    return '%s · %s%s' % (head, base, (' ' + note) if note else '')


def csv_rows(path):
    with open(path, encoding='utf-8', newline='') as fh:
        return list(csv.DictReader(fh))


def csv_glob(pattern):
    rows = []
    for f in sorted(glob.glob(pattern)):
        rows.extend(csv_rows(f))
    return rows


def split_semis(s):
    return [x.strip() for x in s.split(';') if x.strip()]


def dump(obj):
    return json.dumps(obj, ensure_ascii=False, indent=1) + '\n'


TAP_RX = re.compile(r'\btap(?:s|ping)?\b', re.I)
HOVER_RX = re.compile(r'\bhover|\bhesitat', re.I)


def find_tell(sid, body):
    """Body line index after which the scene's Tell hold goes (slate tell_insert=yes scenes only).
    A ladder anchor wins; else the first line (direction or dialogue incl. delivery tag) with a tap and a hold in
    one sentence; else the first tap; else the first hover/hesitation."""
    lines = [(i, ln) for i, ln in enumerate(body) if sp.classify(ln) in ('direction', 'dialogue')]
    if sid in LADDER_ANCHOR:
        rxs = [LADDER_ANCHOR[sid]]
    else:
        rxs = [TELL_RX, TAP_RX, HOVER_RX]
    for rx in rxs:
        for i, ln in lines:
            if rx.search(ln.strip() if sid in LADDER_ANCHOR else direction_text(ln) if sp.classify(ln) == 'direction' else ln):
                return i
    return None


# --------------------------------------------------------------------------------------------------
# gameplay inference
# --------------------------------------------------------------------------------------------------
_HEAD_KIND = {'CMB': 'CMB', 'ENG': 'ENG', 'PLAY': 'PLAY', 'MINI': 'MINI', 'BNT': 'ENG', 'CHC': 'ENG', 'PROMPT': 'ENG',
              'PUZZLE': 'ENG', 'READ': 'ENG', 'EXPLORE': 'PLAY', 'QTE': 'MINI'}


def gameplay_kind(rest):
    m = re.match(r'([A-Za-z]+)\b', rest)
    if m and m.group(1).upper() in _HEAD_KIND and (rest[m.end():m.end() + 1] in (':', ' ', ',', '')):
        return _HEAD_KIND[m.group(1).upper()]
    low = rest[:200].lower()
    if re.search(r'home echo|mini-game|minigame', low):
        return 'MINI'
    if re.search(r'boss|skirmish|\bphase\b|usher|needle|duel|combat|battle|\bspar\b|fight|defense|tutorial: (guard|load|read)', low):
        return 'CMB'
    if re.search(r'free look|walk|explor|roam|free ', low):
        return 'PLAY'
    return 'ENG'


# --------------------------------------------------------------------------------------------------
# build
# --------------------------------------------------------------------------------------------------
class Model:
    pass


def load_inputs(build_dir):
    m = Model()
    d55 = os.path.join(build_dir, 'draft55')
    m.scenes = sp.parse_all(d55)
    m.vo = csv_rows(os.path.join(d55, 'VO_Master_List.csv'))
    m.meta = {r['scene_id']: r for r in csv_glob(os.path.join(build_dir, 'meta', 'meta_*.csv'))}
    m.inserts = csv_glob(os.path.join(build_dir, 'meta', 'inserts_*.csv'))
    m.flags = csv_rows(os.path.join(build_dir, 'flags.csv'))
    m.cues = csv_rows(os.path.join(build_dir, 'cues_final.csv'))
    return m


def flag_default(s):
    s = s.strip()
    if s == 'false':
        return False
    if s == 'true':
        return True
    if re.fullmatch(r'-?\d+', s):
        return int(s)
    return s


def build(build_dir):
    """-> (files {relpath: text}, stats dict, problems [str])"""
    m = load_inputs(build_dir)
    problems = []
    stats = collections.Counter()
    flag_ids = {f['flag_id'] for f in m.flags}

    scene_ids = [s['id'] for s in m.scenes]
    scene_set = set(scene_ids)
    vo_by_scene = collections.defaultdict(list)
    vo_other = []
    for r in m.vo:
        if r['Scene ID'] in scene_set:
            vo_by_scene[r['Scene ID']].append(r)
        else:
            vo_other.append(r)
    ins_by_scene = collections.defaultdict(list)
    for n, r in enumerate(m.inserts):
        if r['scene_id'] not in scene_set:
            problems.append('insert row %d names unknown scene %s' % (n + 2, r['scene_id']))
        ins_by_scene[r['scene_id']].append(r)
    placed = collections.Counter()   # id(insert row) -> times placed
    for s in m.scenes:
        if s['id'] not in m.meta:
            problems.append('no slate row for ' + s['id'])

    loc = {}
    scene_out = {}
    chapters = collections.OrderedDict()
    unmatched_lines = []
    matched_vo = set()
    insert_kinds = collections.Counter()
    placed_kinds = collections.Counter()

    def add_loc(key, text):
        if key in loc and loc[key] != text:
            problems.append('duplicate loc key with different text: ' + key)
        loc[key] = text
        return key

    for s in m.scenes:
        sid = s['id']
        meta = m.meta.get(sid, {})
        body = s['body']
        beats = []
        cnt = collections.Counter()
        short = sid[3:]

        def key_for(kind):
            cnt[kind] += 1
            return 'story.%s.%s%03d' % (sid, kind, cnt[kind])

        # -- anchors
        ins = []   # dicts: row, kind, at (body line index), absorbed
        for r in ins_by_scene.get(sid, []):
            nt = r['notation'].strip()
            km = re.match(r'^\[([A-Z]+)', nt)
            kind = km.group(1) if km else '?'
            pre = r['before_line_prefix']
            at = None
            if pre == '@END':
                at = len(body)
            else:
                fp = fold(pre)
                for i, ln in enumerate(body):
                    if fold(ln).startswith(fp):
                        at = i
                        break
                if at is None:   # tolerate markdown-emphasis differences
                    np_ = norm(pre)
                    for i, ln in enumerate(body):
                        if norm(ln).startswith(np_):
                            at = i
                            break
            ins.append({'row': r, 'kind': kind, 'at': at, 'note': nt, 'absorbed': False})
            insert_kinds[kind] += 1
            if at is None:
                problems.append('%s: insert anchor not found: %r' % (sid, pre))
        at_map = collections.defaultdict(list)
        for it in ins:
            if it['at'] is not None:
                at_map[it['at']].append(it)
        # in-text note lines absorb the first insert of the same keyword anchored on them
        note_abs = {}
        for i, ln in enumerate(body):
            if sp.classify(ln) == 'note':
                kw = sp.INTEXT_NOTE.match(ln).group(1)
                for it in at_map.get(i, []):
                    if it['kind'] == kw and not it['absorbed']:
                        it['absorbed'] = True
                        note_abs[i] = it
                        break

        # -- VO pool: in file order per (speaker, normalized text)
        pool = collections.defaultdict(list)
        for r in vo_by_scene.get(sid, []):
            pool[(r['Speaker'], norm(r['Line (spoken)']))].append(r)

        def take_vo(who, text):
            for k in ((who, norm_nodir(text)), (who, norm(text))):
                lst = [r for r in pool.get(k, []) if r['VO ID'] not in matched_vo]
                if lst:
                    matched_vo.add(lst[0]['VO ID'])
                    return lst[0]['VO ID']
            return None

        has_hold_insert = any(it['kind'] == 'HOLD' for it in ins)
        ladder = HOLD_LADDER.get(sid)
        ladder_done = [False]
        tell_target = None
        if meta.get('tell_insert', '').strip().lower().startswith('y') and not has_hold_insert:
            tell_target = find_tell(sid, body)
            if tell_target is None:
                problems.append('%s: slate says tell_insert=yes but no Tell line was found' % sid)
        line_no = [0]

        def maybe_tell(i):
            if i == tell_target:
                hold_beat(ladder if ladder else TELL_SECONDS, 'Tell')
                ladder_done[0] = True

        def emit(b):
            beats.append(b)
            return len(beats) - 1

        def hold_beat(seconds, note=None):
            b = {'type': 'hold', 'seconds': seconds}
            if note:
                b['note'] = note
            emit(b)

        ph_no = [0]
        labels = {}
        patches = []   # (option dict, label name)
        mini_done = [False]

        def emit_choice(label, scripted, opts, kind):
            ckey = key_for(kind)
            idx = len(beats)
            options = []
            for k, o in enumerate(opts, 1):
                ob = {'text': o['text'], 'loc': add_loc('%s.o%d' % (ckey, k), o['text']),
                      'effects': [{'op': e[0], 'id': e[1], 'value': e[2]} for e in o['effects']],
                      'next': idx if o['next'] == 'self' else None}
                if isinstance(o['next'], int):
                    ob['next'] = o['next']
                elif o['next'] not in (None, 'self'):
                    patches.append((ob, o['next']))
                if not o['selectable']:
                    ob['selectable'] = False
                if o.get('miss'):
                    ob['miss'] = True
                options.append(ob)
            add_loc(ckey, label)
            emit({'type': 'choice', 'label': label, 'loc': ckey, 'scripted': scripted, 'options': options})
            stats['choice_options'] += len(options)

        def emit_block(items):
            """A MINI exchange (see MINI): invented lines get PH- ids, choices / directions pc / pd keys."""
            for item in items:
                t = item[0]
                if t == 'line':
                    _t, who, text, how = item
                    ph_no[0] += 1
                    lid = 'PH-%s-%03d' % (short, ph_no[0])
                    b = {'type': 'line', 'id': lid, 'speaker': who}
                    if how:
                        b['direction'] = how
                    b['text'] = text
                    b['loc'] = add_loc('story.%s.%s' % (sid, lid), text)
                    emit(b)
                    stats['mini_lines'] += 1
                elif t == 'dir':
                    emit({'type': 'direction', 'text': item[1], 'loc': add_loc(key_for('pd'), item[1])})
                elif t == 'hold':
                    hold_beat(item[1], item[2])
                elif t == 'flag':
                    _t, op, fid, value = item
                    if fid not in flag_ids:
                        problems.append('%s: MINI flag beat names unknown flag %s' % (sid, fid))
                    b = {'type': 'flag', 'op': op, 'id': fid}
                    if value is not None:
                        b['value'] = value
                    emit(b)
                elif t == 'label':
                    if item[1] in labels:
                        problems.append('%s: MINI label used twice: %s' % (sid, item[1]))
                    labels[item[1]] = len(beats)
                elif t == 'choice':
                    _t, label, scripted, opts = item
                    for o in opts:
                        for e in o['effects']:
                            if e[1] not in flag_ids:
                                problems.append('%s: MINI option sets unknown flag %s' % (sid, e[1]))
                    if len([o for o in opts if o['selectable'] and not o.get('miss')]) == 0:
                        problems.append('%s: MINI choice %r has no right pick' % (sid, label))
                    emit_choice(label, scripted, opts, 'pc')
                else:
                    problems.append('%s: unknown MINI item %r' % (sid, t))

        # MINI parts placed before a body line of their own (anchor substring)
        mini_at = collections.defaultdict(list)
        for anchor, items in MINI.get(sid, []):
            if anchor is None:
                continue
            hits = [i for i, ln in enumerate(body) if fold(anchor) in fold(ln)]
            if len(hits) != 1:
                problems.append('%s: MINI anchor %r matches %d body lines (need 1)' % (sid, anchor, len(hits)))
            else:
                mini_at[hits[0]].append(items)

        def emit_insert(it):
            nt = it['note']
            kind = it['kind']
            placed[id(it['row'])] += 1
            placed_kinds[kind] += 1
            if kind == 'GAMEPLAY':
                mm = re.match(r'^\[GAMEPLAY\s*→\s*(.*)\]$', nt, re.S)
                rest = mm.group(1) if mm else nt
                params = {'mode': s['mode']}
                tm = re.search(r'about (\d+)(?:\s*-\s*\d+)? minutes?', rest)
                if tm:
                    params['minutes'] = int(tm.group(1))
                if '[OPTIONAL' in rest:
                    params['optional'] = True
                k = gameplay_kind(rest)
                if k == 'MINI':
                    if sid not in MINI:
                        problems.append('%s: MINI gameplay insert has no entry in MINI' % sid)
                    elif mini_done[0]:
                        problems.append('%s: more than one MINI gameplay insert' % sid)
                    else:   # the notation stays as a hidden note; the exchange replaces the hand-off
                        mini_done[0] = True
                        stats['mini_exchanges'] += 1
                        emit({'type': 'direction', 'text': nt, 'note': 'MINI', 'hidden': True})
                        for anchor, items in MINI[sid]:
                            if anchor is None:
                                emit_block(items)
                        return
                stats['gameplay_' + k] += 1
                key = add_loc(key_for('g'), nt)
                emit({'type': 'gameplay', 'kind': k, 'text': nt, 'loc': key, 'params': params})
            elif kind == 'CHOICE':
                spec = CHOICES.get(sid)
                if spec is None:
                    problems.append('%s: CHOICE insert has no entry in CHOICES' % sid)
                    spec = [(nt[9:60], True, [_o(nt[9:60])])]
                for label, scripted, opts in spec:
                    emit_choice(label, scripted, opts, 'c')
            elif kind == 'HOLD':
                seconds = HOLD_LADDER.get(sid, INPUT_HOLD_SECONDS)
                hold_beat(seconds, nt)
                ladder_done[0] = True
                sm = re.search(r'(?<!No flag )\b[Ss]ets (F_[A-Z0-9_]+)', nt)
                if sm and 'No flag set' not in nt:
                    emit({'type': 'flag', 'op': 'set', 'id': sm.group(1), 'value': True})
            else:   # OPTIONAL / TIMED / VARIANT: designer notation, never shown
                b = {'type': 'direction', 'text': nt, 'note': kind, 'hidden': True}
                emit(b)

        for i, ln in enumerate(body):
            for it in at_map.get(i, []):
                if not it['absorbed']:
                    emit_insert(it)
            for items in mini_at.get(i, []):
                emit_block(items)
            if i in note_abs:
                it = note_abs[i]
                if it['kind'] == 'CHOICE':
                    emit_insert(it)
                else:
                    placed[id(it['row'])] += 1
                    placed_kinds[it['kind']] += 1
                    emit({'type': 'direction', 'text': it['note'], 'note': it['kind'], 'hidden': True,
                          'source': ln.strip()})
                continue
            kind = sp.classify(ln)
            if kind == 'blank':
                continue
            if kind == 'dialogue':
                sm = sp.SPEAKER.match(ln)
                line_no[0] += 1
                who, how, text = sm.group('who'), sm.group('how'), sm.group('text')
                vid = take_vo(who, text)
                lid = vid or 'TX-%s-%03d' % (short, line_no[0])
                if not vid:
                    unmatched_lines.append((sid, who, text))
                lkey = add_loc('story.%s.%s' % (sid, lid), text)
                b = {'type': 'line', 'id': lid, 'speaker': who}
                if how:
                    b['direction'] = how
                b['text'] = text
                b['loc'] = lkey
                emit(b)
                maybe_tell(i)
                continue
            if kind == 'note':   # in-text note that no insert covers
                kw = sp.INTEXT_NOTE.match(ln).group(1)
                if kw == 'HOLD':
                    hold_beat(HOLD_LADDER.get(sid, INPUT_HOLD_SECONDS), ln.strip())
                else:
                    emit({'type': 'direction', 'text': ln.strip(), 'note': kw, 'hidden': True})
                    if kw == 'CHOICE':
                        problems.append('%s: in-text CHOICE note without a CHOICE insert' % sid)
                continue
            # direction (italic) or other (bold card)
            txt = direction_text(ln)
            if sid in MINI and txt.startswith('Gameplay:'):
                # describes the retired mini-game mechanic; the exchange replaces it, keep it as a note
                emit({'type': 'direction', 'text': txt, 'note': 'MINI', 'hidden': True})
            else:
                emit({'type': 'direction', 'text': txt, 'loc': add_loc(key_for('d'), txt)})
            maybe_tell(i)
        for it in at_map.get(len(body), []):
            emit_insert(it)
        emit({'type': 'end'})
        for ob, name in patches:
            if name not in labels:
                problems.append('%s: MINI option jumps to unknown label %s' % (sid, name))
            else:
                ob['next'] = labels[name]
        if sid in MINI and not mini_done[0]:
            problems.append('%s: MINI entry but no MINI gameplay insert to expand' % sid)
        if ladder and not ladder_done[0]:
            problems.append('%s: hold-ladder entry %.1f s has no hold beat' % (sid, ladder))

        stats['scenes'] += 1
        for b in beats:
            stats['beat_' + b['type']] += 1

        # scene object
        cast = []
        for c in split_semis(meta.get('cast_on_stage', '')):
            c = re.sub(r'\s*\([^)]*\)', '', c).strip()
            if c and c not in cast:
                cast.append(c)
        scene = collections.OrderedDict([
            ('id', sid),
            ('title', s['title_plain']),
            ('chapter', chapter_id(s['chapter_id'])),
            ('world', meta.get('world', '')),
            ('location', meta.get('location', '')),
            ('time', meta.get('time_of_day', '')),
            ('mode', s['mode']),
            ('gameplay', s['gameplay']),
            ('cast', cast),
            ('props', split_semis(meta.get('props_in', ''))),
            ('cues', split_semis(meta.get('sound_cues', ''))),
            ('beats', beats),
        ])
        add_loc('story.%s.title' % sid, s['title_plain'])
        scene_out[sid] = scene
        ch = chapters.setdefault(chapter_id(s['chapter_id']),
                                 {'id': chapter_id(s['chapter_id']), 'title': chapter_title(s), 'scenes': []})
        ch['scenes'].append(sid)

    for it_row in m.inserts:
        if placed[id(it_row)] != 1:
            problems.append('insert placed %d times: %s %r' % (placed[id(it_row)], it_row['scene_id'],
                                                                it_row['before_line_prefix'][:50]))
    unmatched_vo = [r for sid in scene_ids for r in vo_by_scene.get(sid, []) if r['VO ID'] not in matched_vo]

    story = collections.OrderedDict([
        ('format', FORMAT),
        ('title', TITLE),
        ('start', scene_ids[0]),
        ('chapters', list(chapters.values())),
        ('flags', [{'id': f['flag_id'], 'default': flag_default(f['default'])}
                   for f in sorted(m.flags, key=lambda f: f['flag_id'])]),
        ('cues', [{'id': c['cue_id'], 'kind': {'MUS': 'music', 'SFX': 'sfx'}.get(c['cue_id'].split('-')[0], 'music')}
                  for c in sorted(m.cues, key=lambda c: c['cue_id'])]),
    ])
    files = {'story.json': dump(story)}
    for sid in scene_ids:
        files['scenes/%s.json' % sid] = dump(scene_out[sid])
    files['loc/en.json'] = dump(collections.OrderedDict(sorted(loc.items())))

    stats['vo_rows_in_script_scenes'] = sum(len(v) for v in vo_by_scene.values())
    stats['vo_matched'] = len(matched_vo)
    stats['vo_unmatched'] = len(unmatched_vo)
    stats['vo_other_rows'] = len(vo_other)
    stats['lines_without_vo'] = len(unmatched_lines)
    stats['inserts_total'] = len(m.inserts)
    stats['inserts_placed'] = sum(1 for r in m.inserts if placed[id(r)] == 1)
    stats['loc_keys'] = len(loc)
    info = {'tell_scenes': {sid for sid in scene_ids
                            if m.meta.get(sid, {}).get('tell_insert', '').strip().lower().startswith('y')},
            'unmatched_vo': unmatched_vo, 'unmatched_lines': unmatched_lines, 'insert_kinds': insert_kinds,
            'vo_other': vo_other, 'flag_ids': flag_ids}
    return files, stats, problems, info


# --------------------------------------------------------------------------------------------------
# validation (the spec's "Rules the importer checks"), run on the files on disk
# --------------------------------------------------------------------------------------------------
def validate(out_dir, fresh_files, vo_ids, info):
    errs = []

    def err(msg):
        errs.append(msg)

    on_disk = {}
    for root, _dirs, names in os.walk(out_dir):
        for n in names:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, out_dir).replace(os.sep, '/')
            if rel in fresh_files or rel.endswith('.json'):
                on_disk[rel] = p
    for rel in sorted(set(fresh_files) | set(on_disk)):
        if rel not in on_disk:
            err('missing file: ' + rel)
            continue
        if rel not in fresh_files:
            err('unexpected file: ' + rel)
            continue
        raw = open(on_disk[rel], 'rb').read()
        try:
            txt = raw.decode('utf-8')
        except UnicodeDecodeError:
            err('not UTF-8: ' + rel)
            continue
        if '\r' in txt:
            err('CR in file (not LF): ' + rel)
        if txt != fresh_files[rel]:
            err('differs from a fresh build (stale or hand-edited): ' + rel)

    def load(rel):
        try:
            with open(os.path.join(out_dir, rel), encoding='utf-8') as fh:
                return json.load(fh)
        except (OSError, ValueError) as e:
            err('cannot read %s: %s' % (rel, e))
            return None

    story = load('story.json')
    en = load('loc/en.json')
    if story is None or en is None:
        return errs
    if story.get('format') != FORMAT:
        err('story.json format != %d' % FORMAT)
    chapters = story.get('chapters', [])
    in_story = [sid for ch in chapters for sid in ch['scenes']]
    if len(in_story) != 97:
        err('story.json lists %d scenes, expected 97' % len(in_story))
    if len(set(in_story)) != len(in_story):
        err('a scene appears in more than one chapter slot')
    if story.get('start') not in in_story:
        err('start scene not in chapters')
    flag_ids = {f['id'] for f in story.get('flags', [])}
    cue_ids = {c['id'] for c in story.get('cues', [])}
    if len(flag_ids) != len(story.get('flags', [])):
        err('duplicate flag ids')
    if len(cue_ids) != len(story.get('cues', [])):
        err('duplicate cue ids')
    scene_files = sorted(r for r in on_disk if r.startswith('scenes/'))
    if len(scene_files) != 97:
        err('%d scene files, expected 97' % len(scene_files))
    ch_of = {sid: ch['id'] for ch in chapters for sid in ch['scenes']}
    used_loc = set()
    line_ids = collections.Counter()
    vo_used = collections.Counter()
    for rel in scene_files:
        sc = load(rel)
        if sc is None:
            continue
        sid = sc.get('id')
        if rel != 'scenes/%s.json' % sid:
            err('%s: file name does not match id %s' % (rel, sid))
        if sid not in ch_of:
            err('%s: scene not in any chapter' % sid)
        elif ch_of[sid] != sc.get('chapter'):
            err('%s: chapter field %r != chapter slot %r' % (sid, sc.get('chapter'), ch_of[sid]))
        if sc.get('world') not in WORLDS:
            err('%s: bad world %r' % (sid, sc.get('world')))
        for k in ('title', 'location', 'time', 'mode'):
            if not sc.get(k):
                err('%s: empty %s' % (sid, k))
        for c in sc.get('cues', []):
            if c not in cue_ids:
                err('%s: unknown scene cue %s' % (sid, c))
        used_loc.add('story.%s.title' % sid)
        if en.get('story.%s.title' % sid) != sc.get('title'):
            err('%s: title loc missing or different' % sid)
        beats = sc.get('beats', [])
        if not beats or beats[-1].get('type') != 'end':
            err('%s: does not finish with an end beat' % sid)
        for n, b in enumerate(beats):
            t = b.get('type')
            where = '%s beat %d' % (sid, n)
            if t not in BEAT_TYPES:
                err('%s: unknown type %r' % (where, t))
                continue
            if t == 'end' and n != len(beats) - 1:
                err('%s: end beat is not last' % where)

            def need_loc(obj, text_field):
                k = obj.get('loc')
                if not k:
                    err('%s: missing loc' % where)
                    return
                used_loc.add(k)
                if k not in en:
                    err('%s: loc key not in en.json: %s' % (where, k))
                elif en[k] != obj.get(text_field):
                    err('%s: loc text differs from %s: %s' % (where, text_field, k))

            if t == 'line':
                for f in ('id', 'speaker', 'text', 'loc'):
                    if not b.get(f):
                        err('%s: line missing %s' % (where, f))
                line_ids[b.get('id')] += 1
                if b.get('id', '').startswith('VO-'):
                    vo_used[b['id']] += 1
                    if b['id'] not in vo_ids:
                        err('%s: id %s is not in the VO list' % (where, b['id']))
                need_loc(b, 'text')
                if b.get('loc') != 'story.%s.%s' % (sid, b.get('id')):
                    err('%s: line loc key is not story.<scene>.<id>' % where)
            elif t == 'direction':
                if not b.get('text'):
                    err('%s: direction without text' % where)
                if b.get('hidden'):
                    if 'loc' in b:
                        err('%s: hidden direction carries a loc key' % where)
                else:
                    need_loc(b, 'text')
            elif t == 'choice':
                opts = b.get('options') or []
                if not opts:
                    err('%s: choice without options' % where)
                if b.get('scripted') and len(opts) != 1:
                    err('%s: scripted choice must have exactly one option' % where)
                need_loc(b, 'label')
                for k, o in enumerate(opts, 1):
                    used_loc.add(o.get('loc'))
                    if en.get(o.get('loc')) != o.get('text'):
                        err('%s option %d: loc missing or different' % (where, k))
                    nx = o.get('next')
                    if nx is not None and not (isinstance(nx, int) and 0 <= nx < len(beats)):
                        err('%s option %d: bad next %r' % (where, k, nx))
                    for e in o.get('effects', []):
                        if e.get('op') not in ('set', 'clear') or e.get('id') not in flag_ids:
                            err('%s option %d: bad effect %r' % (where, k, e))
                    if not isinstance(o.get('miss', False), bool):
                        err('%s option %d: miss is not a boolean' % (where, k))
                if opts and not any(o.get('selectable', True) and not o.get('miss') for o in opts):
                    if any(o.get('miss') for o in opts):
                        err('%s: every option is a miss (nothing moves the story on)' % where)
            elif t == 'gameplay':
                if b.get('kind') not in GAMEPLAY_KINDS:
                    err('%s: bad gameplay kind %r' % (where, b.get('kind')))
                if not isinstance(b.get('params'), dict):
                    err('%s: gameplay params not an object' % where)
                need_loc(b, 'text')
            elif t == 'cue':
                if b.get('id') not in cue_ids or b.get('action') not in ('play', 'stop', 'stinger'):
                    err('%s: bad cue' % where)
            elif t == 'flag':
                if b.get('op') not in ('set', 'clear', 'require', 'end'):
                    err('%s: bad flag beat op' % where)
                elif (b.get('op') != 'end' or 'id' in b) and b.get('id') not in flag_ids:
                    err('%s: flag beat names an unknown flag' % where)
            elif t == 'hold':
                if not isinstance(b.get('seconds'), (int, float)) or b['seconds'] <= 0:
                    err('%s: bad hold seconds' % where)
        holds = [b for b in beats if b.get('type') == 'hold']
        if sid in info['tell_scenes'] and not holds:
            err('%s: slate tell_insert=yes but the scene has no hold beat' % sid)
        if sid not in info['tell_scenes'] and any(b.get('note') == 'Tell' for b in holds):
            err('%s: Tell hold in a scene whose slate says tell_insert=no' % sid)
        if sid in HOLD_LADDER and not any(b.get('type') == 'hold' and b.get('seconds') == HOLD_LADDER[sid]
                                           for b in beats):
            err('%s: ladder hold of %.1f s missing' % (sid, HOLD_LADDER[sid]))
    for k, v in line_ids.items():
        if v > 1:
            err('line id used %d times: %s' % (v, k))
    for k, v in vo_used.items():
        if v != 1:
            err('VO id matched %d times: %s' % (v, k))
    for r in info['unmatched_vo']:
        err('VO row not matched to a script line: %s (%s %s: %s)' % (r['VO ID'], r['Scene ID'], r['Speaker'],
                                                                     r['Line (spoken)'][:60]))
    orphans = sorted(set(en) - used_loc)
    for k in orphans[:20]:
        err('en.json key referenced by no beat: ' + k)
    if len(orphans) > 20:
        err('... and %d more unreferenced loc keys' % (len(orphans) - 20))
    return errs


# --------------------------------------------------------------------------------------------------
def write_files(out_dir, files):
    for rel, text in files.items():
        p = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
    # drop scene files that are no longer produced
    sd = os.path.join(out_dir, 'scenes')
    if os.path.isdir(sd):
        for n in os.listdir(sd):
            if n.endswith('.json') and 'scenes/' + n not in files:
                os.remove(os.path.join(sd, n))


def summary(stats, info, out):
    p = lambda *a: print(*a, file=out)
    p('scenes: %d' % stats['scenes'])
    p('beats: ' + ', '.join('%s=%d' % (t, stats['beat_' + t]) for t in BEAT_TYPES))
    p('  gameplay by kind: ' + ', '.join('%s=%d' % (k, stats['gameplay_' + k]) for k in GAMEPLAY_KINDS))
    p('  choice options: %d' % stats['choice_options'])
    p('VO rows in script scenes: %d, matched: %d, unmatched: %d (other VO rows, barks/camp/rest banter: %d)' % (
        stats['vo_rows_in_script_scenes'], stats['vo_matched'], stats['vo_unmatched'], stats['vo_other_rows']))
    p('script lines with no VO row (given TX- ids): %d' % stats['lines_without_vo'])
    for sid, who, text in info['unmatched_lines']:
        p('  %s %s: %s' % (sid, who, text[:80]))
    p('inserts placed: %d of %d (%s)' % (stats['inserts_placed'], stats['inserts_total'],
                                          ', '.join('%s=%d' % kv for kv in sorted(info['insert_kinds'].items()))))
    p('loc keys: %d' % stats['loc_keys'])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--build-dir', default=os.path.join(REPO, 'design', 'forgotten_third', 'build'))
    ap.add_argument('--out-dir', default=os.path.join(REPO, 'assets', 'story'))
    ap.add_argument('--check', action='store_true', help='validate the files in --out-dir instead of writing them')
    a = ap.parse_args(argv)
    files, stats, problems, info = build(a.build_dir)
    if a.check:
        vo_ids = {r['VO ID'] for r in csv_rows(os.path.join(a.build_dir, 'draft55', 'VO_Master_List.csv'))}
        errs = problems + validate(a.out_dir, files, vo_ids, info)
        summary(stats, info, sys.stdout)
        if errs:
            print('CHECK FAILED: %d problem(s)' % len(errs))
            for e in errs[:60]:
                print('  - ' + e)
            return 1
        print('CHECK OK')
        return 0
    write_files(a.out_dir, files)
    summary(stats, info, sys.stdout)
    for e in problems:
        print('PROBLEM: ' + e, file=sys.stderr)
    print('wrote %d files to %s' % (len(files), a.out_dir))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
