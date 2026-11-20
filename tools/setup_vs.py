#!/usr/bin/env python3
"""Install what the Windows build needs from Microsoft: Visual Studio's Build Tools with the C++ tools and clang.

    python tools/setup_vs.py status                what's there and what's missing (tools/doctor.py's check)
    python tools/setup_vs.py install [--dry-run]   install or fix it; --dry-run prints what would run

What install does depends on what's there:
- nothing (or no Visual Studio with the C++ tools): downloads Microsoft's Build Tools for Visual Studio 2022
  bootstrapper (aka.ms, signed by Microsoft - checked before it runs) and installs the C++ workload with the
  recommended parts (MSVC, the Windows SDK, CMake and Ninja) and the C++ Clang tools. About 6 GB.
- a Visual Studio without the C++ Clang tools or CMake: the Visual Studio Installer adds them to it ("modify").
- a Visual Studio whose clang is too old: the Visual Studio Installer updates it.
Windows asks for administrator rights (one prompt); the installer shows its own progress window (--passive) and this
waits for it. The launcher's "Install build tools" runs this.
"""
import argparse
import os
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import doctor  # noqa: E402
import toolchain as tc  # noqa: E402

BOOTSTRAPPER_URL = "https://aka.ms/vs/17/release/vs_BuildTools.exe"
COMPONENTS = [
    "Microsoft.VisualStudio.Workload.VCTools",           # "Desktop development with C++" (Build Tools' name for it)
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",  # MSVC and its libraries (clang-cl links against them)
    "Microsoft.VisualStudio.Component.VC.CMake.Project",  # CMake and Ninja
    "Microsoft.VisualStudio.Component.VC.Llvm.Clang",     # "C++ Clang tools for Windows": clang-cl
    "Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset",
]
# the newest Windows SDK comes with --includeRecommended
ERROR_CANCELLED = 1223    # the UAC prompt was declined
REBOOT_REQUIRED = 3010


def adds(components=COMPONENTS):
    out = []
    for c in components:
        out += ["--add", c]
    return out


def run_elevated(exe, args, dry_run):
    """Runs `exe args` as administrator (ShellExecuteEx "runas") and waits; its exit code."""
    line = subprocess.list2cmdline(args)
    print("[setup] running (as administrator): %s %s" % (exe, line))
    if dry_run:
        return 0
    import ctypes
    from ctypes import wintypes

    class ShellExecuteInfo(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("fMask", ctypes.c_ulong), ("hwnd", wintypes.HWND),
                    ("lpVerb", wintypes.LPCWSTR), ("lpFile", wintypes.LPCWSTR), ("lpParameters", wintypes.LPCWSTR),
                    ("lpDirectory", wintypes.LPCWSTR), ("nShow", ctypes.c_int), ("hInstApp", wintypes.HINSTANCE),
                    ("lpIDList", ctypes.c_void_p), ("lpClass", wintypes.LPCWSTR), ("hkeyClass", wintypes.HKEY),
                    ("dwHotKey", wintypes.DWORD), ("hIcon", wintypes.HANDLE), ("hProcess", wintypes.HANDLE)]

    info = ShellExecuteInfo()
    info.cbSize = ctypes.sizeof(info)
    info.fMask = 0x00000040  # SEE_MASK_NOCLOSEPROCESS
    info.lpVerb = "runas"
    info.lpFile = exe
    info.lpParameters = line
    info.nShow = 1
    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    if not shell32.ShellExecuteExW(ctypes.byref(info)):
        return ctypes.get_last_error() or ERROR_CANCELLED
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    k32.WaitForSingleObject(info.hProcess, 0xFFFFFFFF)
    code = wintypes.DWORD()
    k32.GetExitCodeProcess(info.hProcess, ctypes.byref(code))
    k32.CloseHandle(info.hProcess)
    return code.value


