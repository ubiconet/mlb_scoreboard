# Synthesizes the scoreboard's five score sounds into
# src/sports/mlb/assets/*.wav (16-bit mono 22050 Hz). Run
# tools/gen_audio_clips.py afterwards to bake them into audio_clips.h.
#
# Everything is generated, not sampled, so the repo ships no licensed
# audio: crowds are shaped/filtered noise with whistles, the bat crack is
# an impulse plus damped resonances, the organ is an additive "Charge!"
# fanfare. A fixed RNG seed keeps the clips byte-identical across runs.

import math
import os
import random
import struct
import wave

RATE = 22050
ASSETS = os.path.join(os.path.dirname(__file__), "..", "src", "sports",
                      "mlb", "assets")

random.seed(20261111)


def write_wav(name, samples):
    os.makedirs(ASSETS, exist_ok=True)
    path = os.path.join(ASSETS, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = b"".join(
            struct.pack("<h", max(-32768, min(32767, int(s * 32767.0))))
            for s in samples)
        w.writeframes(frames)
    print("%s: %.2fs, %d samples" % (name, len(samples) / RATE, len(samples)))


def silence(seconds):
    return [0.0] * int(seconds * RATE)


def lowpass(samples, alpha):
    # One-pole LP: alpha in (0, 1] — smaller cutoffs need more passes.
    out = []
    y = 0.0
    for x in samples:
        y += alpha * (x - y)
        out.append(y)
    return out


def highpass(samples, alpha):
    # subtract a slow LP of the signal ( crudely removes rumble )
    lp = lowpass(samples, alpha)
    return [s - l for s, l in zip(samples, lp)]


def mix(dst, src, at_seconds, gain=1.0):
    start = int(at_seconds * RATE)
    end = min(len(dst), start + len(src))
    for i in range(start, end):
        dst[i] += src[i - start] * gain


def ramp(points):
    # Piecewise-linear envelope from [(seconds, level), ...].
    out = []
    for i in range(len(points) - 1):
        (t0, v0), (t1, v1) = points[i], points[i + 1]
        n = max(1, int((t1 - t0) * RATE))
        for k in range(n):
            f = k / n
            out.append(v0 + (v1 - v0) * f)
    return out


def crowd(seconds, lp_alpha, hp_alpha, seed_offset):
    rng = random.Random(20261111 + seed_offset)
    noise = [rng.uniform(-1.0, 1.0) for _ in range(int(seconds * RATE))]
    shaped = highpass(lowpass(noise, lp_alpha), hp_alpha)
    peak = max(1e-9, max(abs(s) for s in shaped))
    return [s / peak for s in shaped]


def whistles(seconds, count, seed_offset, lo=2300.0, hi=3900.0):
    # Short warbling sine bursts — the "fans screaming" texture on top of
    # the crowd bed.
    rng = random.Random(99000 + seed_offset)
    out = silence(seconds)
    for _ in range(count):
        t = rng.uniform(0.05, max(0.06, seconds - 0.7))
        dur = rng.uniform(0.25, 0.6)
        f0 = rng.uniform(lo, hi)
        wobble = rng.uniform(30.0, 90.0)
        burst = []
        n = int(dur * RATE)
        for i in range(n):
            tt = i / RATE
            freq = f0 + wobble * math.sin(2 * math.pi * 7.0 * tt)
            amp = math.sin(math.pi * min(1.0, i / n)) ** 2
            burst.append(0.16 * amp * math.sin(2 * math.pi * freq * tt))
        mix(out, burst, t)
    return out


def bat_crack(lead_in=0.02):
    # Impulse burst + damped resonances (the wood meeting the ball) and a
    # soft crowd echo tail.
    total = 1.2
    out = silence(total)
    at = lead_in
    rng = random.Random(4242)
    n = int(0.0016 * RATE)
    burst = [rng.uniform(-1.0, 1.0) for _ in range(n)]
    mix(out, burst, at, gain=1.0)
    resonators = [
        (700.0, 0.010, 0.55),    # body thump
        (1900.0, 0.030, 0.50),   # crack ring
        (2650.0, 0.022, 0.35),
        (3400.0, 0.012, 0.22),
    ]
    for freq, tau, amp in resonators:
        n = int((6.0 * tau) * RATE)
        tone = []
        for i in range(n):
            tt = i / RATE
            tone.append(amp * math.exp(-tt / tau) *
                        math.sin(2 * math.pi * freq * tt))
        mix(out, tone, at)
    # crowd echo rising just behind the crack
    tail = crowd(total - at - 0.12, 0.16, 0.02, 77)
    env = ramp([(0.0, 0.0), (0.30, 0.30), (0.55, 0.22), (1.05, 0.0)])
    tail = [t * e for t, e in zip(tail, env)]
    mix(out, tail, at + 0.12)
    return out


def cheer_body(seconds, deep=False):
    if deep:
        bed = crowd(seconds, 0.08, 0.008, 2)          # darker roar
        rumble = lowpass(crowd(seconds, 0.015, 0.0, 3), 0.02)
        bed = [b + 0.5 * r for b, r in zip(bed, rumble)]
        peak = max(1e-9, max(abs(s) for s in bed))
        bed = [s / peak for s in bed]
        env = ramp([(0.00, 0.0), (0.45, 0.95), (1.10, 1.0), (1.90, 0.85),
                    (2.60, 0.9), (seconds, 0.0)])
        wh = whistles(seconds, 3, 5)
    else:
        bed = crowd(seconds, 0.16, 0.02, 1)           # brighter cheer
        env = ramp([(0.00, 0.0), (0.18, 0.95), (0.70, 0.75), (1.20, 1.0),
                    (1.90, 0.7), (seconds, 0.0)])
        wh = whistles(seconds, 5, 4)
    out = [b * e for b, e in zip(bed, env)]
    out = [o + w * e for o, w, e in zip(out, wh, env)]
    peak = max(1e-9, max(abs(s) for s in out))
    return [s / peak for s in out]


def organ_note(freq, seconds, amp):
    # Additive organ voice: harmonics + slow vibrato + a detuned twin.
    n = int(seconds * RATE)
    detune = freq * 2.5 / 1000.0  # ~2.5 cents
    out = []
    for i in range(n):
        t = i / RATE
        vib = 1.0 + 0.0035 * math.sin(2 * math.pi * 5.5 * t)
        attack = min(1.0, t / 0.012)
        release = min(1.0, (seconds - t) / 0.06)
        voice = 0.0
        for mult, hamp in ((1.0, 1.0), (2.0, 0.40), (3.0, 0.20), (4.0, 0.08)):
            voice += hamp * math.sin(2 * math.pi * freq * mult * vib * t)
            voice += hamp * 0.6 * math.sin(
                2 * math.pi * (freq + detune) * mult * vib * t)
        out.append(amp * voice * attack * release / 2.5)
    return out


def charge_fanfare():
    # The classic ballpark "Charge!": G3 C4 E4 G4 (hold), E4, G4 (big hold).
    out = silence(3.0)
    t = 0.06
    notes = [
        (196.00, 0.16),   # G3
        (261.63, 0.16),   # C4
        (329.63, 0.16),   # E4
        (392.00, 0.40),   # G4 — "charge..."
        (329.63, 0.14),   # E4 pickup
        (392.00, 0.80),   # G4 — "...UP!"
    ]
    for freq, dur in notes:
        mix(out, organ_note(freq, dur + 0.05, 0.85), t)
        t += dur
    return out[:int(2.6 * RATE)]


def main():
    write_wav("cheer.wav", cheer_body(2.8, deep=False))
    write_wav("roar.wav", cheer_body(4.0, deep=True))
    write_wav("crack.wav", bat_crack())
    crack = bat_crack()
    combo = cheer_body(3.2, deep=False)
    # duck the cheer for the first moment so the crack cuts through, then
    # let the crowd take over
    duck = ramp([(0.0, 0.15), (0.35, 1.0), (3.2, 1.0)])
    combo = [c * d for c, d in zip(combo, duck)]
    for i in range(len(crack)):
        combo[i] += crack[i]
    write_wav("crack_cheer.wav", combo)
    write_wav("organ.wav", charge_fanfare())


if __name__ == "__main__":
    main()
