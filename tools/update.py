#!/usr/bin/env python3
"""Update a release folder to the newest release on GitHub. Only when asked; nothing is sent but the request itself.

    python tools/update.py check               the newest release vs this one (RELEASE.json's version)
    python tools/update.py apply [--zip FILE]  download the newest release's zip (or use FILE) and put it in place
    python3 tools/update.py apply [--appimage FILE]   Linux: the newest AppImage (or FILE) in place of the running one
    python3 tools/update.py install --from DIR --to FOLDER   Linux: an AppImage's release files into the folder the
                                               game is built in (the launcher runs it at start when they differ)

apply replaces the release's files with the new release's, removes the files the old release had and the new one
doesn't (RELEASE.json lists them), and keeps everything else: build/ (the built game, vcs.ini, the launcher's
settings), generated/, your disc, anything you added. The game then needs building again (the launcher says so;
it is incremental). A git clone isn't a release: update it with git pull.

Linux releases are an AppImage (tools/release.py): a read-only image holding the launcher and the release's files.
The game is built in a folder of its own ($XDG_DATA_HOME/VCSRecomp/install, VCS_INSTALL_DIR), which the launcher fills
from the image with `install` - only files whose bytes changed are written, so a rebuild after an update stays
incremental. `apply` there replaces the AppImage file itself ($APPIMAGE, set by the AppImage runtime); the next start
of the launcher puts the new files in place.
"""
import argparse
import filecmp
import hashlib
import json
import os
import re
import shutil
import sys
import time
import urllib.error
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

RELEASE_JSON = os.path.join(tc.ROOT, "RELEASE.json")
UPDATE_DIR = os.path.join(tc.BUILD, "update")
KEEP = ("build/", "generated/")  # never replaced or removed
ASSET_SUFFIX = "-windows.zip" if tc.IS_WINDOWS else "-x86_64.AppImage"


