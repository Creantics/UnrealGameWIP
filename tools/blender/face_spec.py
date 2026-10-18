"""The face shape-set spec (docs/rigging_plan.md §5, §7.3): names, regions, viseme and expression presets.

Pure Python (no bpy), so the lip-sync baker, the validator and the template generator share one source.
`make_rig_template.py` writes it to assets/rig/face_presets.json, which the engine reads
(ne/anim/FacePlayer.h). A test checks that the committed JSON equals what this module produces.

Conventions
-----------
* Shape names are the de-facto camelCase facial-action names (52, tongueOut included) plus the
  house shapes below. "Left" is the character's left, which is Blender +X (the character faces -Y).
* `layer` is the §7.3 layering region: upper | eyes | mouth_art | mouth_tone | corrective.
* `mask` is the spatial region a shape may move (validator F11 "region masks"); on a character it is a
  vertex group named `rgn_<mask>` painted by the artist (the proof head builds them procedurally).
* A viseme / expression preset is a sparse {shape: weight} map. `jawOpen` also drives the `jaw` bone:
  angle = jawOpen * jaw_max_deg (character-tunable, default 25 degrees). Tongue presets are bone angles
  (degrees, flexion about local +X) for tongue_01..03.
"""
import json

VERSION = 1

# --- the 52 facial-action shapes, in the canonical order (also the engine's slot order) ---------------
SHAPES_52 = [
    # brows
    "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
    # cheeks
    "cheekPuff", "cheekSquintLeft", "cheekSquintRight",
    # eyes
    "eyeBlinkLeft", "eyeBlinkRight", "eyeLookDownLeft", "eyeLookDownRight", "eyeLookInLeft", "eyeLookInRight",
    "eyeLookOutLeft", "eyeLookOutRight", "eyeLookUpLeft", "eyeLookUpRight", "eyeSquintLeft", "eyeSquintRight",
    "eyeWideLeft", "eyeWideRight",
    # jaw
    "jawForward", "jawLeft", "jawOpen", "jawRight",
    # mouth
    "mouthClose", "mouthDimpleLeft", "mouthDimpleRight", "mouthFrownLeft", "mouthFrownRight", "mouthFunnel",
    "mouthLeft", "mouthLowerDownLeft", "mouthLowerDownRight", "mouthPressLeft", "mouthPressRight", "mouthPucker",
    "mouthRight", "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper", "mouthSmileLeft",
    "mouthSmileRight", "mouthStretchLeft", "mouthStretchRight", "mouthUpperUpLeft", "mouthUpperUpRight",
    # nose
    "noseSneerLeft", "noseSneerRight",
    # tongue
    "tongueOut",
]
assert len(SHAPES_52) == 52 and len(set(SHAPES_52)) == 52

# House shapes (§5.2): added only where the facial-action set cannot hold a look. Combination correctives
# are named a_x_b and are driven by the product of the two weights (§7.3), never keyed directly.
HOUSE_SHAPES = [
    "mouthSmileNoCheek",     # the Third's "smile that does not reach the eyes" (no cheek raise)
    "lipsTogetherSoft",      # nasal hum / relaxed closed lips (viseme nn)
    "eyeLidTension",         # tight lower lids without a squint (anxious, wary)
    "jawOpen_x_mouthSmile",  # corrective: keeps the smile corners on the teeth when the jaw opens
]
ALL_SHAPES = SHAPES_52 + HOUSE_SHAPES
MAX_SHAPES = 64  # F15 budget per face mesh


def _layer(name):
    if "_x_" in name:
        return "corrective"
    if name.startswith(("eyeBlink", "eyeLook")):
        return "eyes"
    if name.startswith(("brow", "cheek", "eyeSquint", "eyeWide", "eyeLidTension")):
        return "upper"
    if name.startswith(("jaw", "tongue", "lipsTogether")) or name in (
            "mouthClose", "mouthFunnel", "mouthPucker", "mouthRollLower", "mouthRollUpper", "mouthShrugLower",
            "mouthShrugUpper") or name.startswith(("mouthPress", "mouthLowerDown", "mouthUpperUp")):
        return "mouth_art"
    return "mouth_tone"  # smile, frown, dimple, stretch, sideways, sneer, smileNoCheek


