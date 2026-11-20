#!/usr/bin/env python3
"""Build the game (configures CMake the first time).

    python tools/build.py                       build vcs (build/vcs[.exe], with its DLLs and vcs.ini)
    python tools/build.py --recompile [ISO]     recompile from your disc first, then build - the whole pipeline
    python tools/build.py --recompile-if-needed [ISO]   the same, recompiling only when generated/ is not current
                                                (generated/recompile_stamp.json; what the launcher runs)
    python tools/build.py --launcher            build the launcher (VCSLauncher(.exe) in the repository root; no disc)
    python tools/build.py --install-deps        Linux: install the missing packages (the desktop's password prompt)
    python tools/build.py --progress json       also print `@vcs {...}` events for the launcher (toolchain.py)
    python tools/build.py --quick               no link-time optimisation: fast rebuilds while changing code
    python tools/build.py --iop-tests           build and run the IOP / SPU2 unit tests (build/iop_tests, no disc)
    python tools/build.py --check-deps          Linux: list the missing packages and the command that installs them
    python tools/build.py --opt none|lto|pgo    optimisation of the game, kept until changed: lto (default; ThinLTO,
                                                the game to play), none (= --quick), pgo (ThinLTO + profile-guided:
                                                trains first when build/pgo/vcs.profdata is missing or with --train;
                                                training needs the rig's card copies, build/rig/saves)

The optimisation is kept in the CMake cache: after --quick every build stays quick until `--opt lto`. Switching
recompiles the whole game (~6-10 min). The last line of each build says which one was built.

Progress: every build step shows `[build] <target> done/total %, elapsed, ~time left` (Ninja's step count; redrawn in
place on a terminal, a line per 5% / minute otherwise); CMake configure and the recompiler show their elapsed time.
With link-time optimisation the game's link is a step of its own (minutes: the whole game is optimised there), timed
against the last one (link_times.json in the build folder; docs/LAUNCHER.md).

PGO training: an instrumented build in build/pgo-gen plays tools/bench.py (city spots, an AI drive: from the rig's Fort
Baxter card copy, or a new game when there is none) and the reference scenes' boot / flyover / cutscene
(tools/scenes.py); the profiles go to build/pgo/vcs.profdata. A profile
from older code still works: the functions that changed are optimised as without one.

ISO defaults to VCS_ISO, iso=... in build/vcs.ini or vcs.ini, or a '*Vice City Stories*.iso' in the repository root.
A successful game build writes build/build_info.json (source version, optimisation, disc) - the launcher compares it
with the source to know whether the game needs building.
Windows: builds with llvm-mingw (clang, lld, libc++ and the Windows headers), downloaded once into build/build-tools -
nothing to install. VCS_TOOLCHAIN=msvc builds with Visual Studio 2022+ instead ("Desktop development with C++" and "C++
Clang tools for Windows"; its environment is set up automatically).
CMake 3.21+ and Ninja 1.10+: the first that runs on PATH or wherever installers put them (Visual Studio's, Kitware's,
winget, scoop, Chocolatey, pip, CLion...), else downloaded once into build/build-tools (toolchain.build_tools);
VCS_CMAKE / VCS_NINJA=<path> choose them.
Linux: clang 19+ with lld, glslang and the development packages SDL3 and FFmpeg need (docs/LINUX.md); --check-deps
lists what is missing. The first configure checks them too.
"""
import argparse
import filecmp
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recompile", nargs="?", const="", metavar="ISO",
                    help="run tools/recompile.py first (disc image optional, see above)")
    ap.add_argument("--target", default="ps2EntryRunner", help="CMake target (default: the game, ps2EntryRunner)")
    ap.add_argument("--reconfigure", action="store_true", help="run CMake configure again")
    ap.add_argument("-v", "--verbose", action="store_true", help="show all build output")
    ap.add_argument("--iop-tests", action="store_true", help="build and run the ps2xIOP unit tests (SPU2) instead")
    ap.add_argument("--opt", choices=("none", "lto", "pgo"), help="optimisation of the game (kept until changed)")
    ap.add_argument("--quick", action="store_true", help="same as --opt none: fast rebuilds, slower game")
    ap.add_argument("--train", action="store_true", help="with --opt pgo: train a new profile first")
    ap.add_argument("--check-deps", action="store_true", help="Linux: check the build dependencies and stop")
    ap.add_argument("--install-deps", action="store_true",
                    help="Linux: install the missing packages (pkexec: the desktop's password prompt) and stop")
    ap.add_argument("--recompile-if-needed", nargs="?", const="", metavar="ISO",
                    help="recompile first only when the generated code is not current")
    ap.add_argument("--launcher", action="store_true", help="build the launcher (VCSLauncher.exe) instead of the game")
    ap.add_argument("--progress", choices=("text", "json"), default="text", help="json: events for the launcher too")
    a = ap.parse_args()
    if a.progress == "json":
        tc.set_json_progress()
    if a.quick:
        if a.opt not in (None, "none"):
            ap.error("--quick is --opt none; it can't be combined with --opt " + a.opt)
        a.opt = "none"
    if a.recompile is not None and a.recompile_if_needed is not None:
        ap.error("--recompile and --recompile-if-needed: one or the other")

    env = tc.build_env()
    if a.check_deps:
        if tc.IS_WINDOWS:
            sys.exit(0)
        game = tc.check_linux_deps(env)
        launcher = tc.check_linux_deps(env, tc.LAUNCHER_LINUX_DEPS)
        sys.exit(0 if game and launcher else 1)
    if a.install_deps:
        sys.exit(install_deps(env))
    tc.fix_future_files()  # else Ninja re-runs CMake until it gives up (a release zip from another time zone)
    if a.launcher:  # its own packages, checked in build_launcher - not the game's (CI builds it on a bare system)
        sys.exit(0 if build_launcher(env, a.verbose) else 1)
    if not tc.IS_WINDOWS and not os.path.isfile(os.path.join(tc.BUILD_DIR, "build.ninja")) and not tc.check_linux_deps(env):
        tc.fail("missing build dependencies (above)")
    if a.iop_tests:
        sys.exit(iop_tests(env))
    import recompile
    iso_arg = a.recompile if a.recompile is not None else a.recompile_if_needed
    if a.recompile_if_needed is not None:
        why = recompile.why_recompile()
        if why:
            print("[build] recompiling: " + why)
            recompile.recompile(a.recompile_if_needed or None, env, a.verbose)
        else:
            print("[build] generated code is current: no recompile")
            tc.find_iso(a.recompile_if_needed or None)  # the disc is still checked (and remembered below)
    elif a.recompile is not None:
        recompile.recompile(a.recompile or None, env, a.verbose)
    elif not os.path.isdir(os.path.join(tc.ROOT, "generated", "ee")):
        print("[build] no generated code yet: run with --recompile (only the tools will be built)")

    defines = None
    if a.opt == "none":
        defines = {"PS2X_LTO": "OFF", "PS2X_PGO": ""}
    elif a.opt == "lto":
        defines = {"PS2X_LTO": "THIN", "PS2X_PGO": ""}
    elif a.opt == "pgo":
        if a.train or not os.path.isfile(PROFDATA):
            train(env, a.verbose)
        defines = {"PS2X_LTO": "THIN", "PS2X_PGO": PROFDATA.replace(os.sep, "/")}
    tc.configure(env, force=a.reconfigure or configured_without_game(), defines=defines)
    print(f"[build] building {a.target}")
    tc.stage("compile", "Compiling the game" if a.target == "ps2EntryRunner" else "Building " + a.target)
    if not tc.cmake_build(env, a.target, "build.log", a.verbose, link=game_link_stage() if a.target == "ps2EntryRunner" else None):
        sys.exit(1)
    if a.target == "ps2EntryRunner":
        if not os.path.isfile(tc.VCS_EXE):
            tc.fail("build finished but " + tc.show(tc.VCS_EXE) + " is missing", "build_failed")
        move_from_old_place()
        iso = remember_iso(iso_arg or None)
        write_build_info(iso)
        print("[build] ok: " + tc.show(tc.VCS_EXE) + " (" + built_opt() + ")")
        tc.event(result="ok", exe=tc.show(tc.VCS_EXE).replace(os.sep, "/"), opt=built_opt())


