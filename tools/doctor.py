#!/usr/bin/env python3
"""Is everything ready to build and play? The checks the launcher shows as its steps, for people too.

    python tools/doctor.py              a readable report
    python tools/doctor.py --json       the same as JSON (the launcher reads this)
    python tools/doctor.py --iso PATH   check that disc instead of the one the build would find

Checks: the folder (where the game is built: write access, synced / network folders, path length, free space), the
memory, the processor (which build it gets: AVX2, AVX or SSE4 - toolchain.cpu_level), the compiler (Windows: llvm-mingw, or that the build
downloads it - with VCS_TOOLCHAIN=msvc Visual Studio with the C++ tools and its clang-cl; Linux: build.py --check-deps), git
when the dependencies aren't bundled (deps/), the disc (tools/vcs_disc.py check), and the state of the build:
whether generated/ is current (recompile.py's stamp) and whether build/vcs.exe matches the source (build/build_info.json,
file times). "next" says what to do: disc, tools, build or play. Nothing here changes anything.
"""
import argparse
import ctypes
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

GB = 1 << 30
# A release folder after its first build: 1.13 GB (build/ 0.75, generated/ 0.20, deps/ 0.12, python/ 0.02; measured
# 2026-10-30, docs/LAUNCHER.md). 3 GB leaves room for the recompile's second copy of generated/, the compiler's
# temporary files and a PGO build.
NEED_FREE = 3 * GB
NEED_TOOLCHAIN = GB  # llvm-mingw unpacked in build/build-tools: 0.74 GB (20261020), downloaded by the first build
NEED_RAM = 8 * GB  # a unity batch of generated code takes up to ~2 GB to compile, several run at once

VS_COMPONENTS = {
    "cpp": "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "clang": "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
    "cmake": "Microsoft.VisualStudio.Component.VC.CMake.Project",
}


def existing(path):
    """`path`, or its nearest folder that exists (a build folder not made yet)."""
    path = os.path.abspath(path)
    while not os.path.exists(path) and os.path.dirname(path) != path:
        path = os.path.dirname(path)
    return path


def place_warnings(path, what):
    """What makes `path` a poor place to build: [{code, message}]. `what` names it in the messages ("the folder", "the
    build folder")."""
    warnings = []

    def warn(code, message):
        warnings.append({"code": code, "message": message})

    low = os.path.abspath(path).lower()
    for var in ("ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"):
        base = os.environ.get(var)
        if base and low.startswith(base.lower().rstrip("\\") + "\\"):
            warn("program_files", "%s is under Program Files, where programs can't write without admin rights: move it, "
                                  "e.g. to C:\\Games\\VCS" % what)
            break
    synced = [os.environ.get(v) for v in ("OneDrive", "OneDriveConsumer", "OneDriveCommercial")]
    if any(s and low.startswith(s.lower().rstrip("\\") + "\\") for s in synced) or any(
            "\\" + name + "\\" in low + "\\" for name in ("onedrive", "dropbox", "google drive", "icloud drive", "icloudrive")):
        warn("synced", "%s is synced to the cloud (OneDrive, Dropbox...): the build writes thousands of files and "
                       "syncing slows it down or locks files mid-build" % what)
    if low.startswith("\\\\"):
        warn("network", "%s is on a network share: building there is very slow. Use a local drive" % what)
    elif tc.IS_WINDOWS:
        drive = os.path.splitdrive(low)[0] + "\\"
        if ctypes.windll.kernel32.GetDriveTypeW(drive) == 4:  # DRIVE_REMOTE
            warn("network", "%s is on a network drive: building there is very slow. Use a local drive" % what)
    if tc.IS_WINDOWS and len(path) > 60:
        warn("long_path", "%s's path is long (%d characters); some build files go deep below it and Windows limits "
                          "paths to 260. A short one like D:\\VCS is safest" % (what, len(path)))
    try:
        path.encode("ascii")
    except UnicodeEncodeError:
        warn("non_ascii", "%s's path has non-English letters; some build tools don't handle them" % what)
    if not os.access(existing(path), os.W_OK):
        warn("read_only", "%s can't be written to" % what)
    return warnings