def _mask(name):
    if name.startswith("brow"):
        return "brow"
    if name.startswith(("eye",)):
        return "eye_l" if name.endswith("Left") else "eye_r" if name.endswith("Right") else "eyes"
    if name.startswith("cheek"):
        return "cheek"
    if name.startswith("nose"):
        return "nose"
    if name.startswith("tongue"):
        return "tongue"
    if name.startswith("jaw"):
        return "jaw"
    return "mouth"


def _mirror(name):
    if name.endswith("Left"):
        return name[:-4] + "Right"
    if name.endswith("Right"):
        return name[:-5] + "Left"
    return None


SHAPE_INFO = {n: {"layer": _layer(n), "mask": _mask(n), "mirror": _mirror(n)} for n in ALL_SHAPES}
# The cheek raise of a smile lives in cheekSquint, so the mouth-only smile also reads in the cheek mask.
SHAPE_INFO["mouthSmileLeft"]["mask"] = SHAPE_INFO["mouthSmileRight"]["mask"] = "mouth"
SHAPE_INFO["eyeLidTension"]["mask"] = "eyes"
SHAPE_INFO["jawOpen_x_mouthSmile"]["mask"] = "mouth"
SHAPE_INFO["cheekPuff"]["mask"] = "cheek"

# Region masks: which vertex groups exist, and which shapes the validator lets move each one.
MASKS = ["brow", "eyes", "eye_l", "eye_r", "cheek", "nose", "mouth", "jaw", "tongue"]
# A shape may move vertices in its own mask, plus these (e.g. a jaw shape moves the mouth too).
MASK_ALLOW = {
    "brow": ["brow"],
    "eyes": ["eyes", "eye_l", "eye_r"],
    "eye_l": ["eye_l", "eyes"],
    "eye_r": ["eye_r", "eyes"],
    "cheek": ["cheek", "mouth", "eyes", "eye_l", "eye_r"],  # the cheek raiser (AU6) lifts the lower lid
    "nose": ["nose", "cheek", "mouth"],
    "mouth": ["mouth", "cheek", "jaw"],
    "jaw": ["jaw", "mouth", "cheek"],
    "tongue": ["tongue"],
}


def sided(base, w):
    return {base + "Left": w, base + "Right": w}


# --- 15 visemes (§5.2) ---------------------------------------------------------------------------------
# Each: shapes (sparse weights over ALL_SHAPES; jawOpen also sets the jaw bone), tongue bone angles (deg).
VISEMES = {
    "sil": {"shapes": {}, "tongue": [0, 0, 0]},
    "PP": {"shapes": {"mouthClose": 1.0, **sided("mouthPress", 0.35)}, "tongue": [0, 0, 0]},
    "FF": {"shapes": {"mouthRollLower": 0.85, **sided("mouthUpperUp", 0.25), "jawOpen": 0.06},
           "tongue": [0, 0, 0]},
    "TH": {"shapes": {"jawOpen": 0.16, "tongueOut": 0.45, **sided("mouthUpperUp", 0.1)}, "tongue": [-8, -10, -6]},
    "DD": {"shapes": {"jawOpen": 0.2, **sided("mouthStretch", 0.15)}, "tongue": [-10, -18, -14]},
    "kk": {"shapes": {"jawOpen": 0.26, **sided("mouthStretch", 0.2)}, "tongue": [8, 6, 0]},
    "CH": {"shapes": {"jawOpen": 0.14, "mouthFunnel": 0.55, "mouthPucker": 0.2, "mouthShrugUpper": 0.2},
           "tongue": [-6, -8, -4]},
    "SS": {"shapes": {"jawOpen": 0.07, **sided("mouthStretch", 0.4), **sided("mouthUpperUp", 0.12)},
           "tongue": [-6, -8, -4]},
    "RR": {"shapes": {"jawOpen": 0.15, "mouthFunnel": 0.35, "mouthPucker": 0.3}, "tongue": [-4, -12, 10]},
    "aa": {"shapes": {"jawOpen": 0.62, **sided("mouthLowerDown", 0.3), **sided("mouthUpperUp", 0.12)},
           "tongue": [4, 0, 0]},
    "E": {"shapes": {"jawOpen": 0.34, **sided("mouthStretch", 0.4), **sided("mouthLowerDown", 0.15)},
          "tongue": [0, -4, 0]},
    "ih": {"shapes": {"jawOpen": 0.24, **sided("mouthStretch", 0.28)}, "tongue": [0, -6, 0]},
    "oh": {"shapes": {"jawOpen": 0.44, "mouthFunnel": 0.65, "mouthPucker": 0.1}, "tongue": [4, 2, 0]},
    "ou": {"shapes": {"jawOpen": 0.14, "mouthPucker": 0.85, "mouthFunnel": 0.35}, "tongue": [4, 2, 0]},
    "nn": {"shapes": {"lipsTogetherSoft": 1.0, "mouthClose": 0.3}, "tongue": [-10, -18, -14]},
}
VISEME_ORDER = list(VISEMES)
assert len(VISEMES) == 15