BUILD_INFO = os.path.join(tc.BUILD, "build_info.json")


def write_build_info(iso):
    """build/build_info.json: what the game in build/ was built from. The launcher compares it with the source
    (tools/doctor.py) to tell "Play" from "Update the build"."""
    import recompile
    stamp = recompile.read_stamp() or {}
    info = {"source": tc.source_version(), "generated_inputs": stamp.get("inputs", ""), "opt": built_opt(),
            "cpu": tc.cached_value(tc.BUILD_DIR, "PS2X_CPU") or "AVX2", "iso": iso or "", "time": int(time.time())}
    with open(BUILD_INFO, "w", encoding="utf-8") as f:
        json.dump(info, f, indent=1)


LAUNCHER_BUILD = os.path.join(tc.BUILD, "launcher")


def build_launcher(env, verbose):
    """launcher/ is a CMake project of its own (no generated code, no disc): build/launcher, the exe in the
    repository root."""
    tc.stage("launcher", "Building the launcher")
    if not tc.IS_WINDOWS and not tc.check_linux_deps(env, tc.LAUNCHER_LINUX_DEPS):
        tc.fail("missing packages for the launcher (above)", "missing_packages")
    if tc.drop_foreign_cache(LAUNCHER_BUILD, os.path.join(tc.ROOT, "launcher")) or tc.stale_tools(LAUNCHER_BUILD) or \
            tc.drop_other_compiler(LAUNCHER_BUILD, env) is not None or tc.python_changed(LAUNCHER_BUILD) or \
            not os.path.isfile(os.path.join(LAUNCHER_BUILD, "build.ninja")):
        cmd = ["cmake", "-S", os.path.join(tc.ROOT, "launcher"), "-B", LAUNCHER_BUILD, "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release"] + tc.launcher_compiler_defines(env) + tc.deps_defines() + \
              tc.python_defines()
        if os.environ.get("VCS_WAYLAND_PROTOCOLS_DIR"):  # a release built on an older distribution (CI)
            cmd.append("-DVCS_WAYLAND_PROTOCOLS_DIR=" + os.environ["VCS_WAYLAND_PROTOCOLS_DIR"])
        log = os.path.join(tc.BUILD, "configure_launcher.log")
        if tc.run_with_progress(cmd, log, "[build] configure launcher", env=env) != 0:
            print(tc.tail(log, 30))
            tc.fail("CMake configure of the launcher failed (build/configure_launcher.log)", tc.configure_failure(log), log)
    if not tc.cmake_build(env, "VCSLauncher", "build_launcher.log", verbose, build_dir=LAUNCHER_BUILD):
        return False
    print("[build] ok: " + tc.exe("VCSLauncher"))
    tc.event(result="ok", exe=tc.exe("VCSLauncher"))
    return True


