#!/usr/bin/env python3
"""The native sound driver (src/game/vcs_sound.cpp) checked against the emulated IOP on identical input.

    python tools/soundcheck.py record OUT [--seconds N] [--mission N]   reference run: emulated IOP's sound
    python tools/soundcheck.py replay OUT                                its sound RPCs replayed into the native driver
    python tools/soundcheck.py compare OUT                               SPU2 register writes and audio, side by side
    python tools/soundcheck.py all OUT [...]                             the three in a row

`record` plays from the rig's Fort Baxter card (on foot, then an AI drive with the radio; --mission N starts a mission
for cutscene audio) with VCS_NATIVE_SOUND=0, recording every call to CDSTREAM's sound servers with its IOP time
(PS2X_RPC_RECORD), the emulated SPU2's register writes (PS2X_SPU2_TRACE) and the audio output (PS2X_AUDIO_DUMP).
`replay` runs vcs.exe in VCS_SOUND_REPLAY mode: the recorded calls go to the native driver at the same IOP times,
without the game, writing its own trace and audio and listing every command whose reply differs from the IOP's (the
game acts on them: "is this voice still playing" decides whether an engine sound is restarted). A stream open the IOP
took while the native engine is still finishing a stop waits for it (up to 1 s of IOP time), as the game's state polls
would live. `compare` reports how many register writes match per second,
the first differences, and how close the two audio outputs are.
"""
import argparse
import array
import collections
import math
import os
import subprocess
import sys
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIDS = "65686578,65686579"
RATE = 48000


def record(out, seconds=40, mission=None):
    os.makedirs(out, exist_ok=True)
    for name in ("rpc.bin", "ref.0", "ref.wav"):
        if os.path.exists(os.path.join(out, name)):
            os.remove(os.path.join(out, name))
    env = {"VCS_NATIVE_SOUND": "0", "PS2X_RPC_RECORD": os.path.join(out, "rpc.bin"), "PS2X_RPC_RECORD_SIDS": SIDS,
           "PS2X_SPU2_TRACE": os.path.join(out, "ref"), "PS2X_AUDIO_DUMP": os.path.join(out, "ref.wav")}
    g = rig.Game.start(save=rig.default_save(), name="soundcheck", port=47662,
                       data_dir=os.path.join(ROOT, "build", "soundcheck", "data"), env=env)
    try:
        g.wait_ingame()
        g.set_time(12)
        g.invincible()
        time.sleep(6)
        if mission is not None:
            g.mission(mission)
            time.sleep(seconds)
        else:
            g.teleport(575.0, 175.0, -100.0, heading=0.0)
            time.sleep(3)
            g.autodrive(speed=40.0)
            time.sleep(seconds)
    finally:
        g.quit()
    print("recorded %s (%.1f MB of calls)" % (out, os.path.getsize(os.path.join(out, "rpc.bin")) / 1e6))


def replay(out):
    for name in ("nat.1", "native.wav"):
        if os.path.exists(os.path.join(out, name)):
            os.remove(os.path.join(out, name))
    env = dict(os.environ)
    env.update({"VCS_SOUND_REPLAY": os.path.join(out, "rpc.bin"), "VCS_SOUND_REPLAY_OUT": os.path.join(out, "native.wav"),
                "PS2X_SPU2_TRACE": os.path.join(out, "nat"), "PS2X_GS": "hw",
                "VCS_DATA_DIR": os.path.join(ROOT, "build", "soundcheck", "data_replay")})
    tc.test_window(env)
    os.makedirs(env["VCS_DATA_DIR"], exist_ok=True)
    r = subprocess.run([tc.VCS_EXE, tc.find_iso()], env=env, cwd=out, capture_output=True, text=True, timeout=600)
    lines = [l for l in (r.stdout + r.stderr).splitlines() if "[sound]" in l]
    print("\n".join(l for l in lines if "replay" in l))  # calls replayed; any replies that differ from the IOP's
    if r.returncode != 0:
        raise SystemExit("replay failed (%d)" % r.returncode)


def load_trace(path):
    rows = []
    with open(path) as f:
        for line in f:
            t, rest = line.rstrip("\n").split(" ", 1)
            rows.append((int(t), rest))
    return rows