# Hard constraints the coarticulation solver keeps (F2): inside these phones, the shape stays >= value.
VISEME_FLOORS = {"PP": {"mouthClose": 0.8}, "FF": {"mouthRollLower": 0.5}}

# ARPAbet phone (stress digits stripped) -> viseme. HH takes the following vowel's mouth ("next").
PHONE_TO_VISEME = {
    "P": "PP", "B": "PP", "M": "PP",
    "F": "FF", "V": "FF",
    "TH": "TH", "DH": "TH",
    "T": "DD", "D": "DD", "N": "DD", "L": "DD",
    "K": "kk", "G": "kk", "NG": "kk",
    "CH": "CH", "JH": "CH", "SH": "CH", "ZH": "CH",
    "S": "SS", "Z": "SS",
    "R": "RR", "ER": "RR",
    "AA": "aa", "AE": "aa", "AH": "aa", "AY": "aa", "AW": "aa",
    "EH": "E", "EY": "E",
    "IH": "ih", "IY": "ih", "Y": "ih",
    "AO": "oh", "OW": "oh", "OY": "oh",
    "UW": "ou", "UH": "ou", "W": "ou",
    "HH": "next",
    "SIL": "sil", "SP": "sil", "SPN": "sil", "": "sil",
}
# Phones whose rounding the solver anticipates (F4) and the consonants rounding may spread across.
ROUNDED_VOWELS = {"UW", "OW", "W", "UH", "OY", "AO"}
ROUNDING_TRANSPARENT = {"T", "D", "N", "L", "S", "Z", "K", "G", "HH", "R", "SH", "ZH", "CH", "JH", "TH", "DH"}
# Closures: the hard-constraint classes (F3 allows faster changes for them; F4 never drops them).
CLOSURE_VISEMES = {"PP", "FF"}

# Rhubarb Lip Sync mouth cues -> nearest viseme (Rhubarb's own docs describe A..H, X).
RHUBARB_TO_VISEME = {"A": "PP", "B": "ih", "C": "E", "D": "aa", "E": "oh", "F": "ou", "G": "FF", "H": "DD",
                     "X": "sil"}

