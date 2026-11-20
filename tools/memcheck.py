#!/usr/bin/env python3
"""Memory check (GitHub issue 115): the game's memory over a route that streams a lot must level off.

    python tools/memcheck.py [OUT] [--minutes 15] [--leg 30] [--recordings-mb 64] [--max-rise 8] [--port N]
                             [--name run] [ENV=VAL ...]

From a copy of the Fort Baxter card, hidden, as players run it (60 fps, frame-rate independent timing): an invincible
player in an AI-driven car, moved to the next of 16 spots across the map every --leg seconds - every move streams models
in at new addresses, which is what made the RSL recordings grow without end (2.3 GB an hour on this route; PERFORMANCE.md
31). Every 10 s a row in OUT/<name>.csv: private bytes, working set, "memory" (the private working set: Task Manager's
Memory column; RssAnon on Linux), the recordings (count, MB held, dropped so far, from the game's [rsl-world] log lines),
game frames, the player's position.

The verdict: the trend of "memory" over the second half of the run, least squares, in MB a minute. Above --max-rise it
fails (exit 1) - the unbounded recordings rose by 40 here, the game without recordings by less than 2. Blocks the worker
had to read from guest RAM because their recording was gone fail it too (dropping a recording must first take back its
confirmation: src/game/vcs_rsl_world.cpp, trimRecordings).

--recordings-mb sets VCS_RSL_RECORDINGS_MB for the run: 64 by default, so the limit is reached within two minutes and
most of the run shows the plateau; -1 leaves the game's own default (256), 0 is no limit (the leak, for comparison).
ENV=VAL arguments go to the game (VCS_RSL_WORLD=0 VCS_RSL_MODELS=0: no recordings at all). OUT defaults to
build/memcheck/<date-time>; two checks side by side need their own --port and --name.
"""
import argparse
import csv
import ctypes
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = rig.ROOT
SPOTS = [(-600.0, 600.0), (325.0, 425.0), (75.0, 175.0), (-1175.0, -325.0), (575.0, -75.0), (-1425.0, -825.0),
         (575.0, 175.0), (-675.0, 1425.0), (1325.0, -325.0), (-1425.0, -575.0), (-925.0, 175.0), (325.0, -825.0),
         (825.0, 925.0), (-175.0, -1325.0), (1075.0, 425.0), (-1675.0, 325.0)]
MB = float(2 ** 20)


# ---- the game's process and its memory ------------------------------------------------------------------------------
def _children_windows(parent):
    import ctypes.wintypes as wt

    class Entry(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                    ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD),
                    ("szExeFile", wt.WCHAR * 260)]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = k32.CreateToolhelp32Snapshot(2, 0)  # TH32CS_SNAPPROCESS
    e = Entry()
    e.dwSize = ctypes.sizeof(e)
    found = []
    ok = k32.Process32FirstW(wt.HANDLE(snap), ctypes.byref(e))
    while ok:
        if e.th32ParentProcessID == parent:
            found.append((e.th32ProcessID, e.szExeFile.lower()))
        ok = k32.Process32NextW(wt.HANDLE(snap), ctypes.byref(e))
    k32.CloseHandle(wt.HANDLE(snap))
    return found


def _children_linux(parent):
    found = []
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % name) as f:
                stat = f.read()
        except OSError:
            continue
        comm = stat[stat.find("(") + 1:stat.rfind(")")]
        if int(stat[stat.rfind(")") + 2:].split()[1]) == parent:
            found.append((int(name), comm.lower()))
    return found


def game_pid(runner_pid):
    """vcs(.exe) started by run.py (the rig's process)."""
    for pid, name in (_children_windows if os.name == "nt" else _children_linux)(runner_pid):
        if name in ("vcs.exe", "vcs"):
            return pid
    return None