def check_folder():
    """The source folder and the build folder (tc.BUILD: build/ here, or one chosen elsewhere - tools/build_dir.py).
    The space a build needs is on the build folder's drive; generated/ (~0.2 GB) stays in the source folder."""
    root, build = tc.ROOT, tc.BUILD
    custom = os.path.normcase(build) != os.path.normcase(tc.DEFAULT_BUILD)
    warnings = place_warnings(root, "the folder")
    if custom:
        warnings += [w for w in place_warnings(build, "the build folder") if w["code"] != "long_path" or len(build) > 60]
    free = shutil.disk_usage(existing(build)).free
    built = os.path.isfile(tc.VCS_EXE)
    need = NEED_FREE + (NEED_TOOLCHAIN if tc.IS_WINDOWS and tc.windows_toolchain() == "mingw" and
                        not tc.llvm_mingw_bin(download=False) else 0)
    if free < (need // 4 if built else need):
        warnings.append({"code": "disk_space", "message": "%.1f GB free on the build folder's drive; a build needs "
                                                           "about %d GB" % (free / GB, round(need / GB))})
    if custom and shutil.disk_usage(existing(root)).free < GB // 2:
        warnings.append({"code": "disk_space", "message": "less than 0.5 GB free on the source folder's drive: the "
                                                           "recompiled code (0.2 GB) is kept there"})
    future = tc.future_files()
    if future:
        path, ahead = max(future, key=lambda f: f[1])
        warnings.append({"code": "future_files", "message": "%d %s dated after the PC's clock (up to %.1f h, %s): the "
                                                             "build sets them to now. Check the clock and time zone if "
                                                             "this keeps coming back" % (
                                                                 len(future), "file is" if len(future) == 1 else
                                                                 "files are", ahead / 3600, tc.show(path))})
    return {"path": root, "build": build, "build_custom": custom, "free_bytes": free, "need_bytes": need,
            "warnings": warnings}


def total_ram():
    if tc.IS_WINDOWS:
        class MemoryStatus(ctypes.Structure):
            _fields_ = [("length", ctypes.c_uint32), ("load", ctypes.c_uint32), ("total", ctypes.c_uint64),
                        ("avail", ctypes.c_uint64), ("page_total", ctypes.c_uint64), ("page_avail", ctypes.c_uint64),
                        ("virt_total", ctypes.c_uint64), ("virt_avail", ctypes.c_uint64), ("ext", ctypes.c_uint64)]
        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
        return status.total
    try:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except (ValueError, OSError, AttributeError):
        return 0


def check_cpu():
    """The processor and the build it gets (toolchain.cpu_level: the game is compiled for the PC that builds it). Not
    ok only for a CPU without SSE4.2 (code cpu_unsupported); an older one is ok, its build just runs slower."""
    level = tc.cpu_level()
    name = tc.cpu_name()
    if level is None:
        return {"name": name, "level": "", "ok": False, "code": "cpu_unsupported",
                "message": "%s has no SSE4.2: the game needs a processor from 2008 or later" % (name or "this processor")}
    forced = bool(os.environ.get("VCS_CPU"))
    message = "%s: built for %s" % (name or "processor", tc.CPU_TEXT[level]) + (" (VCS_CPU)" if forced else "")
    if level == "SSE4" and not forced:  # the four-lane VU helpers need AVX (ps2_vu1_compiled.h): per lane here
        message += "; the game runs slower than on a processor with AVX"
    return {"name": name, "level": level, "ok": True, "code": "ok", "message": message}


def vswhere():
    return os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Microsoft Visual Studio",
                        "Installer", "vswhere.exe")