def install_deps(env):
    """Linux: the packages the game's build (and the launcher's) lacks, installed through pkexec - the desktop's own
    password prompt, so the launcher can do it without a terminal. Without a polkit agent (or pkexec) it fails with
    no_polkit and the command to run in a terminal; a refused prompt is elevation_denied."""
    if tc.IS_WINDOWS:
        tc.fail("--install-deps is for Linux (Windows: tools/setup_vs.py install)", "unsupported")
    tc.stage("packages", "Installing the packages the build needs")
    missing = tc.linux_missing(env, quiet=True) + tc.linux_missing(env, tc.LAUNCHER_LINUX_DEPS)
    if not missing:
        print("[deps] nothing to install: every build dependency is there")
        tc.event(result="ok", installed=[])
        return 0
    packages = [p for _, p in missing]
    for label, pkgs in missing:
        print("[deps] missing: %s (%s)" % (label, pkgs))
    terminal = " ".join(tc.linux_packages_command(missing))
    if not shutil.which("pkexec"):
        tc.fail("pkexec isn't installed: run this in a terminal, then check again:\n    " + terminal, "no_polkit")
    cmd = tc.linux_packages_command(missing, elevate="pkexec")
    if "--llvm-repo" in cmd:
        print("[deps] this release's clang is too old for the game: LLVM's repository (apt.llvm.org) is added first")
    print("[deps] " + " ".join(cmd))
    code = subprocess.call(cmd)
    if code == 126:  # the prompt was dismissed
        tc.fail("the password prompt was cancelled; nothing was installed", "elevation_denied")
    if code == 127:  # no authentication agent, or not authorised
        tc.fail("no password prompt could be shown (no polkit agent): run this in a terminal, then check again:\n    " +
                terminal, "no_polkit")
    if code != 0:
        tc.fail("the package manager stopped with exit code %d (above)" % code, "install_failed")
    still = tc.linux_missing(env, quiet=True) + tc.linux_missing(env, tc.LAUNCHER_LINUX_DEPS)
    if still:
        tc.fail("still missing after the install: " + ", ".join(label for label, _ in still), "install_failed")
    print("[deps] installed: " + " ".join(packages))
    tc.event(result="ok", installed=packages)
    return 0


