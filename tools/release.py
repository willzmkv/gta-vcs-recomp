#!/usr/bin/env python3
"""Make a release: the Windows zip, or the Linux AppImage - the source, the launcher and the libraries (Windows: and
Python), everything but the game.

    python tools/release.py [--ref TAG | --worktree] [--version X] [--launcher EXE] [--out DIR]

    --ref TAG       the source at a tag or commit (git archive; default HEAD)
    --worktree      the working tree as it is, uncommitted files included (tracked + untracked, not ignored) - tests
    --version X     the release's number (default: VERSION in the repository; a version tag must match it)
    --launcher EXE  a VCSLauncher.exe built elsewhere (CI); default: python tools/build.py --launcher
    --python X.Y.Z  the embeddable Python to bundle (default: the version running this)

Writes build/dist/VCS-Recomp-<version>-windows.zip and its .sha256. Inside, one folder:
    VCSLauncher.exe, VERSION, RELEASE.json (version, repository, file list for tools/update.py),
    python/ (python.org's embeddable CPython), deps/ (tools/deps.py fetch), THIRD_PARTY_LICENSES.txt, the source.
Never game code or data (DECISIONS.md, source-only): before zipping, every file is checked - no disc images, no
PS2 executables (MIPS ELF), nothing from generated/, build/ or the Ghidra project - and the release stops if one is.
Every file in the zip is dated a day before the commit (zip_time): never in the future, whatever the extractor's zone.

    python3 tools/release.py --platform linux [--ref TAG | --worktree] [--launcher BIN] [--allow-new-glibc]

Writes build/dist/VCS-Recomp-<version>-x86_64.AppImage and its .sha256: one file that runs on any x86-64 desktop with
glibc 2.35 or newer (Ubuntu 22.04, Debian 12 and later; the launcher must be built on such a system - CI's
ubuntu-22.04 - or the release refuses it, --allow-new-glibc for a test). Inside (AppDir): usr/bin/VCSLauncher, the
desktop entry and icon, and usr/share/vcs-recomp/release/ - the same files as the zip but Python and FFmpeg (the
system's) - which the launcher copies at start to the folder the game is built in (tools/update.py install). Made by
appimagetool with the type 2 runtime (static: no libfuse2 needed), both pinned below by version and SHA-256.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deps  # noqa: E402
import toolchain as tc  # noqa: E402

DIST = os.path.join(tc.BUILD, "dist")
APP_ID = "gta-vcs-launcher"  # the desktop entry's name and the Wayland app id (launcher/platform/linux/wayland.cpp)
APPIMAGETOOL = ("https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage",
                "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0")
APPIMAGE_RUNTIME = ("https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64",
                    "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d")
GLIBC_FLOOR = (2, 35)  # Ubuntu 22.04; Debian 12 has 2.36
CACHE = os.path.join(DIST, "cache")
FORBIDDEN_DIRS = ("generated/", "build/", "ghidra/project/", "ghidra/export/", "mc0/", "mc1/", "SLUS-21590/", "textures/")
FORBIDDEN_NAME = re.compile(r"(?i)(\.iso|\.gzf|\.chd|\.cso|\.cue)$|(^|/)(SLUS|SLES|SCUS|SLPM|SLPS)_\d{3}\.\d{2}")


def git(*args, **kw):
    return subprocess.run(["git", "-C", tc.ROOT] + list(args), check=True, capture_output=True, **kw).stdout


def repository():
    """owner/name of origin on GitHub ("" when not)."""
    try:
        url = git("remote", "get-url", "origin", text=True).strip()
    except (subprocess.CalledProcessError, OSError):
        return ""
    m = re.search(r"github\.com[:/]([^/]+/[^/.]+?)(\.git)?$", url)
    return m.group(1) if m else ""


def copy_source(stage, ref, worktree):
    if worktree:
        names = git("ls-files", "-z", "--cached", "--others", "--exclude-standard").split(b"\0")
        for name in filter(None, (n.decode("utf-8") for n in names)):
            src = os.path.join(tc.ROOT, name)
            if not os.path.isfile(src):
                continue  # deleted in the working tree
            dst = os.path.join(stage, name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
        return
    data = git("archive", "--format=tar", ref)
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(stage, filter="data")


def build_launcher():
    exe = os.path.join(tc.ROOT, "VCSLauncher.exe" if tc.IS_WINDOWS else "VCSLauncher")
    if subprocess.run([sys.executable, tc.tool("build.py"), "--launcher"]).returncode != 0:
        tc.fail("the launcher didn't build")
    return exe


def bundle_python(stage, version):
    name = "python-%s-embed-amd64.zip" % version
    cached = os.path.join(CACHE, name)
    if not os.path.isfile(cached):
        url = "https://www.python.org/ftp/python/%s/%s" % (version, name)
        print("[release] downloading " + url)
        os.makedirs(CACHE, exist_ok=True)
        deps.download(url, cached)
    with zipfile.ZipFile(cached) as z:
        z.extractall(os.path.join(stage, "python"))


def bundle_deps(stage, platform="windows"):
    cached = os.path.join(CACHE, "deps" if platform == "windows" else "deps-" + platform)
    if deps.check(cached, platform):
        if os.path.isdir(cached):
            shutil.rmtree(cached, onerror=lambda f, p, _: (os.chmod(p, 0o666), f(p)))
        deps.fetch(cached, platform)
    shutil.copytree(cached, os.path.join(stage, "deps"))


def licenses(stage):
    """THIRD_PARTY_LICENSES.txt from the bundled libraries' own licence files (and Python's, FFmpeg's)."""
    parts = []
    for base in sorted(os.listdir(os.path.join(stage, "deps"))):
        folder = os.path.join(stage, "deps", base)
        if not os.path.isdir(folder):
            continue
        for f in sorted(os.listdir(folder)):
            if re.match(r"(?i)(license|licence|copying)(\.(txt|md|rst))?$", f):
                with open(os.path.join(folder, f), encoding="utf-8", errors="replace") as fh:
                    parts.append("=" * 78 + "\n%s (deps/%s/%s)\n" % (base, base, f) + "=" * 78 + "\n" + fh.read())
    for z in (f for f in os.listdir(os.path.join(stage, "deps")) if f.endswith(".zip")):
        with zipfile.ZipFile(os.path.join(stage, "deps", z)) as zf:
            for n in zf.namelist():
                if re.search(r"(?i)(^|/)(license|copying)[^/]*$", n) and not n.endswith("/"):
                    parts.append("=" * 78 + "\nFFmpeg (deps/%s: %s)\n" % (z, n) + "=" * 78 + "\n"
                                 + zf.read(n).decode("utf-8", "replace"))
    py = os.path.join(stage, "python", "LICENSE.txt")
    if os.path.isfile(py):
        with open(py, encoding="utf-8", errors="replace") as fh:
            parts.append("=" * 78 + "\nPython (python/LICENSE.txt)\n" + "=" * 78 + "\n" + fh.read())
    with open(os.path.join(stage, "THIRD_PARTY_LICENSES.txt"), "w", encoding="utf-8") as f:
        f.write("The libraries and tools bundled with this release, under their own licences. The project itself is "
                "GPL-3.0 (LICENSE).\n\n" + "\n\n".join(parts))


