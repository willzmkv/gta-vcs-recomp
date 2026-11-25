#!/usr/bin/env python3
"""The open-source libraries the build downloads, bundled so a release builds without git or internet.

    python tools/deps.py list [--platform windows|linux]   what that build fetches (read from the CMake files)
    python tools/deps.py fetch [DIR] [--platform P]        each one into DIR (default deps/): a shallow clone at its
                                         pin without .git, on Windows the FFmpeg archive; DIR/manifest.json records the
                                         pins and the platform
    python tools/deps.py check [DIR] [--platform P]        is DIR complete and at this source's pins? (exit 0 / 1)
# The platform is this machine's unless given: both Windows and Linux include the Vulkan backend headers/loader (src/rhi),
# while FFmpeg comes from the prebuilt Windows archive or Linux's system packages.

With deps/ in the repository root, toolchain.configure points CMake at it (FETCHCONTENT_SOURCE_DIR_<NAME>, the FFmpeg
URL, FETCHCONTENT_FULLY_DISCONNECTED): nothing is downloaded. tools/release.py runs `fetch` into the release.
The pins stay in the CMake files; this reads them, so there is one list.
"""
import argparse
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

# the CMake files with FetchContent_Declare that the game and recompiler builds use (not ps2xStudio, which is off; not
# sse2neon, ARM only). The launcher downloads nothing.
CMAKE_FILES = [
    "src/platform/CMakeLists.txt",
    "src/rhi/CMakeLists.txt",
    "third_party/PS2Recomp/ps2xRecomp/CMakeLists.txt",
    "third_party/PS2Recomp/ps2xAnalyzer/CMakeLists.txt",
]
CMAKE_FILES_LINUX = CMAKE_FILES
PLATFORMS = ("windows", "linux")


def this_platform():
    return "windows" if tc.IS_WINDOWS else "linux"
FFMPEG_CMAKE = "third_party/PS2Recomp/ps2xRuntime/CMakeLists.txt"
# a release archive instead of the repository where the repository carries far more than the build needs
ARCHIVES = {
    "nlohmann_json": "https://github.com/nlohmann/json/releases/download/{tag}/json.tar.xz",  # 0.2 MB, not 188
}


def declared(platform=None):
    """[{name, repo, tag}] of every FetchContent_Declare the platform's build uses, plus the FFmpeg archive's
    {name, url} (None on Linux: the system's FFmpeg)."""
    platform = platform or this_platform()
    deps = {}
    for rel in CMAKE_FILES:
        with open(os.path.join(tc.ROOT, rel), encoding="utf-8") as f:
            text = re.sub(r"#[^\n]*", "", f.read())  # comments may hold parentheses
        for m in re.finditer(r"FetchContent_Declare\(\s*(\w+)([^)]*)\)", text):
            name, body = m.group(1), m.group(2)
            repo = re.search(r"GIT_REPOSITORY\s+\"?([^\s\"]+)", body)
            tag = re.search(r"GIT_TAG\s+\"?([^\s\"]+)", body)
            if not repo or not tag:
                tc.fail("%s: %s has no GIT_REPOSITORY / GIT_TAG - tools/deps.py can't bundle it" % (rel, name))
            dep = {"name": name, "repo": repo.group(1), "tag": tag.group(1)}
            other = deps.get(name.lower())
            if other and (other["repo"], other["tag"]) != (dep["repo"], dep["tag"]):
                tc.fail("%s is pinned twice, differently: %s %s and %s %s (%s)" % (name, other["repo"], other["tag"],
                                                                                dep["repo"], dep["tag"], rel))
            deps[name.lower()] = dep
    with open(os.path.join(tc.ROOT, FFMPEG_CMAKE), encoding="utf-8") as f:
        text = f.read()
    tag = re.search(r'set\(FFMPEG_PREBUILT_TAG "([^"]+)"\)', text).group(1)
    url = re.search(r'"(https://[^"]*ffmpeg[^"]*\.zip)"', text).group(1).replace("${FFMPEG_PREBUILT_TAG}", tag)
    ffmpeg = {"name": "ffmpeg", "url": url, "file": url.rsplit("/", 1)[1]} if platform == "windows" else None
    return sorted(deps.values(), key=lambda d: d["name"].lower()), ffmpeg


def _remove_tree(path):
    def writable(func, p, _):
        os.chmod(p, stat.S_IWRITE)  # git's pack files are read-only
        func(p)
    shutil.rmtree(path, onerror=writable)


