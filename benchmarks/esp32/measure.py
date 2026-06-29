#!/usr/bin/env python3
"""Orchestrate the ESP32 server comparison and emit an actionable report.

For one device IP, this:
  1. Reads /stats (heap/uptime baseline).
  2. Runs a connection sweep (1,2,4,8,16) measuring rps + latency.
  3. Re-reads /stats to capture peak heap usage under load.
  4. Estimates CPU-per-request: idle CPU is fixed, so CPU_us/req ~= (cores*1e6)/rps
     at saturation; we report rps_at_saturation as the proxy and heap deltas.
  5. Writes results JSON + a Markdown row.

Run once per firmware (flash http-c, measure; flash esp, measure), then
diff the two JSON files. Usage:
  python3 measure.py <ip> --label http-c --duration 15 --out results-httpc.json
"""
import argparse, json, subprocess, sys, urllib.request, os, time

HERE = os.path.dirname(os.path.abspath(__file__))

def stats(ip):
    try:
        with urllib.request.urlopen(f"http://{ip}/stats", timeout=5) as r:
            return json.loads(r.read())
    except Exception as e:
        print(f"warn: /stats failed: {e}", file=sys.stderr); return {}

def load(ip, c, d):
    out = subprocess.check_output([sys.executable, os.path.join(HERE,"loadgen.py"),
        ip, "-c", str(c), "-d", str(d)], text=True)
    return json.loads(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip"); ap.add_argument("--label", required=True)
    ap.add_argument("--duration", type=float, default=15)
    ap.add_argument("--conns", default="1,2,4,8,16")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    base = stats(a.ip)
    sweep, peak_min_heap = [], base.get("min_free_heap", 0)
    for c in [int(x) for x in a.conns.split(",")]:
        r = load(a.ip, c, a.duration); sweep.append(r)
        s = stats(a.ip)
        peak_min_heap = min(peak_min_heap or 1<<30, s.get("min_free_heap", 1<<30))
        print(f"[{a.label}] c={c:>2} rps={r['rps']:>8} p99={r['lat_p99_ms']}ms err={r['errors']}")
        time.sleep(1)
    best = max(sweep, key=lambda x: x["rps"])
    res = {"label": a.label, "base_free_heap": base.get("free_heap"),
           "peak_min_free_heap": peak_min_heap,
           "heap_used_under_load": (base.get("free_heap",0)-peak_min_heap),
           "best_rps": best["rps"], "best_conns": best["connections"],
           "cpu_us_per_req_est": round(2_000_000/best["rps"],1) if best["rps"] else None,
           "sweep": sweep}
    print(json.dumps(res, indent=2))
    if a.out:
        with open(a.out,"w") as f: json.dump(res,f,indent=2)
        print(f"wrote {a.out}")

if __name__ == "__main__": main()