def guard(stage):
    """Every file checked for game code or data; the release stops at the first."""
    problems = []
    for d, _, files in os.walk(stage):
        for f in files:
            path = os.path.join(d, f)
            rel = os.path.relpath(path, stage).replace(os.sep, "/")
            if any(rel.startswith(p) for p in FORBIDDEN_DIRS) or FORBIDDEN_NAME.search(rel):
                problems.append(rel + ": a game file's name or folder")
                continue
            size = os.path.getsize(path)
            if size > 200 << 20:
                problems.append(rel + ": %d MB, larger than anything a release carries" % (size >> 20))
                continue
            with open(path, "rb") as fh:
                head = fh.read(20)
            if head[:4] == b"\x7fELF" and len(head) >= 20 and int.from_bytes(head[18:20], "little") == 8:
                problems.append(rel + ": a MIPS ELF (a PS2 executable)")
            elif head[:6] == b"\x01CD001" or (size > 32768 + 6 and _iso_at(path)):
                problems.append(rel + ": a disc image")
    if problems:
        for p in problems:
            print("[release] REFUSED " + p)
        tc.fail("game files in the release (above): nothing was written", "release_refused")


def self_test(stage):
    """The release's own launcher, with its own Python and scripts, checks itself (VCSLauncher.exe --self-test: the
    scripts it runs are there, the bundled Python runs them, doctor.py answers). A release that fails isn't zipped."""
    out = os.path.join(DIST, "selftest.txt")
    code = subprocess.run([os.path.join(stage, "VCSLauncher.exe"), "--self-test", "--out", out], cwd=stage).returncode
    report = "(no output)"
    if os.path.isfile(out):
        with open(out, encoding="utf-8", errors="replace") as f:
            report = f.read()
        os.remove(out)
    for line in report.splitlines():
        print("[release]   " + line)
    if code != 0:
        tc.fail("the release's launcher failed its self-test (above): nothing was written", "release_selftest")
    # the self-test ran doctor.py, which may leave build/ (an environment cache): not part of a release
    shutil.rmtree(os.path.join(stage, "build"), ignore_errors=True)


