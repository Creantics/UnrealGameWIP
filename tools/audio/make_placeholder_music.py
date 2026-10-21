"""Writes a placeholder music loop: a beat track with a chord per bar, an intro bar before the loop
and a different tail bar after it, so a wrong loop is easy to hear.

    python tools/audio/make_placeholder_music.py --bpm 60 --out Data/Audio/Music/MUS-KNK.wav

It prints the cues.json fields to use (bpm, offset, loopStart, loopEnd). The level is set to
-18 LUFS (BS.1770-4) with the true peak under -1 dBTP, the music bus's rules in ASSET_INTAKE.md.
Only numbers and tones: no samples from anywhere.
"""
import argparse
import wave

import numpy as np
from scipy.signal import lfilter, resample_poly

RATE = 48000


def k_weight(x):
    # BS.1770 pre-filter and RLB high-pass, 48 kHz coefficients.
    b1, a1 = [1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585]
    b2, a2 = [1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621]
    return lfilter(b2, a2, lfilter(b1, a1, x, axis=0), axis=0)


def lufs(x):
    y = k_weight(x)
    block, hop = int(0.4 * RATE), int(0.1 * RATE)
    powers = np.array([np.sum(np.mean(y[i:i + block] ** 2, axis=0)) for i in range(0, len(y) - block + 1, hop)])
    loud = -0.691 + 10 * np.log10(powers + 1e-20)
    gated = powers[loud > -70]
    rel = -0.691 + 10 * np.log10(np.mean(gated)) - 10
    gated = powers[(loud > -70) & (loud > rel)]
    return -0.691 + 10 * np.log10(np.mean(gated))


def true_peak_db(x):
    return 20 * np.log10(np.max(np.abs(resample_poly(x, 4, 1, axis=0))) + 1e-20)


def tone(freq, seconds, decay):
    t = np.arange(int(seconds * RATE)) / RATE
    return np.sin(2 * np.pi * freq * t) * np.exp(-t * decay)


def kick(seconds):
    t = np.arange(int(seconds * RATE)) / RATE
    f = 50 + 70 * np.exp(-t * 30)
    return np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-t * 9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bpm", type=float, default=60.0)
    ap.add_argument("--bars", type=int, default=4, help="bars in the loop")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    beat = 60.0 / a.bpm
    bar = 4 * beat
    total = bar * (a.bars + 2)  # intro bar, the loop, a tail bar
    mix = np.zeros((int(round(total * RATE)) + RATE, 2))

    def add(sig, at, gain, pan=0.0):
        i = int(round(at * RATE))
        n = min(len(sig), len(mix) - i)
        mix[i:i + n, 0] += sig[:n] * gain * (1 - pan)
        mix[i:i + n, 1] += sig[:n] * gain * (1 + pan)

    # Chord roots per loop bar, in Hz; the intro and tail use their own.
    roots = [220.0, 174.61, 196.0, 164.81]
    for b in range(a.bars + 2):
        start = b * bar
        intro, tail = b == 0, b == a.bars + 1
        for k in range(4):
            add(kick(beat), start + k * beat, 0.9 if k == 0 else 0.6)
            if not intro:
                add(tone(2400 if tail else 1800, 0.05, 60), start + k * beat + beat / 2, 0.08, 0.4)
        if intro:
            continue
        root = 880.0 if tail else roots[(b - 1) % len(roots)]
        for ratio, pan in ((1.0, -0.3), (1.26, 0.0), (1.5, 0.3)):
            add(tone(root * ratio, bar, 0.8), start, 0.12, pan)
    mix = mix[:int(round(total * RATE))]

    mix *= 10 ** ((-18.0 - lufs(mix)) / 20)
    peak = true_peak_db(mix)
    if peak > -1.2:
        mix *= 10 ** ((-1.2 - peak) / 20)
    pcm = np.clip(np.round(mix * 32767), -32768, 32767).astype("<i2")
    with wave.open(a.out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())
    print(f"{a.out}: {total:.3f} s, {lufs(mix):.2f} LUFS, true peak {true_peak_db(mix):.2f} dBTP")
    print(f'cues.json: "bpm": {a.bpm:g}, "offset": 0, "loopStart": {bar:g}, "loopEnd": {bar * (a.bars + 1):g}')


if __name__ == "__main__":
    main()
