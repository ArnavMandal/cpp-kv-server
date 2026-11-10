"""Repeatable throughput/latency benchmark for kvserver_multi (Lesson 6).

Run: python3 bench/run_bench.py --server ./kvserver_multi --client ./kvbench

For every persistence mode it starts a fresh server with an empty log on an
OS-selected port, lets bench/kvbench drive it, and stops it again. Modes are
interleaved (A B C D, A B C D, ...) so slow drift on the machine, such as
heating up or a background job, spreads over every mode instead of one.

Results go to bench/results/<timestamp>.json, together with what is needed to
reproduce them: parameters, binary hashes, git commit, OS, and compiler.
Uses only Python's standard library.
"""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import re
import select
import statistics
import subprocess
import sys
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]
MODES = {
    'memory': [],                                   # No log: the upper bound.
    'aof-no': ['--fsync', 'no'],                    # Write to the OS; it flushes later.
    'aof-everysec': ['--fsync', 'everysec'],        # fsync at most once per second.
    'aof-always': ['--fsync', 'always'],            # fsync before every OK.
}
# Never auto-compact during a run: a fork() mid-measurement would be noise
# that depends on log size, not on the fsync policy being measured.
NO_COMPACTION = str(2**62)


def start_server(server, mode, aof):
    command = [server, '0']
    if mode != 'memory':
        command += ['--aof', str(aof), *MODES[mode], '--compact-min', NO_COMPACTION]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               bufsize=0)
    while True:
        if not select.select([process.stdout], [], [], 5)[0]:
            process.kill()
            raise RuntimeError('server startup timed out')
        line = process.stdout.readline()
        if not line:
            raise RuntimeError(f'server exited: {process.wait()} {process.stderr.read()!r}')
        match = re.match(rb'Listening on 127\.0\.0\.1:(\d+) ', line)
        if match:
            break
    # Keep draining output so a full pipe can never stall the server.
    for stream in (process.stdout, process.stderr):
        threading.Thread(target=stream.read, daemon=True).start()
    return process, int(match.group(1))