# C++ runtimes Windows doesn't have: the launcher links them in (MSVC's static runtime, llvm-mingw's -static)
FOREIGN_RUNTIME_DLLS = ("libc++", "libunwind", "libwinpthread", "libgcc", "libstdc++", "vcruntime", "msvcp", "concrt")


def pe_imports(path):
    """The DLL names a PE32+ executable imports (its import directory), standard library only."""
    import struct
    with open(path, "rb") as f:
        data = f.read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections_count, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        tc.fail(path + " is not a 64-bit Windows program", "release_launcher")
    import_rva = struct.unpack_from("<I", data, optional + 112 + 8)[0]  # data directory 1: imports
    sections = [struct.unpack_from("<IIII", data, optional + optional_size + 40 * i + 8)
                for i in range(sections_count)]  # virtual size, virtual address, raw size, raw offset

    def offset(rva):
        for vsize, vaddr, rsize, raw in sections:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return raw + rva - vaddr
        raise ValueError("RVA %x outside the sections" % rva)

    names, entry = [], offset(import_rva) if import_rva else None
    while entry is not None:
        name_rva = struct.unpack_from("<I", data, entry + 12)[0]
        if not name_rva:
            break
        start = offset(name_rva)
        names.append(data[start:data.index(b"\0", start)].decode("ascii", "replace"))
        entry += 20
    return names


def check_launcher_imports(launcher):
    """A launcher that needs a C++ runtime DLL the release doesn't carry (a build without -static) starts nowhere."""
    foreign = [n for n in pe_imports(launcher) if n.lower().startswith(FOREIGN_RUNTIME_DLLS)]
    if foreign:
        tc.fail("the launcher needs %s, which Windows doesn't have: build it with the C++ runtime linked in "
                "(launcher/CMakeLists.txt)" % ", ".join(foreign), "release_launcher")


def zip_time(ref, worktree):
    """The one date every file in the zip carries. Zip dates are a wall-clock time with no time zone: the extractor's
    zone reads them, so a file packed at 23:44 in Adelaide came out 13.5 h in the future in Brazil, and Ninja re-ran
    CMake until it gave up (2026-10-31). The commit's time (SOURCE_DATE_EPOCH when set; now for --worktree) a day
    earlier, written as UTC, is in the past in every zone (UTC-12 to UTC+14) - and the same for every file and run."""
    if os.environ.get("SOURCE_DATE_EPOCH"):
        when = int(os.environ["SOURCE_DATE_EPOCH"])
    elif worktree:
        when = int(time.time())
    else:
        when = int(git("log", "-1", "--format=%ct", ref, text=True).strip())
    return max(time.gmtime(when - 86400)[:6], (1980, 1, 1, 0, 0, 0))