def compare_traces(ref, nat):
    """Writes per one-second window as multisets: what matches regardless of small shifts in time."""
    a, b = load_trace(ref), load_trace(nat)
    wa, wb = collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    for t, op in a:
        wa[t // RATE][op] += 1
    for t, op in b:
        wb[t // RATE][op] += 1
    matched = only_ref = only_nat = 0
    missing, extra = collections.Counter(), collections.Counter()
    for w in sorted(set(wa) | set(wb)):
        common = wa[w] & wb[w]
        matched += sum(common.values())
        r, n = wa[w] - common, wb[w] - common
        only_ref += sum(r.values())
        only_nat += sum(n.values())
        missing.update(r)
        extra.update(n)
    # writes slipping across a window boundary match in the neighbour window: count those as matched
    slipped = sum((missing & extra).values())
    total = max(len(a), 1)
    print("SPU2 trace: reference %d writes / DMAs, native %d" % (len(a), len(b)))
    print("  same second: %d (%.2f%%), across a boundary: %d, only reference: %d, only native: %d"
          % (matched, 100.0 * matched / total, slipped, only_ref - slipped, only_nat - slipped))
    first = 0
    while first < min(len(a), len(b)) and a[first][1] == b[first][1]:
        first += 1
    print("  identical in order for the first %d entries" % first)
    if first < min(len(a), len(b)):
        print("  first difference at sample %d / %d: %r vs %r" % (a[first][0], b[first][0], a[first][1], b[first][1]))
    for label, c in (("only reference", missing - extra), ("only native", extra - missing)):
        if c:
            print("  most common %s: %s" % (label, ", ".join("%s x%d" % kv for kv in c.most_common(8))))
    return matched / total


def read_wav(path):
    with wave.open(path, "rb") as w:
        data = array.array("h", w.readframes(w.getnframes()))
    return data[0::2], data[1::2]


def envelope(x, step=480):
    return [math.sqrt(sum(v * v for v in x[i:i + step]) / step) for i in range(0, len(x) - step, step)]


def compare_audio(ref, nat):
    rl, rr = read_wav(ref)
    nl, nr = read_wav(nat)
    er, en = envelope(rl), envelope(nl)
    # lag of the native output against the reference, in 10 ms steps (the dump starts when audio output starts)
    best, lag = -2.0, 0
    n = min(len(er), len(en))
    ma, mb = sum(er[:n]) / max(n, 1), sum(en[:n]) / max(n, 1)
    for d in range(-300, 301):
        xs = [(er[i] - ma, en[i + d] - mb) for i in range(max(0, -d), min(n, n - d))]
        num = sum(p * q for p, q in xs)
        den = math.sqrt(sum(p * p for p, _ in xs) * sum(q * q for _, q in xs)) or 1.0
        if num / den > best:
            best, lag = num / den, d
    print("audio: reference %.1f s, native %.1f s; loudness envelopes correlate %.3f at a lag of %+d ms"
          % (len(rl) / RATE, len(nl) / RATE, best, lag * 10))
    # Per 5 s window, to the sample: the emulated IOP's SPU2 runs up to one IOP time batch behind the IOP clock (its
    # register writes land a few ms before the call that caused them), the native one applies each call at its IOP
    # time - so the offset between the two outputs moves a little from window to window. Each window gets its own.
    def corr(start, d, length, stride):
        num = da = db = 0
        for i in range(start, min(start + length, len(rl), len(nl) - d), stride):
            if i + d >= 0:
                a, b = rl[i], nl[i + d]
                num += a * b
                da += a * a
                db += b * b
        return num / math.sqrt(da * db) if da and db else 0.0
    window = 5 * RATE
    rows = []
    sig = diff = 0
    for start in range(0, min(len(rl), len(nl)) - window, window):
        level = max(range(start, start + window - RATE // 8, RATE // 8),
                    key=lambda k: sum(abs(v) for v in rl[k:k + RATE // 8:16]))
        coarse = max(range(lag * 480 - 2500, lag * 480 + 2501, 4), key=lambda d: corr(level, d, RATE // 8, 8))
        shift = max(range(coarse - 4, coarse + 5), key=lambda d: corr(level, d, RATE // 8, 2))
        s = d = 0
        for i in range(start, start + window):
            j = i + shift
            if 0 <= j < len(nl):
                for a, b in ((rl[i], nl[j]), (rr[i], nr[j])):
                    s += a * a
                    d += (a - b) * (a - b)
        if s < window * 200:  # silence (RMS under 10)
            continue
        sig += s
        diff += d
        rows.append((start // RATE, shift, 10 * math.log10(s / d) if d else 99.0))
    same = sum(1 for _, _, snr in rows if snr >= 40.0)
    print("  %d of %d sounding 5 s windows within -40 dB of the reference (each at its own offset), overall %.1f dB"
          % (same, len(rows), 10 * math.log10(sig / diff) if diff else 99.0))
    print("  windows (s: offset, dB): " + ", ".join("%d: %+d %.0f" % r for r in rows))
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["record", "replay", "compare", "all"])
    ap.add_argument("out")
    ap.add_argument("--seconds", type=int, default=40)
    ap.add_argument("--mission", type=int)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if a.cmd in ("record", "all"):
        record(out, a.seconds, a.mission)
    if a.cmd in ("replay", "all"):
        replay(out)
    if a.cmd in ("compare", "all"):
        compare_traces(os.path.join(out, "ref.0"), os.path.join(out, "nat.1"))
        compare_audio(os.path.join(out, "ref.wav"), os.path.join(out, "native.wav"))


if __name__ == "__main__":
    main()
