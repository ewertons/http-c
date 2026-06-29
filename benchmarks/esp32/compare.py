#!/usr/bin/env python3
"""Diff two measure.py result JSONs into a side-by-side Markdown table."""
import json, sys
a = json.load(open(sys.argv[1])); b = json.load(open(sys.argv[2]))
def row(n,k,f="{}"): return f"| {n} | {f.format(a.get(k))} | {f.format(b.get(k))} |"
print(f"| Metric | {a['label']} | {b['label']} |")
print("|---|---|---|")
print(row("Best req/s","best_rps"))
print(row("Conns at best","best_conns"))
print(row("CPU us/req (est)","cpu_us_per_req_est"))
print(row("Heap used under load (B)","heap_used_under_load"))
print(row("Peak min free heap (B)","peak_min_free_heap"))