def _iso_at(path):
    with open(path, "rb") as fh:
        fh.seek(16 * 2048)
        return fh.read(6) == b"\x01CD001"


def pinned_download(url_sha, name):
    """A tool the release is made with, downloaded once into the cache and checked against its pinned SHA-256."""
    url, sha = url_sha
    path = os.path.join(CACHE, name)
    if not os.path.isfile(path) or hashlib.sha256(open(path, "rb").read()).hexdigest() != sha:
        print("[release] downloading " + url)
        os.makedirs(CACHE, exist_ok=True)
        deps.download(url, path)
        got = hashlib.sha256(open(path, "rb").read()).hexdigest()
        if got != sha:
            os.remove(path)
            tc.fail("%s: SHA-256 %s, expected %s" % (url, got, sha), "download_failed")
    os.chmod(path, 0o755)
    return path


def glibc_needed(binary):
    """The newest GLIBC_x.y symbol version an ELF binary asks for (its .dynstr names them)."""
    with open(binary, "rb") as f:
        found = re.findall(rb"GLIBC_(\d+)\.(\d+)", f.read())
    return max(((int(a), int(b)) for a, b in found), default=(0, 0))


def app_dir(stage, launcher, version):
    """The AppImage's tree: AppRun (the launcher), the desktop entry and icon at the top (appimagetool's contract), the
    launcher in usr/bin, the release's files in usr/share/vcs-recomp/release (returned; filled by the caller)."""
    os.makedirs(os.path.join(stage, "usr", "bin"))
    shutil.copy2(launcher, os.path.join(stage, "usr", "bin", "VCSLauncher"))
    os.chmod(os.path.join(stage, "usr", "bin", "VCSLauncher"), 0o755)
    os.symlink("usr/bin/VCSLauncher", os.path.join(stage, "AppRun"))
    with open(os.path.join(stage, APP_ID + ".desktop"), "w", encoding="utf-8") as f:
        f.write("[Desktop Entry]\nType=Application\nName=GTA: Vice City Stories\n"
                "Comment=The PS2 classic, recompiled for this PC: build it from your disc and play\n"
                "Exec=VCSLauncher\nIcon=%s\nCategories=Game;\nTerminal=false\nStartupWMClass=VCSLauncher\n"
                "X-AppImage-Version=%s\n" % (APP_ID, version))
    shutil.copy2(os.path.join(tc.ROOT, "launcher", "launcher.png"), os.path.join(stage, APP_ID + ".png"))
    os.symlink(APP_ID + ".png", os.path.join(stage, ".DirIcon"))
    payload = os.path.join(stage, "usr", "share", "vcs-recomp", "release")
    os.makedirs(payload)
    return payload


def self_test_appimage(appimage):
    """The AppImage as a player starts it: it copies its files into a fresh install folder, then checks itself (the
    scripts there, the system's Python running them, doctor.py's answer, Vulkan or lavapipe offscreen)."""
    folder = os.path.join(DIST, "selftest-install")
    shutil.rmtree(folder, ignore_errors=True)
    out = os.path.join(DIST, "selftest.txt")
    env = dict(os.environ, VCS_INSTALL_DIR=folder)
    code = subprocess.run([appimage, "--self-test", "--out", out], env=env).returncode
    report = "(no output)"
    if os.path.isfile(out):
        with open(out, encoding="utf-8", errors="replace") as f:
            report = f.read()
        os.remove(out)
    for line in report.splitlines():
        print("[release]   " + line)
    shutil.rmtree(folder, ignore_errors=True)
    if code != 0:
        os.remove(appimage)
        tc.fail("the AppImage failed its self-test (above): nothing was written", "release_selftest")