def _vswhere_json(*args):
    try:
        out = subprocess.run([vswhere(), "-products", "*", "-prerelease", "-format", "json", "-utf8"] + list(args),
                             capture_output=True, timeout=60).stdout.decode("utf-8", "replace")
        return json.loads(out or "[]")
    except (OSError, ValueError, subprocess.SubprocessError):
        return []


def vs_installs():
    """Every Visual Studio / Build Tools install: [{path, name, version, components: {cpp, clang, cmake}}]. One query
    per component (vswhere's -path ignores -requires)."""
    if not os.path.isfile(vswhere()):
        return []
    having = {key: {os.path.normcase(i.get("installationPath", "")) for i in _vswhere_json("-requires", component)}
              for key, component in VS_COMPONENTS.items()}
    result = []
    for inst in _vswhere_json():
        path = inst.get("installationPath", "")
        result.append({"path": path, "name": inst.get("displayName", ""), "version": inst.get("installationVersion", ""),
                       "components": {key: os.path.normcase(path) in paths for key, paths in having.items()}})
    return result


def check_compiler():
    """Windows: the Visual Studio the build would use and what it lacks (code: ok, no_vs, no_cpp_tools, no_clang,
    old_clang; CMake / Ninja: found or downloaded by the build) - with VCS_TOOLCHAIN=mingw llvm-mingw instead (always
    ok: the build downloads it when it isn't there yet). Linux: build.py --check-deps."""
    if tc.IS_WINDOWS and tc.windows_toolchain() == "mingw":
        return check_llvm_mingw()
    if not tc.IS_WINDOWS:
        missing = tc.linux_missing(tc.build_env(), quiet=True)
        if not missing:
            return {"ok": True, "code": "ok", "message": "clang and every package the build needs"}
        packages = [p for _, p in missing]
        return {"ok": False, "code": "missing_packages", "missing": [label for label, _ in missing],
                "packages": packages, "command": " ".join(tc.linux_packages_command(missing)),
                "llvm_repo": tc.apt_llvm_repo(missing) or "",
                "message": "%d package%s missing: %s" % (len(missing), "" if len(missing) == 1 else "s",
                                                          ", ".join(label for label, _ in missing))}
    installs = vs_installs()
    result = {"toolchain": "msvc", "installs": installs, "installer": os.path.join(os.path.dirname(vswhere()), "setup.exe")}
    with_cpp = [i for i in installs if i["components"]["cpp"]]
    if not installs:
        return dict(result, ok=False, code="no_vs", message="Visual Studio or its Build Tools aren't installed")
    if not with_cpp:
        return dict(result, ok=False, code="no_cpp_tools", vs=installs[0],
                    message="%s is installed without the C++ tools (\"Desktop development with C++\")" % installs[0]["name"])
    # the newest with the C++ tools is the one toolchain.build_env picks (vswhere -latest -requires VC.Tools)
    vs = sorted(with_cpp, key=lambda i: [int(x) for x in i["version"].split(".") if x.isdigit()])[-1]
    result["vs"] = vs
    # the clang-cl the build takes: scoop's LLVM when installed (toolchain.build_env puts it first), else Visual Studio's
    # C++ Clang tools, else one on PATH
    front, _ = tc._scoop_paths()
    clang = (shutil.which("clang-cl", path=os.pathsep.join(front)) if front else None) or next(
        (p for p in [os.path.join(vs["path"], "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")] if os.path.isfile(p)),
        None) or shutil.which("clang-cl")
    if not clang:
        return dict(result, ok=False, code="no_clang", message="%s has no \"C++ Clang tools for Windows\"" % vs["name"])
    version = tc.clang_version(clang)
    result["clang"] = version
    result["clang_path"] = clang
    if version < tc.MIN_CLANG:
        return dict(result, ok=False, code="old_clang", message="its clang-cl is version %d; %d or newer is needed: "
                                                                "update Visual Studio" % (version, tc.MIN_CLANG))
    # CMake / Ninja never stop a build: the build downloads them when none here is new enough (toolchain.build_tools)
    tools = tc.build_tools(dict(os.environ), download=False)
    result["cmake"], result["ninja"] = tools["cmake"] or "", tools["ninja"] or ""
    missing = [title for title in ("CMake", "Ninja") if not tools[title.lower()]]
    return dict(result, ok=True, code="ok", message="%s %s, clang-cl %d%s" % (
        vs["name"], vs["version"], version, "; the build downloads " + " and ".join(missing) if missing else ""))


