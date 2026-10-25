"""Summarize measured Switch PERF logs; never infer stability from missing data.

Run: python packaging/profile_report.py performance.log
Repeat each scenario for at least 120 seconds at fixed clocks; validate audio,
input and visible stutter manually. Reports candidates, not verified stability.
"""
import argparse
import math
from collections import defaultdict
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    # Transient discovery/connecting/input transitions are retained in the log,
    # but cannot necessarily be held continuously for a two-minute sample.
    parser.add_argument('--scenarios', default='menu,puzzle,custom,lobby,match,winner')
    parser.add_argument('--seconds', type=float, default=120)
    args = parser.parse_args()
    required = set(args.scenarios.split(','))
    if not math.isfinite(args.seconds) or args.seconds < 120:
        parser.error('--seconds must be finite and at least 120')
    records = defaultdict(list)
    for line in args.log.read_text(encoding='utf-8', errors='replace').splitlines():
        if not line.startswith('PERF '):
            continue
        values = dict(token.split('=', 1) for token in line.split()[1:] if '=' in token)
        try:
            key = (values['mode'], int(values['cpu_hz']), int(values['gpu_hz']), int(values['emc_hz']))
            for field in ('seconds', 'worst_ms', 'work_worst_ms', 'late', 'missed', 'frames'):
                if not math.isfinite(float(values[field])) or float(values[field]) < 0:
                    raise ValueError('Invalid sample')
            records[key].append(values)
        except (KeyError, ValueError):
            continue
    if not records:
        parser.error('No valid PERF summaries found')
    results = {}
    print('Mode                         CPU MHz GPU MHz EMC MHz  Result')
    for key, rows in sorted(records.items()):
        # A summary is cumulative within a scenario/configuration segment.
        # Require one sustained segment per scenario instead of adding repeated snapshots.
        durations = defaultdict(float)
        for row in rows:
            durations[row['scenario']] = max(durations[row['scenario']], float(row['seconds']))
        missing = sorted(s for s in required if durations[s] < args.seconds)
        known = all(key[1:]) and 'unknown' not in key[0]
        unstable = any(int(r['late']) or int(r['missed']) for r in rows)
        headroom = all(float(r['work_worst_ms']) <= (1000 / float(r.get('target_fps', 60))) * .8 for r in rows)
        lost = any(int(r.get('lost_segments', '0')) or int(r.get('skipped_samples', '0')) for r in rows)
        result = ('Unknown clocks/mode' if not known else 'Frame unstable' if unstable else
                  'Headroom fail' if not headroom else 'Incomplete' if missing or lost else 'Candidate; manual checks pending')
        results[key] = result
        print(f'{key[0]:28} {key[1]/1e6:7.1f} {key[2]/1e6:7.1f} {key[3]/1e6:7.1f}  {result}')
        if missing:
            print('  Needs sustained coverage: ' + ', '.join(missing))
        if lost:
            print('  Incomplete telemetry; repeat with no lost/skipped samples')
    for key, result in results.items():
        if not result.startswith('Candidate'):
            continue
        print(f'Boundary evidence for {key[0]} {tuple(hz/1e6 for hz in key[1:])} MHz:')
        for axis, label in enumerate(('CPU', 'GPU', 'EMC'), 1):
            lower = [k for k in results if k[0] == key[0] and k[axis] < key[axis]
                     and all(k[i] == key[i] for i in range(1, 4) if i != axis)]
            if lower:
                nearest = max(lower, key=lambda k: k[axis])
                print(f'  Nearest measured lower {label}: {nearest[axis]/1e6:.1f} MHz: {results[nearest]}')
            else:
                print(f'  Lower {label}: not measured; boundary unknown')
    print('GPU timing/audio underruns unavailable. Confirm audio, input, stutter, max-player lobby,')
    print('score bursts, discovery and rapid transitions manually before calling a candidate stable.')


if __name__ == '__main__':
    main()