def release_linux(a, ref, version):
    name = "VCS-Recomp-%s-x86_64" % version
    stage = os.path.join(DIST, name + ".AppDir")
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    launcher = a.launcher or build_launcher()
    need = glibc_needed(launcher)
    print("[release] the launcher needs glibc %d.%d (the floor: %d.%d)" % (need + GLIBC_FLOOR))
    if need > GLIBC_FLOOR and not a.allow_new_glibc:
        tc.fail("the launcher needs glibc %d.%d, newer than the release's floor %d.%d: build it on Ubuntu 22.04 (CI "
                "does), or --allow-new-glibc for a test" % (need + GLIBC_FLOOR), "release_glibc")
    os.makedirs(os.path.dirname(stage), exist_ok=True)
    payload = app_dir(stage, launcher, version)
    print("[release] %s: source %s" % (name, "working tree" if a.worktree else ref))
    copy_source(payload, ref, a.worktree)
    with open(os.path.join(payload, "VERSION"), "w", encoding="utf-8") as f:
        f.write(version + "\n")
    print("[release] libraries (deps/, Linux: with the Vulkan backend's, FFmpeg from the system)")
    bundle_deps(payload, "linux")
    licenses(payload)
    with open(os.path.join(payload, "THIRD_PARTY_LICENSES.txt"), "a", encoding="utf-8") as f:
        f.write("\n\n" + "=" * 78 + "\nThe AppImage runtime (the start of the .AppImage file)\n" + "=" * 78 + "\n"
                "AppImage type 2 runtime, release 20251108, https://github.com/AppImage/type2-runtime (MIT), statically "
                "linked with libfuse 3 (LGPL-2.1), squashfuse (BSD-2-Clause), zstd (BSD-3-Clause) and zlib (zlib "
                "licence); their sources are at that release. Made with appimagetool 1.9.1 "
                "(https://github.com/AppImage/appimagetool, MIT).\n")
    files = sorted(os.path.relpath(os.path.join(d, f), payload).replace(os.sep, "/")
                   for d, _, fs in os.walk(payload) for f in fs)
    commit = "" if a.worktree else git("rev-parse", ref, text=True).strip()
    with open(os.path.join(payload, "RELEASE.json"), "w", encoding="utf-8") as f:
        json.dump({"version": version, "repository": repository(), "commit": commit, "platform": "linux",
                   "files": files + ["RELEASE.json"]}, f, indent=0)
    guard(payload)  # its paths are checked as a release's: build/, generated/, disc images...

    tool = pinned_download(APPIMAGETOOL, "appimagetool-1.9.1-x86_64.AppImage")
    runtime = pinned_download(APPIMAGE_RUNTIME, "runtime-20251108-x86_64")
    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, name + ".AppImage")
    if os.path.exists(out):
        os.remove(out)
    # extract-and-run: appimagetool is an AppImage itself, and CI has no FUSE
    env = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1",
               SOURCE_DATE_EPOCH=os.environ.get("SOURCE_DATE_EPOCH") or str(int(time.time())))
    print("[release] appimagetool (%d files)" % (len(files) + 1))
    if subprocess.run([tool, "--no-appstream", "--runtime-file", runtime, stage, out], env=env,
                      cwd=DIST).returncode != 0 or not os.path.isfile(out):
        tc.fail("appimagetool failed (above)", "release_failed")
    os.chmod(out, 0o755)
    self_test_appimage(out)
    shutil.rmtree(stage)
    digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
    with open(out + ".sha256", "w", encoding="utf-8") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(out)))
    print("[release] ok: %s (%.0f MB, sha256 %s)" % (out, os.path.getsize(out) / (1 << 20), digest[:16]))
    tc.event(result="ok", path=out, version=version)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--ref", default=None)
    src.add_argument("--worktree", action="store_true")
    ap.add_argument("--version")
    ap.add_argument("--launcher")
    ap.add_argument("--python", default="%d.%d.%d" % sys.version_info[:3])
    ap.add_argument("--allow-dirty", action="store_true", help="package --ref even with uncommitted changes")
    ap.add_argument("--out", default=DIST)
    ap.add_argument("--platform", choices=("windows", "linux"), default="windows" if tc.IS_WINDOWS else "linux")
    ap.add_argument("--allow-new-glibc", action="store_true", help="Linux: a launcher built on a newer system (tests)")
    a = ap.parse_args()
    if (a.platform == "windows") != tc.IS_WINDOWS:
        tc.fail("a %s release is made on %s (its launcher is built here)" % (a.platform, a.platform), "release_platform")

    ref = a.ref or "HEAD"
    if not a.worktree and not a.allow_dirty:
        # the launcher is built from the working tree, the source taken from a commit: with uncommitted changes the
        # two don't match (a new launcher next to old scripts - "build tools missing" in the window, 2026-10-30)
        changed = git("status", "--porcelain", "--untracked-files=normal", text=True).splitlines()
        if changed:
            for line in changed[:15]:
                print("[release]   " + line)
            if len(changed) > 15:
                print("[release]   ... and %d more" % (len(changed) - 15))
            tc.fail("uncommitted changes (above): the release would package %s's files next to a launcher built from "
                    "the working tree. Commit them, or use --worktree to package the working tree as it is "
                    "(--allow-dirty to go ahead anyway)" % ref, "release_dirty")
    # VERSION in the repository is the number (the launcher's exe carries it too: launcher/CMakeLists.txt); a version tag
    # must agree with it, or the release would say one number and its launcher another
    if a.worktree:
        repo_version = tc.source_version()["version"]
    else:
        try:
            repo_version = git("show", ref + ":VERSION", text=True).strip()
        except subprocess.CalledProcessError:
            repo_version = ""
    tag_version = re.sub(r"^v", "", a.ref) if a.ref and re.match(r"v?\d+\.\d+", a.ref) else ""
    if tag_version and repo_version and tag_version != repo_version and not a.version:
        tc.fail("the tag says %s, VERSION says %s: bump VERSION (and commit) before tagging" % (tag_version, repo_version),
                "release_version")
    version = a.version or tag_version or repo_version or "0.0.0-dev"
    if a.platform == "linux":
        release_linux(a, ref, version)
        return
    name = "VCS-Recomp-%s-windows" % version
    stage = os.path.join(DIST, name)
    if os.path.isdir(stage):
        shutil.rmtree(stage, onerror=lambda f, p, _: (os.chmod(p, 0o666), f(p)))
    os.makedirs(stage)

    print("[release] %s: source %s" % (name, "working tree" if a.worktree else ref))
    copy_source(stage, ref, a.worktree)
    with open(os.path.join(stage, "VERSION"), "w", encoding="utf-8") as f:
        f.write(version + "\n")
    launcher = a.launcher or build_launcher()
    check_launcher_imports(launcher)
    shutil.copy2(launcher, os.path.join(stage, "VCSLauncher.exe"))
    print("[release] python %s (embeddable)" % a.python)
    bundle_python(stage, a.python)
    print("[release] libraries (deps/)")
    bundle_deps(stage)
    licenses(stage)

    files = sorted(os.path.relpath(os.path.join(d, f), stage).replace(os.sep, "/")
                   for d, _, fs in os.walk(stage) for f in fs)
    commit = "" if a.worktree else git("rev-parse", ref, text=True).strip()
    with open(os.path.join(stage, "RELEASE.json"), "w", encoding="utf-8") as f:
        json.dump({"version": version, "repository": repository(), "commit": commit, "files": files + ["RELEASE.json"]},
                  f, indent=0)
    guard(stage)
    self_test(stage)

    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, name + ".zip")
    when = zip_time(ref, a.worktree)
    print("[release] zipping %d files, dated %04d-%02d-%02d %02d:%02d:%02d" % ((len(files) + 1,) + when))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in files + ["RELEASE.json"]:
            info = zipfile.ZipInfo.from_file(os.path.join(stage, rel), name + "/" + rel)
            info.date_time = when
            with open(os.path.join(stage, rel), "rb") as f:
                z.writestr(info, f.read(), zipfile.ZIP_DEFLATED, 9)
    digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
    with open(out + ".sha256", "w", encoding="utf-8") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(out)))
    print("[release] ok: %s (%.0f MB, sha256 %s)" % (out, os.path.getsize(out) / (1 << 20), digest[:16]))
    tc.event(result="ok", path=out, version=version)


if __name__ == "__main__":
    main()