# --- 14 base expressions (§7.1), at intensity 5 (weight 1.0). Intensity i scales by 0.2 * i. ----------
EXPRESSIONS = {
    "neutral": {},
    # Duchenne: the smile reaches the eyes (cheek lift + lower-lid squint), brows soft.
    "warm": {**sided("mouthSmile", 1.0), **sided("cheekSquint", 1.0), **sided("eyeSquint", 0.55),
             "browInnerUp": 0.2},
    "amused": {**sided("mouthSmile", 1.0), **sided("cheekSquint", 0.9), **sided("eyeSquint", 0.6),
               "jawOpen": 0.25, **sided("mouthUpperUp", 0.3), "browInnerUp": 0.2, **sided("browOuterUp", 0.3)},
    # asymmetric: a one-sided smile pulled across, one brow up, the other side pressed and lowered
    "wry": {"mouthSmileLeft": 1.0, "mouthLeft": 0.6, "mouthDimpleLeft": 0.3, "cheekSquintLeft": 0.35,
            "browOuterUpLeft": 1.0, "browDownRight": 0.5, "eyeSquintRight": 0.45, "mouthPressRight": 0.45},
    # lifted-chin feel: jaw thrust + chin raised under a pressed, slightly smiling mouth; lids relaxed
    # (shrugLower stays below 1: at full weight with the press it folds the lower lip into the upper one)
    "proud": {"jawForward": 0.6, "mouthShrugLower": 0.65, **sided("mouthSmile", 0.5), **sided("mouthPress", 0.15),
              **sided("eyeBlink", 0.45), **sided("browOuterUp", 0.35)},
    "hopeful": {"browInnerUp": 0.7, **sided("browOuterUp", 0.55), **sided("eyeWide", 0.45),
                **sided("mouthSmile", 0.3), "jawOpen": 0.05},
    # inner brows up (the slanted "sad brow"), corners down, chin pushed up into a pout, gaze lowered
    "sad": {"browInnerUp": 1.0, **sided("browDown", 0.15), **sided("mouthFrown", 0.8), "mouthShrugLower": 0.55,
            **sided("eyeLookDown", 0.35), **sided("eyeBlink", 0.25)},
    "grief": {"browInnerUp": 1.0, **sided("browDown", 0.45), **sided("mouthFrown", 0.9), **sided("mouthStretch", 0.5),
              "mouthShrugLower": 0.5, **sided("eyeSquint", 0.7), **sided("cheekSquint", 0.6), "jawOpen": 0.2},
    "afraid": {"browInnerUp": 0.9, **sided("browOuterUp", 0.7), **sided("eyeWide", 1.0), **sided("mouthStretch", 0.7),
               "jawOpen": 0.25, **sided("mouthLowerDown", 0.3)},
    # knitted worry: inner brows up AND drawn together, eyes a little wide and tense, lips pressed and pulled
    # sideways (not the open mouth of afraid, not the frown of sad)
    "anxious": {"browInnerUp": 0.85, **sided("browDown", 0.35), **sided("mouthPress", 0.5), "mouthRollLower": 0.3,
                "eyeLidTension": 0.45, **sided("eyeWide", 0.25), **sided("mouthStretch", 0.5)},
    "angry": {**sided("browDown", 1.0), **sided("eyeSquint", 0.5), **sided("noseSneer", 0.6), **sided("mouthPress", 0.4),
              **sided("mouthUpperUp", 0.3), "jawForward": 0.2, **sided("mouthFrown", 0.3)},
    # one-sided: sneer, lip curl and a tight half smile dimpled into the cheek, the other brow lifted
    "contempt": {"noseSneerRight": 0.7, "mouthUpperUpRight": 0.4, "mouthSmileRight": 0.55, "mouthRight": 0.4,
                 "mouthDimpleRight": 0.4, "eyeSquintRight": 0.4, "browDownRight": 0.3, **sided("eyeBlink", 0.25),
                 "browOuterUpLeft": 0.35},
    "stunned": {**sided("eyeWide", 1.0), **sided("browOuterUp", 0.75), "browInnerUp": 0.6, "jawOpen": 0.4,
                **sided("mouthLowerDown", 0.25)},
    # flat: lids lowered and tensed (no lift, no smile), brows slightly down, mouth pressed thin
    "cold": {**sided("eyeBlink", 0.38), "eyeLidTension": 0.7, **sided("eyeSquint", 0.25), **sided("browDown", 0.5),
             **sided("mouthPress", 1.0), "mouthRollLower": 0.25, **sided("mouthStretch", 0.2)},
}
EXPRESSION_ORDER = list(EXPRESSIONS)
assert len(EXPRESSIONS) == 14
# Variants that reuse a base (§7.1): uncanny is cold plus a smile that does not reach the eyes.
EXPRESSION_VARIANTS = {"uncanny": {"base": "cold", "add": {"mouthSmileNoCheek": 0.45}},
                       "hollow": {"base": "cold", "add": {}}}