def _memory_windows(pid):
    import ctypes.wintypes as wt

    class Counters(ctypes.Structure):  # PROCESS_MEMORY_COUNTERS_EX
        _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t)]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    c = Counters()
    c.cb = ctypes.sizeof(c)
    ok = psapi.GetProcessMemoryInfo(wt.HANDLE(h), ctypes.byref(c), c.cb)
    k32.CloseHandle(wt.HANDLE(h))
    if not ok:
        return None
    # the private working set (Task Manager's Memory column): SYSTEM_PROCESS_INFORMATION.WorkingSetPrivateSize, the
    # 64-bit layout (offset 8; UniqueProcessId at 0x50; NextEntryOffset first)
    private_ws = None
    if ctypes.sizeof(ctypes.c_void_p) == 8:
        ntdll = ctypes.WinDLL("ntdll")
        size = wt.ULONG(1 << 20)
        while True:
            buf = ctypes.create_string_buffer(size.value)
            need = wt.ULONG(0)
            if ntdll.NtQuerySystemInformation(5, buf, size, ctypes.byref(need)) == 0:
                break
            size = wt.ULONG(max(need.value, size.value * 2))
        raw, off = buf.raw, 0
        while True:
            nxt = int.from_bytes(raw[off:off + 4], "little")
            if int.from_bytes(raw[off + 0x50:off + 0x58], "little") == pid:
                private_ws = int.from_bytes(raw[off + 8:off + 16], "little")
                break
            if not nxt:
                break
            off += nxt
    return c.PrivateUsage / MB, c.WorkingSetSize / MB, (private_ws / MB if private_ws is not None else None)


def _memory_linux(pid):
    v = {}
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                k, _, rest = line.partition(":")
                if k in ("VmData", "VmRSS", "RssAnon"):
                    v[k] = int(rest.split()[0]) / 1024.0  # kB
    except OSError:
        return None
    return v.get("VmData"), v.get("VmRSS"), v.get("RssAnon")


def memory(pid):
    """(private bytes, working set, private working set) in MB; None when the process is gone."""
    return (_memory_windows if os.name == "nt" else _memory_linux)(pid)


# ---- the game's own word (VCS_RSL_WORLD_LOG) ------------------------------------------------------------------------
class LogReader:
    """What was appended to the log since the last look: the newest recordings figures, guest-RAM replays summed."""

    def __init__(self, path):
        self.path, self.pos = path, 0
        self.recordings = self.held_mb = self.limit_mb = self.dropped = self.again = None
        self.from_guest_ram = 0
        self.skipped = 0

    def read(self):
        try:
            with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(self.pos)
                text = f.read()
                self.pos = f.tell()
        except OSError:
            return
        for m in re.finditer(r"(\d+) recordings\n", text):
            self.recordings = int(m.group(1))
        for m in re.finditer(r"recordings hold ([\d.]+) MB \(limit (\d+) MB\); dropped (\d+)", text):
            self.held_mb, self.limit_mb, self.dropped = float(m.group(1)), int(m.group(2)), int(m.group(3))
        for m in re.finditer(r"(\d+) of them recorded again", text):
            self.again = int(m.group(1))
        for m in re.finditer(r"skipped \(not copied\): (\d+) blocks, \d+ replayed, (\d+) from guest RAM", text):
            self.skipped += int(m.group(1))
            self.from_guest_ram += int(m.group(2))