def check_llvm_mingw():
    """The compiler a Windows build uses (toolchain.llvm_mingw_bin): there, or downloaded by the first build. Nothing is
    fetched here."""
    bin_dir = tc.llvm_mingw_bin(download=False)
    tools = tc.build_tools(dict(os.environ), download=False)
    missing = [title for title in ("CMake", "Ninja") if not tools[title.lower()]]
    result = {"toolchain": "mingw", "cmake": tools["cmake"] or "", "ninja": tools["ninja"] or ""}
    if bin_dir:
        version = tc.clang_version(os.path.join(bin_dir, "clang.exe"))
        result.update(clang=version, clang_path=os.path.join(bin_dir, "clang.exe"))
        text = "llvm-mingw, clang %d" % version
    else:
        missing.insert(0, "llvm-mingw (clang, ~190 MB)")
        text = "nothing to install"
    return dict(result, ok=True, code="ok", download=missing,
                message=text + ("; the build downloads " + " and ".join(missing) if missing else ""))


def check_downloads():
    bundled = os.path.isdir(tc.DEPS_DIR)
    git = shutil.which("git")
    ok = bundled or bool(git)
    return {"bundled": bundled, "git": git or "", "ok": ok,
            "message": "dependencies bundled (deps/)" if bundled else "git found: the first build downloads the "
                       "dependencies" if git else "git not found: the first build needs it to download the dependencies "
                       "(https://git-scm.com), or use a release, which bundles them"}


def check_disc(given):
    import vcs_disc
    try:
        iso = tc.find_iso(given) if given else _quiet_find_iso()
    except SystemExit:
        iso = given
    if not iso:
        return {"ok": False, "code": "no_disc", "path": "", "message": "no disc image chosen"}
    return dict(vcs_disc.check(iso), path=os.path.abspath(iso))


def _quiet_find_iso():
    try:
        return tc.find_iso(None)
    except SystemExit:
        return None


# what the game's build depends on, besides generated/ (recompile.py's stamp covers that): newer files here than
# build/vcs.exe mean a build is due. Only these folders, so the check stays fast.
BUILD_INPUTS = ["src", "third_party/PS2Recomp/ps2xRuntime", "third_party/PS2Recomp/ps2xIOP", "CMakeLists.txt"]


def newest_input():
    newest, which = 0.0, ""
    for rel in BUILD_INPUTS:
        path = os.path.join(tc.ROOT, rel)
        if os.path.isfile(path):
            entries = [path]
        else:
            entries = (os.path.join(d, f) for d, _, names in os.walk(path) for f in names)
        for f in entries:
            try:
                t = os.path.getmtime(f)
            except OSError:
                continue
            if t > newest:
                newest, which = t, f
    return newest, which


def check_build(cpu_level=None):
    import recompile
    source = tc.source_version()
    why_recompile = recompile.why_recompile()
    exe = tc.VCS_EXE
    info = None
    try:
        with open(os.path.join(tc.BUILD, "build_info.json"), encoding="utf-8") as f:
            info = json.load(f)
    except (OSError, ValueError):
        pass
    reasons = []
    if not os.path.isfile(exe):
        reasons.append("the game hasn't been built yet")
    else:
        if why_recompile:
            reasons.append(why_recompile)
        if not info:
            reasons.append("no build/build_info.json (built before the launcher existed)")
        elif info.get("source") != source:
            reasons.append("the source changed since the last build")
        else:
            stamp = recompile.read_stamp() or {}
            if info.get("generated_inputs") != stamp.get("inputs", ""):
                reasons.append("the generated code changed since the last build")
        # built before build_info said (AVX2 then), or on / for another PC: a game for a newer CPU stops at once
        built_for = (info or {}).get("cpu") or "AVX2"
        if cpu_level and built_for != cpu_level:
            reasons.append("the game was built for %s processors; this PC's is %s" % (
                tc.CPU_TEXT.get(built_for, built_for), tc.CPU_TEXT.get(cpu_level, cpu_level)))
        newest, which = newest_input()
        if newest > os.path.getmtime(exe) + 2:
            reasons.append("newer source files than the game (%s)" % tc.show(which))
    return {"exe": tc.show(exe), "built": os.path.isfile(exe), "info": info, "source": source,
            "recompile": why_recompile or "", "current": not reasons, "reasons": reasons}