def _move(src, dst):
    """A folder renamed; files just written can be held open for a moment (a virus scanner): retry, then copy."""
    for attempt in range(6):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            time.sleep(0.5 * (attempt + 1))
    shutil.copytree(src, dst)
    _remove_tree(src)


def download(url, out):
    tc.download_file(url, out + ".part", timeout=120)
    os.replace(out + ".part", out)


def fetch(out_dir, platform=None):
    platform = platform or this_platform()
    deps, ffmpeg = declared(platform)
    os.makedirs(out_dir, exist_ok=True)
    manifest = {"platform": platform, "deps": deps, "ffmpeg": ffmpeg}
    for i, dep in enumerate(deps):
        target = os.path.join(out_dir, dep["name"])
        tc.stage("dep_" + dep["name"], "Fetching " + dep["name"])
        tc.event(progress=i, total=len(deps) + 1, eta=None, note=dep["name"])
        if os.path.isdir(target):
            _remove_tree(target)
        archive = ARCHIVES.get(dep["name"].lower())
        if archive:
            url = archive.format(tag=dep["tag"])
            print("[deps] %s %s (%s)" % (dep["name"], dep["tag"], url))
            tmp = target + ".tar.xz"
            download(url, tmp)
            with tarfile.open(tmp) as t:
                t.extractall(target + ".x", filter="data")
            os.remove(tmp)
            inner = os.listdir(target + ".x")  # the archive holds one folder
            _move(os.path.join(target + ".x", inner[0]) if len(inner) == 1 else target + ".x", target)
            if os.path.isdir(target + ".x"):
                _remove_tree(target + ".x")
            continue
        print("[deps] %s %s (%s)" % (dep["name"], dep["tag"], dep["repo"]))
        cmd = ["git", "-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1", "--branch", dep["tag"],
               dep["repo"], target]
        if subprocess.run(cmd).returncode != 0:
            tc.fail("git clone of %s %s failed" % (dep["repo"], dep["tag"]), "download_failed")
        _remove_tree(os.path.join(target, ".git"))
    if ffmpeg:
        tc.stage("dep_ffmpeg", "Fetching FFmpeg")
        print("[deps] ffmpeg (%s)" % ffmpeg["url"])
        download(ffmpeg["url"], os.path.join(out_dir, ffmpeg["file"]))
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print("[deps] %d libraries%s in %s (%s)" % (len(deps), " and FFmpeg" if ffmpeg else "", out_dir, platform))
    tc.event(result="ok", dir=os.path.abspath(out_dir))


def check(out_dir, platform=None):
    """Problems with out_dir against this source's pins ([] = complete)."""
    platform = platform or this_platform()
    deps, ffmpeg = declared(platform)
    try:
        with open(os.path.join(out_dir, "manifest.json"), encoding="utf-8") as f:
            manifest = json.load(f)
    except (OSError, ValueError):
        return ["no manifest.json (not made by tools/deps.py fetch)"]
    if manifest.get("platform", "windows") != platform:  # older manifests: Windows
        return ["made for %s, not %s" % (manifest.get("platform", "windows"), platform)]
    have = {(d["name"], d["repo"], d["tag"]) for d in manifest.get("deps", [])}
    problems = ["%s %s missing or at another pin" % (d["name"], d["tag"]) for d in deps
                if (d["name"], d["repo"], d["tag"]) not in have or not os.path.isdir(os.path.join(out_dir, d["name"]))]
    if ffmpeg and not os.path.isfile(os.path.join(out_dir, ffmpeg["file"])):
        problems.append(ffmpeg["file"] + " missing")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("--platform", choices=PLATFORMS)
    for name in ("fetch", "check"):
        p = sub.add_parser(name)
        p.add_argument("dir", nargs="?", default=tc.DEPS_DIR)
        p.add_argument("--platform", choices=PLATFORMS)
    a = ap.parse_args()
    if a.cmd == "list":
        deps, ffmpeg = declared(a.platform)
        for d in deps:
            print("%-22s %-28s %s" % (d["name"], d["tag"], d["repo"]))
        print("%-22s %-28s %s" % ("ffmpeg", "", ffmpeg["url"] if ffmpeg else "(the system's)"))
    elif a.cmd == "fetch":
        fetch(a.dir, a.platform)
    else:
        problems = check(a.dir, a.platform)
        for p in problems:
            print("[deps] " + p)
        print("[deps] " + ("complete" if not problems else "incomplete") + ": " + a.dir)
        sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
