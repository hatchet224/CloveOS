#!/usr/bin/env python3
"""Summarize boot-stage durations without exposing game titles or paths."""
from pathlib import Path
import argparse, json, re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('startup_log', type=Path)
parser.add_argument('--supervisor-log', type=Path)
args = parser.parse_args()
text = args.startup_log.read_text(encoding='utf-8', errors='replace')
stages = []
for line in text.splitlines():
    match = re.fullmatch(r'boot timing: stage=([a-z-]+) uptime_seconds=(\d+(?:\.\d+)?)', line)
    if match:
        stages.append({'stage': match[1], 'uptime_ms': round(float(match[2])*1000)})
    match = re.fullmatch(r'catalog timing: stage=([a-z-]+) uptime_ms=(\d+) elapsed_ms=(\d+)', line)
    if match:
        stages.append({'stage': 'catalog-'+match[1], 'uptime_ms': int(match[2]), 'elapsed_ms': int(match[3])})
if args.supervisor_log:
    for line in args.supervisor_log.read_text(encoding='utf-8', errors='replace').splitlines():
        match = re.fullmatch(r'ui timing: stage=first-frame uptime_ms=(\d+)', line)
        if match:
            stages.append({'stage': 'ui-first-frame', 'uptime_ms': int(match[1])})
            break
print(json.dumps({'stages': stages, 'first_frame_measured': any(s['stage']=='ui-first-frame' for s in stages)}, indent=2))