def this_release():
    try:
        with open(RELEASE_JSON, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def version_key(v):
    return [int(x) for x in re.findall(r"\d+", v)] or [0]


def latest(repo):
    req = urllib.request.Request("https://api.github.com/repos/%s/releases/latest" % repo,
                                 headers={"Accept": "application/vnd.github+json", "User-Agent": "vcs-recomp-update"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None  # no release published yet
        tc.fail("GitHub answered %d" % e.code, "network")
    except (OSError, ValueError) as e:
        tc.fail("could not reach GitHub: %s" % e, "network")
    assets = {a["name"]: a["browser_download_url"] for a in data.get("assets", [])}
    # this platform's: the Windows zip, the Linux AppImage
    zips = [n for n in assets if n.endswith(ASSET_SUFFIX)]
    return {"version": re.sub(r"^v", "", data.get("tag_name", "")), "url": data.get("html_url", ""),
            "zip": assets[zips[0]] if zips else "", "sha256": assets.get(zips[0] + ".sha256", "") if zips else ""}


def check():
    mine = this_release()
    if not mine:
        message = ("This is a git clone, not a release: update it with git pull, then build again."
                   if os.path.exists(os.path.join(tc.ROOT, ".git")) else "No RELEASE.json: not a release folder.")
        print("[update] " + message)
        tc.event(result="ok", newer=False, message=message)
        return
    repo = mine.get("repository") or ""
    if not repo:
        tc.fail("RELEASE.json names no repository", "error")
    found = latest(repo)
    if not found or not found["version"]:
        message = "No release published yet on github.com/%s." % repo
        print("[update] " + message)
        tc.event(result="ok", newer=False, message=message)
        return
    newer = version_key(found["version"]) > version_key(mine["version"])
    print("[update] this: %s, newest: %s%s" % (mine["version"], found["version"], " - an update is available" if newer else ""))
    tc.event(result="ok", newer=newer, current=mine["version"], latest=found["version"], url=found["url"],
             zip=found["zip"], message="" if newer else "You have the latest release (%s)." % mine["version"])


def set_aside(path):
    """A file in use (a running exe or DLL can be renamed, not replaced): moved to <name>.old, removed next time."""
    old = path + ".old"
    if os.path.exists(old):
        os.remove(old)
    os.replace(path, old)


def put(z, name, target):
    """One file of the release zip written to `target`. A file held open is tried again for a few seconds (an
    antivirus scanning it lets go), then renamed aside (running: the launcher, this Python - a running exe or DLL can
    be renamed, not replaced) and written fresh."""
    os.makedirs(os.path.dirname(target), exist_ok=True)
    for attempt in range(5):
        try:
            with z.open(name) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
            return
        except PermissionError:
            if attempt == 0:
                try:
                    set_aside(target)
                    continue
                except OSError:
                    pass
            if attempt == 4:
                raise
            time.sleep(0.5 * (attempt + 1))


def remove_set_aside():
    """The .old files a previous update couldn't remove (they were running then)."""
    for d in (tc.ROOT, os.path.join(tc.ROOT, "python")):
        if os.path.isdir(d):
            for f in os.listdir(d):
                if f.endswith(".old"):
                    try:
                        os.remove(os.path.join(d, f))
                    except OSError:
                        pass


def download(url, out):
    tc.stage("download", "Downloading the new release")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with urllib.request.urlopen(url, timeout=60) as r, open(out + ".part", "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            tc.event(progress=done >> 20, total=total >> 20, eta=None, note="MB")
    os.replace(out + ".part", out)


def verified_download(found, out):
    download(found["zip"], out)
    if found["sha256"]:
        with urllib.request.urlopen(found["sha256"], timeout=30) as r:
            want = r.read().decode().split()[0].lower()
        got = hashlib.sha256(open(out, "rb").read()).hexdigest()
        if got != want:
            os.remove(out)
            tc.fail("the download's checksum doesn't match: try again", "network")


def apply_appimage(mine, appimage_path=None):
    """Linux: the new AppImage in place of the running one (renamed over it: the running image stays mounted until the
    launcher ends). Its files go into the install folder when the launcher next starts (install below)."""
    target = os.environ.get("APPIMAGE", "")
    if not target or not os.path.isfile(target):
        tc.fail("not started from an AppImage ($APPIMAGE isn't set): a git clone updates with git pull", "git_checkout")
    new = target + ".new"
    if appimage_path:
        shutil.copyfile(appimage_path, new)
        version = "the new release (%s)" % os.path.basename(appimage_path)  # its number: when it starts
    else:
        found = latest(mine.get("repository", ""))
        if not found or not found["zip"]:
            tc.fail("no AppImage found in the newest release on GitHub", "network")
        if version_key(found["version"]) <= version_key(mine["version"]):
            print("[update] already the newest (%s)" % mine["version"])
            tc.event(result="ok", version=mine["version"])
            return
        verified_download(found, new)
        version = found["version"]
    tc.stage("apply", "Putting the new release in place")
    with open(new, "rb") as f:
        head = f.read(16)
    if head[:4] != b"\x7fELF" or head[8:11] != b"AI\x02":  # an ELF with the AppImage type 2 magic
        os.remove(new)
        tc.fail("not an AppImage: " + (appimage_path or new), "error")
    os.chmod(new, 0o755)
    os.replace(new, target)
    print("[update] %s -> %s: %s. Start the launcher again; it puts the new files in place, then build the game again."
          % (mine["version"], version, target))
    tc.event(result="ok", version=version, restart=True)  # the launcher offers Restart (the new AppImage)


def install(source, target):
    """An AppImage's release files (source: its usr/share/vcs-recomp/release) into the install folder: the files whose
    bytes differ written, the ones the previous release had and this one doesn't removed (RELEASE.json lists them),
    build/ and generated/ left alone. RELEASE.json last, so an interrupted install is redone."""
    new = None
    try:
        with open(os.path.join(source, "RELEASE.json"), encoding="utf-8") as f:
            new = json.load(f)
    except (OSError, ValueError):
        tc.fail("not a release's files (no RELEASE.json): " + source, "install_failed")
    try:
        with open(os.path.join(target, "RELEASE.json"), encoding="utf-8") as f:
            old = json.load(f)
    except (OSError, ValueError):
        old = {}
    tc.stage("install", "Putting the game's files in place" if not old else
             "Putting version %s's files in place" % new.get("version", "?"))
    names = [n for n in new.get("files", []) if n != "RELEASE.json" and not n.startswith(KEEP)]
    written = 0
    for i, rel in enumerate(names):
        src = os.path.join(source, *rel.split("/"))
        dst = os.path.join(target, *rel.split("/"))
        if not os.path.abspath(dst).startswith(os.path.abspath(target) + os.sep) or not os.path.isfile(src):
            continue
        if os.path.isfile(dst) and os.path.getsize(dst) == os.path.getsize(src) and filecmp.cmp(src, dst, shallow=False):
            continue  # the same bytes: its date stays, and nothing rebuilds for it
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        tmp = dst + ".new"
        shutil.copyfile(src, tmp)
        shutil.copymode(src, tmp)  # the scripts' executable bits
        os.replace(tmp, dst)
        written += 1
        if i % 200 == 0:
            tc.event(progress=i, total=len(names), eta=None, note="files")
    gone = sorted(set(old.get("files", [])) - set(new.get("files", [])))
    removed = 0
    for rel in gone:
        if rel.startswith(KEEP):
            continue
        path = os.path.join(target, *rel.split("/"))
        if os.path.isfile(path):
            os.remove(path)
            removed += 1
    shutil.copyfile(os.path.join(source, "RELEASE.json"), os.path.join(target, "RELEASE.json"))
    print("[update] %s: %d files written, %d already current, %d removed (%s)"
          % (new.get("version", "?"), written, len(names) - written, removed, target))
    tc.event(result="ok", version=new.get("version", ""), written=written, removed=removed)


def apply(zip_path=None, appimage_path=None):
    mine = this_release()
    if not mine:
        tc.fail("not a release folder (no RELEASE.json): a git clone updates with git pull", "git_checkout")
    if not tc.IS_WINDOWS:
        apply_appimage(mine, appimage_path)
        return
    if not zip_path:
        found = latest(mine.get("repository", ""))
        if not found or not found["zip"]:
            tc.fail("no release zip found on GitHub", "network")
        if version_key(found["version"]) <= version_key(mine["version"]):
            print("[update] already the newest (%s)" % mine["version"])
            tc.event(result="ok", version=mine["version"])
            return
        zip_path = os.path.join(UPDATE_DIR, found["zip"].rsplit("/", 1)[1])
        verified_download(found, zip_path)
    tc.stage("apply", "Putting the new release in place")
    failed = []  # (relative path, error): files that could not be written - the rest still goes in
    with zipfile.ZipFile(zip_path) as z:
        top = {n.split("/", 1)[0] for n in z.namelist()}
        if len(top) != 1:
            tc.fail("not a release zip (one folder expected): " + zip_path, "error")
        prefix = top.pop() + "/"
        new = json.loads(z.read(prefix + "RELEASE.json"))
        names = [n for n in z.namelist() if not n.endswith("/")]
        # VERSION and RELEASE.json last, and only when every other file is in: an update that stopped halfway once
        # left a folder saying 0.2.1 with half of 0.1.1's tools (issue 66). With RELEASE.json still the old one, the
        # launcher offers the update again and a second run writes what the first could not.
        last = [prefix + "VERSION", prefix + "RELEASE.json"]
        for i, n in enumerate([n for n in names if n not in last] + [n for n in last if n in names]):
            rel = n[len(prefix):]
            if not rel or rel.startswith(KEEP):
                continue
            if rel in ("VERSION", "RELEASE.json") and failed:
                continue
            target = os.path.join(tc.ROOT, *rel.split("/"))
            if not os.path.abspath(target).startswith(os.path.abspath(tc.ROOT) + os.sep):
                continue
            try:
                put(z, n, target)
            except OSError as e:
                print("[update] could not write %s: %s" % (rel, e))
                failed.append((rel, e))
            if i % 200 == 0:
                tc.event(progress=i, total=len(names), eta=None, note="files")
    if failed:
        locked = [rel for rel, e in failed if isinstance(e, PermissionError)]
        tc.fail("%d of the new release's files could not be written (%s%s)%s. The folder is still the old release: "
                "close the game and anything else using the folder, then update again - or extract the new release's "
                "zip into a new folder." % (
                    len(failed), ", ".join(rel for rel, _ in failed[:3]), ", ..." if len(failed) > 3 else "",
                    "; in use by another program - often an antivirus or a cloud-synced folder" if locked else ""),
                "update_incomplete")
    gone = sorted(set(mine.get("files", [])) - set(new.get("files", [])))
    for rel in gone:
        if rel.startswith(KEEP):
            continue
        path = os.path.join(tc.ROOT, *rel.split("/"))
        if os.path.isfile(path):
            try:
                os.remove(path)
            except PermissionError:
                try:
                    set_aside(path)
                except OSError as e:  # an old file left behind is harmless: the new release doesn't use it
                    print("[update] could not remove %s: %s" % (rel, e))
    print("[update] %s -> %s (%d files, %d removed). Build the game again (the launcher, or python tools/build.py "
          "--recompile-if-needed)." % (mine["version"], new["version"], len(names), len(gone)))
    tc.event(result="ok", version=new["version"], restart=True)  # the launcher offers Restart (its new exe)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    p = sub.add_parser("apply")
    p.add_argument("--zip", help="a release zip to apply instead of downloading the newest")
    p.add_argument("--appimage", help="Linux: an AppImage to put in place instead of downloading the newest")
    p = sub.add_parser("install")
    p.add_argument("--from", dest="source", required=True, help="the release's files (an AppImage's)")
    p.add_argument("--to", dest="target", required=True, help="the install folder")
    a = ap.parse_args()
    if a.cmd == "install":
        install(a.source, a.target)
        return
    remove_set_aside()
    if a.cmd == "check":
        check()
    else:
        apply(a.zip, a.appimage)


if __name__ == "__main__":
    main()
