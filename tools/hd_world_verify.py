#!/usr/bin/env python3
"""Static verification for vcs_world_hd.cpp: checks for forbidden patterns that
corrupt guest memory or call guest functions from host threads.

Forbidden patterns (P0 severity):
- std::thread usage
- Direct call to lookupFunction(kRequestArea) from worker/host thread
- Use of level + 0x25C..0x288 as area slots (these are NOT area slots)
- Host thread calling guest function via synthetic R5900Context

Run:
    python tools/hd_world_verify.py [--fix]

Exit code: 0 = clean, 1 = violations found
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HD_WORLD_FILE = os.path.join(ROOT, "src", "game", "vcs_world_hd.cpp")

FORBIDDEN_PATTERNS = [
    # std::thread usage
    (r"\bstd::thread\b", "std::thread usage (host thread calling guest code)"),
    # Host thread worker function
    (r"processHdRequestQueue\s*\(", "Host worker function calling guest code"),
    # Direct lookup of RequestArea from host
    (r"lookupFunction\s*\(\s*0x002415B0\s*\)", "Direct lookup of RequestArea (0x002415B0) from host"),
    (r"lookupFunction\s*\(\s*kRequestArea\s*\)", "Direct lookup of kRequestArea from host"),
    # Use of level + 0x25C..0x288 as area slots (P0 - wrong offsets)
    (r"level\s*\+\s*0x2(5[C-F]|6[0-9A-F]|7[0-9A-F]|8[0-8])\b", "Use of level + 0x25C..0x288 as area slots (INVALID - these are level fields, not area slots)"),
    # Synthetic R5900Context for guest calls
    (r"R5900Context\s+\w+\s*\{", "Synthetic R5900Context creation for guest function call"),
    # startHdWorker / g_hdWorkerThread
    (r"startHdWorker\s*\(", "Host worker thread startup"),
    (r"g_hdWorkerThread\b", "Host worker thread variable"),
    (r"g_hdWorkerRunning\b", "Host worker thread flag"),
    # g_hdRequestQueue (host-side request queue)
    (r"g_hdRequestQueue\b", "Host-side request queue"),
    # g_hdAreaSlots initialization with wrong offsets
    (r"g_hdAreaSlots\[\d+\]\s*=\s*level\s*\+\s*0x2(5[C-F]|6[0-9A-F]|7[0-9A-F]|8[0-8])\b", "g_hdAreaSlots initialized with invalid level offsets"),
    # initHDAreaSlots using wrong offsets
    (r"initHDAreaSlots\s*\(", "initHDAreaSlots function (uses invalid area slots)"),
]

REQUIRED_PATTERNS = [
    # Fallback behavior when hd_radius > 0
    (r"g_hdRadius\s*\.", "g_hdRadius usage"),
    (r"g_origChooseArea\s*\(", "Original chooseArea call"),
    (r"g_origSwapArea\s*\(", "Original swapArea call"),
    (r"g_origWorldDraw\s*\(", "Original worldDraw call"),
]

def check_file(filepath, fix=False):
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()
    
    lines = content.splitlines()
    violations = []
    
    for pattern, desc in FORBIDDEN_PATTERNS:
        for i, line in enumerate(lines, 1):
            # Skip comment lines
            stripped = line.lstrip()
            if stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
                continue
            if re.search(pattern, line):
                violations.append((i, line.strip(), desc, pattern))
    
    missing_required = []
    for pattern, desc in REQUIRED_PATTERNS:
        if not re.search(pattern, content):
            missing_required.append((desc, pattern))
    
    # Check required patterns (ignoring comments)
    actual_missing = []
    for pattern, desc in REQUIRED_PATTERNS:
        found = False
        for line in lines:
            stripped = line.lstrip()
            if stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
                continue
            if re.search(pattern, line):
                found = True
                break
        if not found:
            actual_missing.append((desc, pattern))
    
    return violations, actual_missing, lines

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fix", action="store_true", help="Attempt to fix by commenting out forbidden code (experimental)")
    args = ap.parse_args()
    
    print(f"Checking {HD_WORLD_FILE}...")
    
    violations, missing_required, lines = check_file(HD_WORLD_FILE, args.fix)
    
    if violations:
        print("\n=== FORBIDDEN PATTERNS FOUND (P0) ===")
        for line_num, line, desc, pattern in violations:
            print(f"  Line {line_num}: {desc}")
            print(f"    Code: {line}")
        print()
    
    if missing_required:
        print("=== REQUIRED PATTERNS MISSING ===")
        for desc, pattern in missing_required:
            print(f"  Missing: {desc}")
        print()
    
    if args.fix and violations:
        # Simple fix: comment out forbidden lines
        new_lines = []
        for i, line in enumerate(lines, 1):
            is_violation = False
            for v_line, v_code, v_desc, v_pattern in violations:
                if i == v_line:
                    is_violation = True
                    break
            if is_violation:
                new_lines.append(f"// [HD_WORLD_FIX] {line}")
            else:
                new_lines.append(line)
        
        backup = HD_WORLD_FILE + ".bak"
        import shutil
        shutil.copy2(HD_WORLD_FILE, backup)
        print(f"Backup saved to {backup}")
        
        with open(HD_WORLD_FILE, "w", encoding="utf-8") as f:
            f.write("\n".join(new_lines))
        print("Forbidden lines commented out.")
        
        # Re-check
        violations2, missing2, _ = check_file(HD_WORLD_FILE)
        if not violations2:
            print("All forbidden patterns removed.")
        else:
            print(f"WARNING: {len(violations2)} violations remain after fix attempt.")
    
    if violations:
        print(f"FAIL: {len(violations)} forbidden pattern(s) found")
        return 1
    else:
        print("PASS: No forbidden patterns found")
        if missing_required:
            print("WARNING: Some required fallback patterns are missing")
        return 0

if __name__ == "__main__":
    sys.exit(main())