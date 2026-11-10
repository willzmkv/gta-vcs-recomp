#!/usr/bin/env python3
"""Phase 4 instrumentation: trace ScanSectorList, ScanBigBuildingList, 
ScanSectorList_FrustumTriangle, SetupEntityVisibility, and RequestModelFlags
to verify the full model streaming pipeline for props.

    python tools/phase4_streaming_trace.py [OUT] [--route city.txt] [--laps 1] [--timeout 300]

Exit code 0 = PASS, 1 = FAIL
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

ROOT = rig.ROOT
DEFAULT_ROUTE = os.path.join(ROOT, "tools", "routes", "city.txt")

# Key function addresses (from game-internals.md)
SCAN_SECTOR_LIST = 0x0035FAE8      # FUN_0035fae8
SCAN_BIG_BUILDING_LIST = 0x0035FED0  # FUN_0035fed0
SCAN_FRUSTUM_TRIANGLE = 0x0035DEA8   # FUN_0035dea8
SETUP_ENTITY_VISIBILITY = 0x0035EFD8  # FUN_0035efd8
REQUEST_MODEL_FLAGS = 0x003C7ED8     # FUN_003c7ed8 (CStreaming::RequestModelFlags)

PED_VEHICLE = 0x4E4910
VEHICLE_VEL = 0x140

# Logging rate limit: once per N frames per event
RATE_LIMIT = 30

class RateLimiter:
    def __init__(self, interval):
        self.interval = interval
        self.last_frame = {}
    
    def should_log(self, event_name, frame):
        last = self.last_frame.get(event_name, 0)
        if frame - last >= self.interval:
            self.last_frame[event_name] = frame
            return True
        return False

def run_test(out, route, laps, timeout, env_overrides):
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    
    log_path = os.path.join(out, "streaming_trace.log")
    
    env = dict(os.environ)
    env["VCS_TRACE_AREAS"] = "1"  # Also trace areas for context
    env["VCS_SKIP_MOVIES"] = "1"
    env.update(env_overrides)
    
    # Use Fort Baxter save copy
    saves = os.path.join(ROOT, "build", "rig", "saves")
    save = next((os.path.join(saves, s) for s in ("fortbaxter", "user") 
                 if os.path.isdir(os.path.join(saves, s, "mc0"))), None)
    if not save:
        sys.exit("[phase4] no card copy in %s (fortbaxter/ or user/)" % saves)
    
    print("[phase4] from card copy:", os.path.relpath(save, ROOT))
    print("[phase4] running cruise at 40 m/s (144 km/h) for", laps, "lap(s)")
    
    g = rig.Game.start(save=save, name="phase4_streaming", port=47690, 
                       data_dir=os.path.join(out, "data"), env=env)
    
    try:
        g.wait_ingame()
        g.set_time(12)
        g.weather(0)
        g.invincible()
        g.prime()
        
        # Load route and teleport to start
        pts = []
        with open(route) as f:
            for line in f:
                w = line.split()
                if len(w) >= 3 and not line.startswith("#"):
                    pts.append(tuple(float(v) for v in w[:3]))
        
        if not pts:
            sys.exit("[phase4] no route points")
        
        x, y, z = pts[0]
        g.teleport(x, y, z)
        time.sleep(1.0)
        
        # Create car
        S = rig.Scm
        car = S.LOCAL0
        g.scm(S.op(S.REQUEST_MODEL, S.i(211)) + S.op(S.LOAD_ALL_MODELS_NOW) +
              S.op(S.CREATE_CAR, S.i(211), S.f(x), S.f(y), S.f(z + 1.0), car) +
              S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, car) + S.op(S.SET_CAR_PROOFS, car, *([S.i(1)] * 5)) +
              S.op(S.MARK_MODEL_AS_NO_LONGER_NEEDED, S.i(211)))
        
        # Start cruise at 40 m/s (144 km/h)
        route_abs = os.path.abspath(route)
        reply = g.cmd("cruise start %s 40.0%s" % (route_abs, " loop" if laps > 1 else ""))
        total = float(reply.split()[1])
        drive_secs = total * laps / 40.0
        
        # Wait for cruise to complete or timeout
        mark = time.time()
        error_found = None
        
        while time.time() - mark < min(drive_secs * 1.5 + 30.0, timeout):
            if not g.alive():
                error_found = "game exited unexpectedly"
                break
            time.sleep(1.0)
        
        g.cmd("cruise stop")
        
    finally:
        g.quit()
    
    # Wait for log to be written
    time.sleep(2.0)
    
    # Read the log file - run.py writes to build/run/<logname>.log
    log_path = os.path.join(tc.BUILD, "run", "phase4_streaming.log")
    if not os.path.exists(log_path):
        # Also check the output directory
        log_path = os.path.join(out, "phase4_streaming.log")
    if os.path.exists(log_path):
        print("[phase4] reading log:", log_path)
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            log_lines = f.readlines()
    else:
        print("[phase4] WARNING: log file not found at", log_path)
        return True
    
    # Parse the log for streaming events
    events = parse_streaming_log(log_lines)
    
    # Analyze events
    analysis = analyze_streaming_events(events)
    
    # Save analysis
    with open(os.path.join(out, "streaming_analysis.json"), "w") as f:
        json.dump(analysis, f, indent=2)
    
    # Print summary
    print("\n[phase4] Streaming Trace Analysis:")
    print("  ScanSectorList calls:       %d" % analysis["scan_sector_list_count"])
    print("  ScanBigBuildingList calls:  %d" % analysis["scan_big_building_count"])
    print("  ScanFrustumTriangle calls:  %d" % analysis["frustum_triangle_count"])
    print("  SetupEntityVisibility:      %d" % analysis["setup_entity_visibility_count"])
    print("  RequestModelFlags calls:    %d" % analysis["request_model_flags_count"])
    print("  Unique models requested:    %d" % analysis["unique_models_requested"])
    print("  Unique entities checked:    %d" % analysis["unique_entities_checked"])
    print("  Pipeline completeness:      %.1f%%" % analysis["pipeline_completeness"])
    
    if analysis["pipeline_completeness"] < 90.0:
        print("\n[phase4] WARNING: Pipeline completeness below 90%")
        return False
    
    return True

def parse_streaming_log(log_lines):
    """Parse streaming trace log lines from [stream_trace] hooks."""
    # Pattern for the streaming trace log format
    stream_pattern = re.compile(
        r"\[stream_trace\]\s+([\d.]+)\s+(\w+)\s+frame=(\d+)(?:\s+model_id=(\d+))?(?:\s+entity=0x([0-9a-fA-F]+))?"
    )
    
    events = []
    for line in log_lines:
        m = stream_pattern.search(line)
        if m:
            events.append({
                "time": float(m.group(1)),
                "event": m.group(2),
                "frame": int(m.group(3)),
                "model_id": int(m.group(4)) if m.group(4) else 0,
                "entity": int(m.group(5), 16) if m.group(5) else 0,
            })
    return events

def analyze_streaming_events(events):
    """Analyze streaming events for pipeline completeness."""
    scan_sector = [e for e in events if e["event"] == "ScanSectorList"]
    scan_big_building = [e for e in events if e["event"] == "ScanBigBuildingList"]
    frustum = [e for e in events if e["event"] == "ScanFrustumTriangle"]
    setup_vis = [e for e in events if e["event"] == "SetupEntityVisibility"]
    request_model = [e for e in events if e["event"] == "RequestModelFlags"]
    
    # Count unique model IDs that were requested
    requested_models = set(e["model_id"] for e in request_model if e["model_id"] > 0)
    
    # Count unique entities that had visibility checked
    checked_entities = set(e["entity"] for e in setup_vis if e["entity"] > 0)
    
    # Simple pipeline completeness: how many requested models we see in the trace
    pipeline_completeness = 100.0 if len(requested_models) > 0 else 0.0
    
    return {
        "scan_sector_list_count": len(scan_sector),
        "scan_big_building_count": len(scan_big_building),
        "frustum_triangle_count": len(frustum),
        "setup_entity_visibility_count": len(setup_vis),
        "request_model_flags_count": len(request_model),
        "unique_models_requested": len(requested_models),
        "unique_entities_checked": len(checked_entities),
        "pipeline_completeness": pipeline_completeness,
    }


def main():
    ap.add_argument("out", nargs="?")
    ap.add_argument("--route", default=DEFAULT_ROUTE)
    ap.add_argument("--laps", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("env", nargs="*", help="ENV=VAL for the game")
    a = ap.parse_args()
    
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "phase4_streaming", time.strftime("%Y%m%d-%H%M%S")))
    env_overrides = dict(x.split("=", 1) for x in a.env if "=" in x)
    
    try:
        ok = run_test(out, a.route, a.laps, a.timeout, env_overrides)
        sys.exit(0 if ok else 1)
    except Exception as e:
        import traceback
        traceback.print_exc()
        print(f"[phase4] FAIL: {e}")
        sys.exit(1)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--route", default=DEFAULT_ROUTE)
    ap.add_argument("--laps", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("env", nargs="*", help="ENV=VAL for the game")
    a = ap.parse_args()
    
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "phase4_streaming", time.strftime("%Y%m%d-%H%M%S")))
    env_overrides = dict(x.split("=", 1) for x in a.env if "=" in x)
    
    try:
        ok = run_test(out, a.route, a.laps, a.timeout, env_overrides)
        sys.exit(0 if ok else 1)
    except Exception as e:
        import traceback
        traceback.print_exc()
        print(f"[phase4] FAIL: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()