def signed_by_microsoft(path):
    """The file's Authenticode signature is valid and Microsoft's (PowerShell's Get-AuthenticodeSignature)."""
    ps = ("$s = Get-AuthenticodeSignature -LiteralPath $env:VCS_SIGNED_FILE; "
          "if ($s.Status -eq 'Valid' -and $s.SignerCertificate.Subject -match 'O=Microsoft Corporation') { 'ok' } "
          "else { $s.Status }")
    env = dict(os.environ, VCS_SIGNED_FILE=path)
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, env=env,
                             timeout=120).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return False
    return out == "ok"


def download(url, out):
    tc.stage("download", "Downloading Microsoft's installer")
    print("[setup] downloading " + url)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    try:
        done = [0]
        def on_chunk(n, total):
            done[0] += n
            tc.event(progress=done[0], total=total, eta=None, note="bytes")
        tc.download_file(url, out + ".part", timeout=60, allow_unverified=True, on_chunk=on_chunk)
    except OSError as e:
        tc.fail("download failed: %s" % e, "network")
    os.replace(out + ".part", out)


def finish(code, what):
    if code == 0:
        return
    if code == ERROR_CANCELLED:
        tc.fail("the administrator prompt was declined", "elevation_denied")
    if code == REBOOT_REQUIRED:
        print("[setup] installed; Windows needs a restart to finish")
        tc.event(result="ok", reboot=True, message="Installed. Restart Windows to finish, then start the launcher again.")
        sys.exit(0)
    tc.fail("%s ended with code %d" % (what, code), "install_failed")


def install(dry_run):
    if not tc.IS_WINDOWS:
        tc.fail("Linux: install the packages python3 tools/build.py --check-deps lists", "not_windows")
    check = doctor.check_compiler()
    code = check["code"]
    print("[setup] now: " + check["message"])
    if check["ok"]:
        tc.event(result="ok", message="The build tools are already installed: " + check["message"])
        return
    installer = check.get("installer", "")
    vs = check.get("vs") or next((i for i in check.get("installs", []) if int(i["version"].split(".")[0] or 0) >= 17), None)
    if code in ("no_clang", "no_cmake", "old_clang") and vs and os.path.isfile(installer):
        tc.stage("install", "Updating Visual Studio" if code == "old_clang" else "Adding the missing parts to Visual Studio")
        args = ["update" if code == "old_clang" else "modify", "--installPath", vs["path"], "--passive", "--norestart"]
        if code != "old_clang":  # the parts only: the workload's ID differs between Build Tools and Visual Studio
            args += adds(COMPONENTS[1:])
        finish(run_elevated(installer, args, dry_run), "the Visual Studio Installer")
    else:
        bootstrapper = os.path.join(tc.BUILD, "setup", "vs_BuildTools.exe")
        if dry_run:
            print("[setup] would download " + BOOTSTRAPPER_URL)
        else:
            download(BOOTSTRAPPER_URL, bootstrapper)
            if not signed_by_microsoft(bootstrapper):
                os.remove(bootstrapper)
                tc.fail("the downloaded installer isn't signed by Microsoft - not running it", "install_failed")
        tc.stage("install", "Installing Visual Studio Build Tools (its window shows the progress)")
        args = ["--passive", "--wait", "--norestart", "--nocache", "--includeRecommended"] + adds()
        finish(run_elevated(bootstrapper, args, dry_run), "the Build Tools installer")
    if dry_run:
        return
    time.sleep(2)
    after = doctor.check_compiler()
    print("[setup] now: " + after["message"])
    if not after["ok"]:
        tc.fail("still missing after the installer: " + after["message"], "install_failed")
    tc.event(result="ok", message="The build tools are installed: " + after["message"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    i = sub.add_parser("install")
    i.add_argument("--dry-run", action="store_true", help="print what would run, run nothing")
    a = ap.parse_args()
    if a.cmd == "status":
        check = doctor.check_compiler()
        print(("ok: " if check["ok"] else "missing: ") + check["message"])
        for inst in check.get("installs", []):
            print("  %s %s  %s  %s" % (inst["name"], inst["version"], inst["path"],
                                       ", ".join(k for k, v in inst["components"].items() if v) or "no C++ parts"))
        sys.exit(0 if check["ok"] else 1)
    install(a.dry_run)


if __name__ == "__main__":
    main()
