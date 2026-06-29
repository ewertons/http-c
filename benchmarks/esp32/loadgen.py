#!/usr/bin/env python3
"""Dependency-free HTTP/1.1 keep-alive load generator for the ESP32 bench.

Opens N persistent connections, hammers GET <path>, measures throughput and
latency percentiles. Designed to fairly stress both http-c and esp_http_server
firmwares (identical 6-byte "hello" body). Prints a JSON summary.
"""
import argparse, asyncio, json, statistics, time

REQ_TMPL = "GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: keep-alive\r\n\r\n"

async def worker(host, port, path, deadline, lats, errs):
    req = REQ_TMPL.format(path=path, host=host).encode()
    try:
        r, w = await asyncio.wait_for(asyncio.open_connection(host, port), 5)
    except Exception:
        errs[0] += 1; return
    while time.monotonic() < deadline:
        t0 = time.monotonic()
        try:
            w.write(req); await w.drain()
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = await asyncio.wait_for(r.read(256), 5)
                if not chunk: raise ConnectionError
                data += chunk
            hdr = data.split(b"\r\n\r\n", 1)[0]
            clen = 0
            for line in hdr.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    clen = int(line.split(b":")[1])
            body = data.split(b"\r\n\r\n", 1)[1]
            while len(body) < clen:
                body += await asyncio.wait_for(r.read(clen - len(body)), 5)
            lats.append((time.monotonic() - t0) * 1000.0)
        except Exception:
            errs[0] += 1
            try: w.close()
            except Exception: pass
            return
    try: w.close()
    except Exception: pass

def pct(v, p):
    if not v: return 0.0
    s = sorted(v); return s[min(len(s)-1, int(len(s)*p/100))]

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host"); ap.add_argument("-p", "--port", type=int, default=80)
    ap.add_argument("-c", "--connections", type=int, default=4)
    ap.add_argument("-d", "--duration", type=float, default=15)
    ap.add_argument("--path", default="/")
    a = ap.parse_args()
    lats, errs, t0 = [], [0], time.monotonic()
    deadline = t0 + a.duration
    await asyncio.gather(*[worker(a.host, a.port, a.path, deadline, lats, errs)
                           for _ in range(a.connections)])
    dur = time.monotonic() - t0
    out = {"connections": a.connections, "duration_s": round(dur, 2),
           "requests": len(lats), "errors": errs[0],
           "rps": round(len(lats)/dur, 1),
           "lat_avg_ms": round(statistics.mean(lats), 2) if lats else 0,
           "lat_p50_ms": round(pct(lats,50),2), "lat_p90_ms": round(pct(lats,90),2),
           "lat_p99_ms": round(pct(lats,99),2), "lat_max_ms": round(max(lats),2) if lats else 0}
    print(json.dumps(out))

if __name__ == "__main__":
    asyncio.run(main())
