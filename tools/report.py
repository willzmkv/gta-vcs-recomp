#!/usr/bin/env python3
"""A zip to attach to a bug report: the build's and the game's logs, the doctor's checks, the PC. Standard library only.

    python tools/report.py [OUT.zip]        default: build/reports/report-<date>.zip

What goes in: the build logs (configure, build, recompiler), the newest four build/run/*.log (the game's output, crash reports included:
the crash handler writes to it), build/build_info.json, generated/recompile_stamp.json, build/vcs.ini and
launcher.ini (settings), tools/doctor.py --json, Windows / CPU / GPU names. What never goes in: the disc, the
executable or anything extracted or generated from it (generated/*.cpp), the memory card. Logs keep their last 2 MB.
"""
import argparse
import datetime
import glob
import json
import os
import platform
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

KEEP = 2 << 20
BUILD_LOGS = ["configure.log", "build.log", "build_recomp.log", "recomp.log", "configure_launcher.log",
              "build_launcher.log", "build_pgo_gen.log"]


def tail_bytes(path):
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - KEEP))
        data = f.read()
    return (b"[... %d bytes cut ...]\n" % (size - KEEP) if size > KEEP else b"") + data


def system_info():
    info = {"platform": platform.platform(), "machine": platform.machine(), "python": sys.version.split()[0],
            "processor": platform.processor(), "cpus": os.cpu_count()}
    if tc.IS_WINDOWS:
        ps = ("Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion | ConvertTo-Json -Compress; "
              "(Get-CimInstance Win32_Processor | Select-Object -First 1).Name")
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True,
                                 timeout=60).stdout.strip().splitlines()
            if out:
                info["gpus"] = json.loads(out[0])
                info["cpu"] = out[-1] if len(out) > 1 else ""
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
    else:
        try:
            info["gpus"] = subprocess.run(["sh", "-c", "lspci | grep -i -E 'vga|3d|display'"], capture_output=True,
                                          text=True, timeout=30).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    a = ap.parse_args()
    tc.stage("collect", "Collecting the logs")
    out = a.out or os.path.join(tc.BUILD, "reports", "report-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + ".zip")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    # the pipeline's logs (the recompiler's has counts and addresses, no code), the game's newest runs, the state files
    files = [os.path.join(tc.BUILD, n) for n in BUILD_LOGS + ["build_info.json", "vcs.ini", "launcher.ini"]]
    files.append(os.path.join(tc.ROOT, "generated", "recompile_stamp.json"))
    files += sorted(glob.glob(os.path.join(tc.BUILD, "run", "*.log")), key=os.path.getmtime, reverse=True)[:4]
    files = [p for p in files if os.path.isfile(p)]
    doctor = subprocess.run(tc.PY + [tc.tool("doctor.py"), "--json"], capture_output=True, cwd=tc.ROOT,
                            encoding="utf-8", errors="replace")  # the tools print UTF-8 (toolchain)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path in files:
            # the build folder's files under build/ in the zip, wherever it is
            name = os.path.relpath(path, tc.BUILD) if os.path.normcase(path).startswith(os.path.normcase(tc.BUILD)) else None
            z.writestr(("build/" + name if name else tc.show(path)).replace(os.sep, "/"), tail_bytes(path))
        z.writestr("doctor.json", doctor.stdout or doctor.stderr)
        z.writestr("system.json", json.dumps(system_info(), indent=1))
        z.writestr("source.json", json.dumps(tc.source_version(), indent=1))
    print("[report] %s (%d files): attach it to your bug report" % (out, len(files) + 3))
    tc.event(result="ok", path=os.path.abspath(out))


if __name__ == "__main__":
    main()