def run_once(args, mode, directory):
    aof = pathlib.Path(directory) / f'{mode}.aof'
    aof.unlink(missing_ok=True)
    process, port = start_server(args.server, mode, aof)
    try:
        result = subprocess.run(
            [args.client, '--port', str(port), '--clients', str(args.clients),
             '--ops', str(args.ops), '--warmup', str(args.warmup), '--keys', str(args.keys),
             '--value-size', str(args.value_size), '--get-percent', str(args.get_percent),
             '--seed', str(args.seed)],
            capture_output=True, text=True, timeout=args.timeout)
    finally:
        process.terminate()
        process.wait()
        aof.unlink(missing_ok=True)
    if result.returncode != 0:
        raise RuntimeError(f'kvbench failed ({mode}): {result.stderr.strip()} {result.stdout}')
    return json.loads(result.stdout)


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def command_output(command):
    try:
        return subprocess.run(command, capture_output=True, text=True, cwd=ROOT,
                              timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def environment(args, directory):
    compiler = command_output(['c++', '--version'])
    return {
        'timestamp': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
        'git_commit': command_output(['git', 'rev-parse', 'HEAD']),
        'git_dirty': bool(command_output(['git', 'status', '--porcelain'])),
        'os': platform.platform(),
        'machine': platform.machine(),
        'cpu_count': os.cpu_count(),
        'cpu_model': command_output(['sysctl', '-n', 'machdep.cpu.brand_string'])
                     if sys.platform == 'darwin' else _linux_cpu_model(),
        'compiler': compiler.splitlines()[0] if compiler else None,
        'server': {'path': args.server, 'sha256': sha256(args.server)},
        'client': {'path': args.client, 'sha256': sha256(args.client)},
        'log_directory': str(directory),
        'note': args.note,
    }


def _linux_cpu_model():
    try:
        for line in pathlib.Path('/proc/cpuinfo').read_text().splitlines():
            if line.startswith('model name'):
                return line.split(':', 1)[1].strip()
    except OSError:
        pass
    return None


def summarize(runs):
    """Medians across repeats, plus spread so you can see how repeatable they are."""
    throughput = [r['ops_per_sec'] for r in runs]
    median = statistics.median(throughput)
    summary = {
        'ops_per_sec_median': round(median, 1),
        'ops_per_sec_min': round(min(throughput), 1),
        'ops_per_sec_max': round(max(throughput), 1),
        # (max - min) / median. Large values mean the machine was not quiet.
        'ops_per_sec_spread_pct': round(100 * (max(throughput) - min(throughput)) / median, 1),
    }
    for kind in ('get', 'set'):
        for stat in ('p50_us', 'p99_us', 'p999_us', 'max_us'):
            summary[f'{kind}_{stat}_median'] = round(
                statistics.median(r[kind][stat] for r in runs), 2)
    return summary


def print_table(summary):
    header = (f"{'mode':<14}{'ops/s (median)':>16}{'spread':>9}"
              f"{'GET p50':>10}{'GET p99':>10}{'SET p50':>10}{'SET p99':>10}{'SET p99.9':>11}")
    print(header)
    print('-' * len(header))
    for mode, s in summary.items():
        print(f"{mode:<14}{s['ops_per_sec_median']:>16,.0f}{s['ops_per_sec_spread_pct']:>8.1f}%"
              f"{s['get_p50_us_median']:>10.1f}{s['get_p99_us_median']:>10.1f}"
              f"{s['set_p50_us_median']:>10.1f}{s['set_p99_us_median']:>10.1f}"
              f"{s['set_p999_us_median']:>11.1f}")
    print('Latencies are microseconds, medians across repeats.')


def print_comparison(report, baseline_path):
    """Percent change of each median against an earlier result file."""
    baseline = json.loads(pathlib.Path(baseline_path).read_text())
    ignore = {'repeats', 'modes'}  # These may differ; the workload must not.
    old_p = {k: v for k, v in baseline['parameters'].items() if k not in ignore}
    new_p = {k: v for k, v in report['parameters'].items() if k not in ignore}
    if old_p != new_p:
        print(f'\nNot comparable: workload parameters differ.\n  old: {old_p}\n  new: {new_p}')
        return
    print(f'\nChange vs {baseline_path} (negative latency change = faster):')
    for mode, new in report['summary'].items():
        old = baseline['summary'].get(mode)
        if old is None:
            continue
        def change(key):
            return 100 * (new[key] - old[key]) / old[key] if old[key] else float('nan')
        print(f"  {mode:<14} ops/s {change('ops_per_sec_median'):+6.1f}%   "
              f"SET p99 {change('set_p99_us_median'):+6.1f}%   "
              f"GET p99 {change('get_p99_us_median'):+6.1f}%")
    print('  Treat changes smaller than the spread column as noise.')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--server', default=str(ROOT / 'kvserver_multi'))
    parser.add_argument('--client', default=str(ROOT / 'kvbench'))
    parser.add_argument('--modes', default=','.join(MODES),
                        help='comma-separated subset of: ' + ', '.join(MODES))
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--clients', type=int, default=4)
    parser.add_argument('--ops', type=int, default=200000)
    parser.add_argument('--warmup', type=int, default=2000)
    parser.add_argument('--keys', type=int, default=10000)
    parser.add_argument('--value-size', type=int, default=64)
    parser.add_argument('--get-percent', type=int, default=50)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--dir', help='where to put the logs (default: a temporary directory); '
                                      'the disk under it is part of what you measure')
    parser.add_argument('--out', help='result file (default: bench/results/<timestamp>.json)')
    parser.add_argument('--note', default='', help='free text saved with the results, '
                                                   'e.g. how the binaries were compiled')
    parser.add_argument('--compare', help='earlier result file to compare against')
    parser.add_argument('--timeout', type=float, default=300, help='seconds per run')
    parser.add_argument('--quick', action='store_true',
                        help='tiny run to check that everything works; numbers are meaningless')
    args = parser.parse_args()
    if args.quick:
        args.repeats, args.ops, args.warmup, args.keys = 1, 2000, 200, 500
    modes = args.modes.split(',')
    unknown = [m for m in modes if m not in MODES]
    if unknown or args.repeats < 1:
        parser.error(f'unknown modes {unknown}' if unknown else '--repeats must be >= 1')
    args.server = str(pathlib.Path(args.server).resolve())
    args.client = str(pathlib.Path(args.client).resolve())

    with tempfile.TemporaryDirectory(dir=args.dir) as directory:
        report = {
            'environment': environment(args, directory),
            'parameters': {k: getattr(args, k) for k in (
                'repeats', 'clients', 'ops', 'warmup', 'keys', 'value_size',
                'get_percent', 'seed', 'quick')} | {'modes': modes},
            'runs': {m: [] for m in modes},
        }
        for repeat in range(args.repeats):
            for mode in modes:
                result = run_once(args, mode, directory)
                # 1-minute load average: a high value explains a slow outlier run.
                result['load_average'] = round(os.getloadavg()[0], 2)
                report['runs'][mode].append(result)
                print(f'[{repeat + 1}/{args.repeats}] {mode:<13} '
                      f"{result['ops_per_sec']:>10,.0f} ops/s  "
                      f"SET p99 {result['set']['p99_us']:.1f} us", flush=True)

    report['summary'] = {m: summarize(runs) for m, runs in report['runs'].items()}
    out = pathlib.Path(args.out) if args.out else (
        ROOT / 'bench' / 'results'
        / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '.json'))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + '\n')
    print()
    print_table(report['summary'])
    if args.compare:
        print_comparison(report, args.compare)
    noisy = [m for m, s in report['summary'].items() if s['ops_per_sec_spread_pct'] > 10]
    if noisy and args.repeats > 1:
        print(f"\nWarning: throughput varied more than 10% across repeats for {', '.join(noisy)}. "
              'Close other programs, plug in power, and run again before comparing.')
    print(f'\nSaved {out}')
    if report['environment']['git_dirty']:
        print('Warning: uncommitted changes; git_commit does not fully describe this build.')


if __name__ == '__main__':
    main()