def configured_without_game():
    """True when build/release was configured before the generated code existed. A first --recompile does that
    (recompile.py configures to build the recompiler), and CMake then leaves the game out (VCS_HAVE_GENERATED in
    CMakeLists.txt) until a configure sees generated/ee - without one the build stops at "unknown target
    'ps2EntryRunner'"."""
    if not os.path.isfile(os.path.join(tc.ROOT, "generated", "ee", "register_functions.cpp")):
        return False
    try:
        with open(os.path.join(tc.BUILD_DIR, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            return any(line.strip() == "PS2X_BUILD_RUNTIME:BOOL=OFF" for line in f)
    except OSError:
        return False


def move_from_old_place():
    """vcs.exe used to be built in build/release/third_party/PS2Recomp/ps2xRuntime; it is now at the top of build/.
    The player's vcs.ini moves with it (when build/ has none yet), and the old exe goes, so nobody starts a stale one."""
    new_dir = os.path.dirname(tc.game_exe(tc.BUILD_DIR))
    if os.path.normcase(new_dir) == os.path.normcase(tc.OLD_GAME_DIR):
        return
    old_ini, new_ini = os.path.join(tc.OLD_GAME_DIR, "vcs.ini"), os.path.join(new_dir, "vcs.ini")
    if os.path.isfile(old_ini) and not os.path.isfile(new_ini):
        shutil.copy2(old_ini, new_ini)
        print("[build] settings moved: " + tc.show(old_ini) + " -> " + tc.show(new_ini))
    for name in ("vcs.exe", "vcs.pdb", "vcs", "vcs.ini"):
        path = os.path.join(tc.OLD_GAME_DIR, name)
        # the old vcs.ini only once build/'s is the same file (a different one stays for the player to look at)
        if os.path.isfile(path) and (name != "vcs.ini" or (os.path.isfile(new_ini) and filecmp.cmp(path, new_ini, shallow=False))):
            try:
                os.remove(path)
                print("[build] removed the old " + tc.show(path) + " (the game is now in build/)")
            except OSError as e:
                print("[build] could not remove the old " + tc.show(path) + ": " + str(e))


def remember_iso(given):
    """Writes iso=<disc> into the vcs.ini next to vcs.exe when it has none, so the exe started on its own (a double
    click) finds the disc the build used. An iso= already there is the player's and stays. Returns the disc (None when
    none is known)."""
    try:
        iso = tc.find_iso(given)
    except SystemExit:
        return None  # no disc known here: vcs.exe looks next to itself, or is given one
    ini = os.path.join(os.path.dirname(tc.VCS_EXE), "vcs.ini")
    lines = []
    if os.path.isfile(ini):
        with open(ini, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    if any(line.strip().lower().startswith("iso=") for line in lines):
        return iso
    with open(ini, "w", encoding="utf-8") as f:
        f.write("\n".join(lines + ["iso=" + iso]) + "\n")
    print("[build] disc for vcs.exe: " + iso + " (iso= in " + tc.show(ini) + ")")
    return iso


def game_link_stage():
    """(kind, title) of the game's link when it is a step of its own (toolchain.cmake_build): with link-time
    optimisation it is the longest step of a build. A quick build's link takes seconds: no stage."""
    lto = tc.cached_value(tc.BUILD_DIR, "PS2X_LTO") or "OFF"
    if lto.upper() not in ("THIN", "FULL"):
        return None
    pgo = tc.cached_value(tc.BUILD_DIR, "PS2X_PGO") or ""
    if pgo and pgo != "GEN":
        return "pgo", "Linking and optimising the game (ThinLTO + profile)"
    return lto.lower(), "Linking and optimising the game (%s)" % ("ThinLTO" if lto.upper() == "THIN" else "LTO")


def built_opt():
    """The optimisation the CMake cache of build/release holds, for the last line of a build."""
    cache = {}
    try:
        with open(os.path.join(tc.BUILD_DIR, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            for line in f:
                key, sep, value = line.partition("=")
                if sep and key.split(":")[0] in ("PS2X_LTO", "PS2X_PGO"):
                    cache[key.split(":")[0]] = value.strip()
    except OSError:
        return "optimisation unknown"
    cpu = tc.cached_value(tc.BUILD_DIR, "PS2X_CPU") or "AVX2"
    cpu = "" if cpu == "AVX2" else ", for %s CPUs" % cpu
    if cache.get("PS2X_PGO") and cache["PS2X_PGO"] != "GEN":
        return "optimised: ThinLTO + profile-guided" + cpu
    if cache.get("PS2X_LTO") == "THIN":
        return "optimised: ThinLTO" + cpu
    if cache.get("PS2X_LTO") == "FULL":
        return "optimised: full LTO" + cpu
    return "quick build, not optimised - python tools/build.py --opt lto for the game to play" + cpu


PGO_DIR = os.path.join(tc.BUILD, "pgo")
PROFDATA = os.path.join(PGO_DIR, "vcs.profdata")
PGO_BUILD = os.path.join(tc.BUILD, "pgo-gen")


def train(env, verbose):
    """Instrumented build (build/pgo-gen), training runs, build/pgo/vcs.profdata."""
    import glob
    import shutil
    tc.configure(env, build_dir=PGO_BUILD, defines={"PS2X_LTO": "OFF", "PS2X_PGO": "GEN"})
    print("[build] building the instrumented game (build/pgo-gen)")
    if not tc.cmake_build(env, "ps2EntryRunner", "build_pgo_gen.log", verbose, build_dir=PGO_BUILD):
        sys.exit(1)
    raw = os.path.join(PGO_DIR, "raw")
    shutil.rmtree(raw, ignore_errors=True)
    os.makedirs(raw)
    run_env = dict(os.environ)
    run_env["VCS_EXE"] = tc.game_exe(PGO_BUILD)
    run_env["LLVM_PROFILE_FILE"] = os.path.join(raw, "vcs-%p.profraw")
    for cmd in ([tool("bench.py"), "--name", "pgo-train"], [tool("scenes.py"), "capture", os.path.join(PGO_DIR, "scenes")]):
        print("[build] training: " + " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
        subprocess.call([sys.executable] + cmd, env=run_env)
    profiles = glob.glob(os.path.join(raw, "*.profraw"))
    if not profiles:
        tc.fail("training wrote no profiles (build/pgo/raw)")
    profdata = shutil.which("llvm-profdata", path=env.get("PATH")) or "llvm-profdata"
    subprocess.check_call([profdata, "merge", "-o", PROFDATA] + profiles, env=env)
    print("[build] profile: " + tc.show(PROFDATA) + " from %d runs" % len(profiles))


def tool(name):
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), name)


def iop_tests(env):
    """ps2xIOP on its own with PS2X_IOP_BUILD_TESTS=ON in build/iop_tests; runs the SPU2 checks."""
    out = os.path.join(tc.BUILD, "iop_tests")
    if not os.path.isfile(os.path.join(out, "build.ninja")) or tc.stale_tools(out) or \
            tc.drop_other_compiler(out, env) is not None:
        subprocess.check_call(tc.resolve(["cmake", "-S", os.path.join(tc.ROOT, "third_party", "PS2Recomp", "ps2xIOP"),
                                          "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                                          "-DPS2X_IOP_BUILD_TESTS=ON", "-DPS2X_CPU=" + tc.check_cpu()] +
                                         tc.compiler_defines(env), env), env=env)
    if not tc.cmake_build(env, "ps2_iop_spu2_tests", "build_iop_tests.log", build_dir=out):
        return 1
    return subprocess.call([tc.exe(os.path.join(out, "ps2_iop_spu2_tests"))])


if __name__ == "__main__":
    main()