FACE_JOINTS = {"jaw": "jaw", "eye_l": "eye_l", "eye_r": "eye_r", "tongue": ["tongue_01", "tongue_02", "tongue_03"]}
JAW_MAX_DEG = 25.0
EYE_LIMITS_DEG = {"yaw": 30.0, "pitch_up": 20.0, "pitch_down": 25.0}  # F8


def all_weights_valid():
    for table in (VISEMES,):
        for name, p in table.items():
            for s, w in p["shapes"].items():
                assert s in SHAPE_INFO, (name, s)
                assert 0.0 <= w <= 1.0, (name, s, w)
    for name, p in EXPRESSIONS.items():
        for s, w in p.items():
            assert s in SHAPE_INFO, (name, s)
            assert 0.0 <= w <= 1.0, (name, s, w)
    for v in VISEME_FLOORS.values():
        for s in v:
            assert s in SHAPE_INFO
    return True


def layer_weights(expr, vis, speech=1.0, micro=None):
    """§7.3 layering math. expr / vis: {shape: weight}; speech = sigma in [0, 1]. Returns {shape: weight}.
    The engine's FacePlayer::compose implements the same function (tests compare them)."""
    micro = micro or {}
    out = {}
    rho = max(vis.get("mouthFunnel", 0.0), vis.get("mouthPucker", 0.0))
    close = vis.get("mouthClose", 0.0)
    for s in ALL_SHAPES:
        layer = SHAPE_INFO[s]["layer"]
        e, v, m = expr.get(s, 0.0), vis.get(s, 0.0), micro.get(s, 0.0)
        if layer == "upper":
            w = e + m
        elif layer == "eyes":
            w = max(e, v, m)  # blinks arrive in the micro layer
        elif layer == "mouth_art":
            w = max(v, (1.0 - speech) * e) if s == "jawOpen" else v + (1.0 - speech) * e
        elif layer == "mouth_tone":
            w = e * (1.0 - 0.7 * rho) * (1.0 - 0.5 * close)
        else:
            w = 0.0
        out[s] = min(1.0, max(0.0, w))
    for s in ALL_SHAPES:  # correctives: product of the two driver weights
        if SHAPE_INFO[s]["layer"] == "corrective":
            a, b = s.split("_x_")
            if b in out:  # a named shape drives it directly
                drive = out[b]
            else:  # side-less driver: the larger of its Left / Right shapes
                drive = max(out.get(b + "Left", 0.0), out.get(b + "Right", 0.0))
            out[s] = min(1.0, out.get(a, 0.0) * drive)
    return out


def presets_json():
    all_weights_valid()
    return {
        "version": VERSION,
        "shapes": [{"name": n, **SHAPE_INFO[n]} for n in ALL_SHAPES],
        "masks": MASKS,
        "mask_allow": MASK_ALLOW,
        "visemes": [{"name": v, "shapes": VISEMES[v]["shapes"], "tongue_deg": VISEMES[v]["tongue"]}
                    for v in VISEME_ORDER],
        "viseme_floors": VISEME_FLOORS,
        "phone_to_viseme": PHONE_TO_VISEME,
        "rhubarb_to_viseme": RHUBARB_TO_VISEME,
        "expressions": [{"name": e, "shapes": EXPRESSIONS[e]} for e in EXPRESSION_ORDER],
        "expression_variants": EXPRESSION_VARIANTS,
        "face_joints": FACE_JOINTS,
        "jaw_max_deg": JAW_MAX_DEG,
        "eye_limits_deg": EYE_LIMITS_DEG,
        "max_shapes": MAX_SHAPES,
    }


def write_presets(path):
    import os
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(presets_json(), f, indent=1, sort_keys=True)
        f.write("\n")


if __name__ == "__main__":
    import sys
    write_presets(sys.argv[1] if len(sys.argv) > 1 else "assets/rig/face_presets.json")