def trend(points):
    """Least squares slope of (x, y) points; None for too few."""
    if len(points) < 4:
        return None
    n = len(points)
    mx, my = sum(p[0] for p in points) / n, sum(p[1] for p in points) / n
    den = sum((p[0] - mx) ** 2 for p in points)
    return sum((p[0] - mx) * (p[1] - my) for p in points) / den if den else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--minutes", type=float, default=15.0)
    ap.add_argument("--leg", type=float, default=30.0, help="seconds per spot")
    ap.add_argument("--recordings-mb", type=int, default=64, help="VCS_RSL_RECORDINGS_MB for the run (-1: the game's default)")
    ap.add_argument("--max-rise", type=float, default=8.0, help="MB a minute over the second half that still passes")
    ap.add_argument("--port", type=int, default=47670)
    ap.add_argument("--name", default="run")
    ap.add_argument("env", nargs="*", help="ENV=VAL for the game")
    a, rest = ap.parse_known_args()
    # (OUT and ENV=VAL are both positional: the first without '=' is OUT)
    words = ([a.out] if a.out else []) + a.env + rest
    out = next((w for w in words if "=" not in w), None)
    out = os.path.abspath(out or os.path.join(ROOT, "build", "memcheck", time.strftime("%Y%m%d-%H%M%S")))
    env = {"VCS_TIMING": "1", "VCS_FPS": "60", "VCS_RSL_WORLD_LOG": "1"}
    if a.recordings_mb >= 0:
        env["VCS_RSL_RECORDINGS_MB"] = str(a.recordings_mb)
    env.update(dict(w.split("=", 1) for w in words if "=" in w))
    os.makedirs(out, exist_ok=True)
    g = rig.Game.start(save=rig.default_save(), name="memcheck_" + a.name, seconds=a.minutes * 60 + 600,
                       data_dir=os.path.join(out, "data_" + a.name), port=a.port, env=env)
    pid = game_pid(g.proc.pid)
    if pid is None:
        print("[memcheck] the game's process was not found under run.py: no memory figures", flush=True)
    log = LogReader(g.log_path)
    rows, exited = [], False
    f = open(os.path.join(out, a.name + ".csv"), "w", newline="")
    w = csv.writer(f)
    w.writerow(["t_s", "private_mb", "working_mb", "memory_mb", "recordings", "recordings_mb", "dropped", "frames",
                "x", "y", "leg"])
    t0 = time.time()
    try:
        g.wait_ingame()
        g.invincible(True)
        g.prime()
        leg, next_leg = -1, 0.0
        while time.time() - t0 < a.minutes * 60:
            now = time.time() - t0
            if not g.alive():
                exited = True
                print("[memcheck] the game exited", flush=True)
                break
            if now >= next_leg:
                leg += 1
                x, y = SPOTS[leg % len(SPOTS)]
                try:
                    g.teleport(x, y, -100.0)
                    time.sleep(2)
                    g.invincible(True)
                    g.autodrive(speed=40.0, release=True)
                except Exception as e:
                    print("[memcheck] leg %d: %s" % (leg, str(e)[:100]), flush=True)
                next_leg = now + a.leg
            try:
                frames = int(g.cmd("frames"))
                _, (px, py, _), _ = g.player()
            except Exception:
                frames, px, py = None, 0.0, 0.0
            m = memory(pid) if pid else None
            log.read()
            r1 = lambda v: round(v, 1) if v is not None else None  # noqa: E731
            row = [round(now)] + [r1(v) for v in (m or (None, None, None))] + [log.recordings, log.held_mb, log.dropped,
                                                                              frames, round(px), round(py), leg]
            w.writerow(row)
            f.flush()
            rows.append(row)
            if len(rows) % 6 == 1:
                print("[memcheck] %5d s: memory %s MB (private %s), %s recordings holding %s MB, %s dropped" %
                      (row[0], row[3], row[1], row[4], row[5], row[6]), flush=True)
            time.sleep(max(0.0, 10.0 - (time.time() - t0 - now)))
    finally:
        g.quit()
        f.close()
    time.sleep(1.0)
    log.read()
    col = 3 if any(r[3] is not None for r in rows) else 1  # the private working set, else private bytes
    half = rows[-1][0] / 2.0 if rows else 0.0
    rise = trend([(r[0] / 60.0, r[col]) for r in rows if r[col] is not None and r[0] >= half])
    mem = [r[col] for r in rows if r[col] is not None]
    reasons = []
    if exited:
        reasons.append("the game exited")
    if rise is None:
        reasons.append("too few memory samples" if pid else "the game's process was not found")
    elif rise > a.max_rise:
        reasons.append("memory rises by %.1f MB a minute over the second half (limit %.1f)" % (rise, a.max_rise))
    if log.from_guest_ram:
        reasons.append("%d skipped blocks had no recording (read from guest RAM)" % log.from_guest_ram)
    summary = {"minutes": round((time.time() - t0) / 60.0, 1), "samples": len(rows), "env": env,
               "memory_column": "private working set" if col == 3 else "private bytes",
               "memory_mb_first": mem[0] if mem else None, "memory_mb_last": mem[-1] if mem else None,
               "memory_mb_max": max(mem) if mem else None,
               "rise_mb_per_min_second_half": round(rise, 2) if rise is not None else None,
               "recordings": log.recordings, "recordings_mb": log.held_mb, "recordings_limit_mb": log.limit_mb,
               "recordings_dropped": log.dropped, "dropped_recorded_again": log.again, "blocks_skipped": log.skipped,
               "skipped_from_guest_ram": log.from_guest_ram, "ok": not reasons, "failed": reasons}
    with open(os.path.join(out, a.name + ".json"), "w") as jf:
        json.dump(summary, jf, indent=1)
    print(json.dumps(summary, indent=1))
    print("[memcheck] %s (%s)" % ("ok" if not reasons else "FAILED: " + "; ".join(reasons), os.path.relpath(out, ROOT)))
    return 0 if not reasons else 1


if __name__ == "__main__":
    sys.exit(main())