def run(given_iso):
    report = {"root": tc.ROOT, "python": {"version": sys.version.split()[0], "path": sys.executable,
                                          "bundled": os.path.normcase(sys.executable).startswith(
                                              os.path.normcase(os.path.join(tc.ROOT, "python")))}}
    report["folder"] = check_folder()
    ram = total_ram()
    report["memory"] = {"bytes": ram, "ok": ram == 0 or ram >= NEED_RAM - GB // 2,
                        "message": "%.0f GB" % (ram / GB) if ram >= NEED_RAM - GB // 2 else
                        "%.0f GB of memory: the build may run out; close other programs while it runs" % (ram / GB)}
    report["cpu"] = check_cpu()
    report["compiler"] = check_compiler()
    report["downloads"] = check_downloads()
    report["disc"] = check_disc(given_iso)
    report["build"] = check_build(report["cpu"]["level"] or None)
    if not report["cpu"]["ok"]:
        nxt = "cpu"
    elif not report["disc"]["ok"]:
        nxt = "disc"
    elif not report["build"]["current"] and (not report["compiler"]["ok"] or not report["downloads"]["ok"]):
        nxt = "tools"
    elif not report["build"]["current"]:
        nxt = "build"
    else:
        nxt = "play"
    report["next"] = nxt
    return report


def show(report):
    def line(ok, title, message):
        print("  [%s] %-10s %s" % ("ok" if ok else "!!" if ok is False else "--", title, message))

    print("doctor: " + report["root"])
    folder = report["folder"]
    line(not folder["warnings"], "folder", "%.1f GB free" % (folder["free_bytes"] / GB))
    for w in folder["warnings"]:
        print("               - " + w["message"])
    line(report["memory"]["ok"], "memory", report["memory"]["message"])
    line(report["cpu"]["ok"], "processor", report["cpu"]["message"])
    line(report["compiler"]["ok"], "compiler", report["compiler"]["message"])
    line(report["downloads"]["ok"], "downloads", report["downloads"]["message"])
    disc = report["disc"]
    line(disc["ok"], "disc", disc["message"] + (" (%s)" % disc["path"] if disc.get("path") else ""))
    build = report["build"]
    line(build["current"], "game", "built and current (%s)" % (build["info"] or {}).get("opt", "?") if build["current"]
         else "; ".join(build["reasons"]))
    advice = {"cpu": "this processor can't run the game",
              "disc": "choose your disc: python tools/build.py --recompile \"path\\to\\your.iso\" (or the launcher)",
              "tools": "install what's missing above" + (" (the launcher can install Visual Studio's Build Tools: "
                                                         "python tools/setup_vs.py)" if tc.IS_WINDOWS and tc.windows_toolchain() == "msvc" else ""),
              "build": "python tools/build.py --recompile-if-needed",
              "play": "start %s (or the launcher)" % tc.show(tc.VCS_EXE)}
    print("next: " + advice[report["next"]])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", action="store_true", help="print the report as JSON")
    ap.add_argument("--iso", help="the disc image to check (default: the one the build would use)")
    a = ap.parse_args()
    report = run(a.iso)
    if a.json:
        print(json.dumps(report))
    else:
        show(report)


if __name__ == "__main__":
    main()
