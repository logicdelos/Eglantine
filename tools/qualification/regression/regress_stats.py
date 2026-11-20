#!/usr/bin/env python3
"""Regression statistics for the 0.6 frame-rate check (slice E).

WHAT IT DECIDES
  Whether a candidate build is slower than a baseline build beyond measured noise, on the median
  frame rate and on the low end (inverse p99 frame time), per scene, from paired A/B runs; and
  whether the check itself works (self-proof). Contract: plans/contracts_0.6.md (slice E) and
  regress_contract.py (constants, exit codes, result schema). Pure Python, no SciPy, no lab
  imports, no emulator.

METHOD
  Per scene s and metric m (median_fps, inverse_p99; both "higher is better") each run gives one
  value. A pair is a baseline run A and a candidate run B taken back to back, in alternating
  order. d = ln(A / B) is the log loss of the candidate: positive when B is slower.
    d_bar = mean(d), s_obs = sample standard deviation of d over n pairs,
    c     = Student-t critical value, upper tail alpha_test, df degrees of freedom,
    h     = c * s / sqrt(n),  lower = d_bar - h,  upper = d_bar + h,
    mde   = (c + Z_BETA) * s / sqrt(n)   (smallest log loss detected with 95 % power).
  There is no calibration stage: s is the spread of the pairs of the run itself (df = n - 1).
  A stored A/A calibration of the same build (>= 30 pairs, optional) may tighten it: when the
  pairs are not noisier than the calibration (F test, 1 %), the variances are pooled and
  df = n + m - 2; otherwise the run's own spread is used.
  FAIL when lower > 0 for any metric (the loss is proven positive). PASS needs, for the median
  frame rate of every scene, mde <= DELTA = -ln(0.95) (the run could have detected a 5 % loss) and
  upper < DELTA (a loss of 5 % or more is rejected); and no FAIL anywhere. Otherwise INCONCLUSIVE,
  with the number of pairs that would be needed.
  Low end (inverse_p99): it does not gate PASS (p99 sampling noise is larger and the number of
  pairs is capped). A significant low-end loss still FAILs. For every scene the result reports
  "low-end drops beyond X % would have been detected", X being its achieved sensitivity.

SEQUENTIAL DESIGN AND ALPHA SPENDING
  Pairs are taken in rounds. The manifest's design block gives min_pairs (4) and max_pairs (10);
  a look is made after every round from min_pairs on: looks = max_pairs - min_pairs + 1 (7).
  alpha_test = ALPHA_FAMILY / (K * looks), K = scenes x metrics. This Bonferroni spending is
  valid for any data-dependent stopping rule (union bound), so stopping early on a decided scene
  cannot inflate the family-wise false-alarm probability above ALPHA_FAMILY. Price: a larger
  critical value at every look than a single fixed-n test. A manifest without a design block is
  one fixed-sample look at its n pairs (looks = 1, at least C.MIN_PAIRS pairs).
  Stopping rule (applied by the driver with scene_decided): a scene stops when its median row has
  its answer (check: PASS or FAIL; self-proof: A/A PASS and injection caught or missed); any FAIL
  stops the run; at max_pairs a scene with no answer is INCONCLUSIVE and the pairs needed are
  reported.

SELF-PROOF (mode self_proof)
  Per round and scene a triplet is run: baseline A1, baseline again A2, baseline with the
  injected delay I (balanced rotating order). Two comparisons share A1:
    A/A:       d_AA  = ln A1 - ln A2   (must NOT alarm: the false-alarm check),
    injection: d_inj = ln A1 - ln I    (must alarm: the 5 % slowdown is caught).
  Dependence: d_AA and d_inj are positively correlated (0.5 when the three run errors are i.i.d.)
  because they share A1. Each comparison is tested on its own sample of i.i.d. rounds, so each
  test's size and power are exact marginally; the shared run does not change them. The proof
  succeeds only when A/A does not alarm AND the injection alarms, and its failure probability is
  bounded by P(A/A false alarm) + P(miss) whatever the dependence; the positive correlation can
  only make the two errors co-occur more, which makes the joint failure no more likely than the
  sum. The two verdicts are reported separately (sections aa and injection of the result); the
  alpha spending above covers every test of both. The injected loss is estimated from the
  injection rounds themselves (1 - exp(-d_bar), 95 % t interval); the proof is valid when the
  median estimate lies in 4.5-5.5 % (point_in_band) or its interval covers 5 % and lies within
  4-6 % (interval_in_tolerance); the result says which. One retune of the delay is advised when
  the interval lies off target after 3-6 rounds (retune_delay).
  Verdict: PASS = proved (no A/A false alarm, injection caught, valid); FAIL = A/A false alarm;
  INCONCLUSIVE = not proved (missed, invalid, no power, missing data).

PASS WORDING
  PASS: "Over N paired runs per scene, a 5 % median frame-rate loss was ruled out with 99 %
  family-wise confidence in every checked scene. Low-end (1 % low) drops beyond the stated
  sensitivity would have been detected."

INPUT
  A runs manifest (JSON, schema 1, kind regression_runs) lists runs; each names a telemetry file
  (a PresentMon CSV or an emulator --frame-times JSONL log, read strictly here) or carries inline
  frame times marked synthetic. Synthetic runs only test the logic: the result says so
  ("evidence": "synthetic") and must never be shown as acceptance evidence. Entries flagged
  discarded are skipped and counted (a restarted injection arm).

USAGE
  python regress_stats.py --runs runs.json [--calibration cal.json] [--out result.json]
  python regress_stats.py --runs runs.json --calibrate --out cal.json
  python regress_stats.py --validate-frame-times a.jsonl [b.jsonl ...]

EXIT CODES (regress_contract)
  0 PASS, 1 FAIL (a measured regression, or an A/A false alarm in a self-proof), 2 bad arguments
  or malformed input (INVALID), 3 INCONCLUSIVE, 4 tool failure, 130 interrupted.
"""

import argparse
import csv
import functools
import json
import math
import os
import random
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import regress_contract as C  # noqa: E402

TOOL = 'regress_stats.py'
MANIFEST_SCHEMA = 1
MANIFEST_KIND = 'regression_runs'
CALIBRATION_SCHEMA = 1
CALIBRATION_KIND = 'regression_calibration'
METRICS = tuple(C.METRICS_DEFAULT)
GATING_METRIC = 'median_fps'     # the only metric whose sensitivity gates PASS
MIN_FRAMES = 300                 # frames a run needs before its p99 means anything
BAD_ROW_FRACTION = 0.005         # unparsable PresentMon rows tolerated before the file is invalid
ROLE_BASELINE = 'baseline'
ROLE_CANDIDATE = 'candidate'
ROLE_CAL_A = 'calibration_a'
ROLE_CAL_B = 'calibration_b'
ROLE_REPEAT = 'baseline_repeat'  # A2 of a self-proof triplet
ROLE_INJECTED = 'injected'       # I of a self-proof triplet (baseline build + injected delay)
ROLES = (ROLE_BASELINE, ROLE_CANDIDATE, ROLE_CAL_A, ROLE_CAL_B, ROLE_REPEAT, ROLE_INJECTED)
FORMATS = ('presentmon', 'frame_times')
EVIDENCE_MEASURED = 'measured'
EVIDENCE_SYNTHETIC = 'synthetic'
BIAS_ALPHA = 0.01                # two-sided, per scene and metric, on the A/A mean
CAL_COMPAT_ALPHA = 0.01          # F test: are these pairs noisier than the stored calibration?
MODE_CHECK = 'check'
MODE_SELF_PROOF = 'self_proof'
MODES = (MODE_CHECK, MODE_SELF_PROOF)
SEQ_MIN_PAIRS = 4                # first look of the sequential design
SEQ_MAX_PAIRS = 10               # cap: the last look
CI_ALPHA = 0.05                  # two-sided level of the achieved-loss interval
INJECTION_TOLERANCE = (0.04, 0.06)
RESTART_WINDOW = (3, 6)          # injection pairs at which a retune may be advised, once
RESTART_POINT_FROM = 4           # from this pair on a point estimate outside the band also advises it
VALID_POINT = 'point_in_band'              # achieved median loss within 4.5 to 5.5 %
VALID_INTERVAL = 'interval_in_tolerance'   # its 95 % interval covers 5 % and lies in 4 to 6 %
VALID_OUTSIDE = 'outside'
VALID_NONE = 'no_data'
SPENDING_RULE = (
    'Bonferroni alpha spending: the family alpha is split equally over the K scene-metric tests '
    'and over every look (one per pair count from min_pairs to max_pairs). Each test at each '
    'look uses the one-sided t critical value of alpha / (K * looks). By the union bound the '
    'chance that any test at any look declares a false loss is at most alpha, whatever the '
    'stopping rule and whatever the dependence between the tests.')


class TelemetryError(Exception):
    """A telemetry input that cannot be used. kind: invalid (malformed or truncated), missing."""

    def __init__(self, message, kind='invalid'):
        super().__init__(message)
        self.kind = kind


class ManifestError(Exception):
    """The runs manifest or calibration document itself is malformed."""


# ----------------------------------------------------------------------------- Student t

def _betacf(a, b, x):
    tiny = 1e-300
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    d = 1.0 / (d if abs(d) > tiny else tiny)
    h = d
    for m in range(1, 1000):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) > tiny else tiny
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) > tiny else tiny
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 1e-15:
            return h
    raise ArithmeticError('incomplete beta did not converge')


def _betai(a, b, x):
    """Regularised incomplete beta function I_x(a, b)."""
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    front = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
                     + a * math.log(x) + b * math.log1p(-x))
    if x < (a + 1.0) / (a + b + 2.0):
        return front * _betacf(a, b, x) / a
    return 1.0 - front * _betacf(b, a, 1.0 - x) / b


def t_upper_tail(t, df):
    """P(T > t) for Student's t with df degrees of freedom."""
    if df <= 0:
        raise ValueError('degrees of freedom must be positive')
    tail = 0.5 * _betai(df / 2.0, 0.5, df / (df + t * t))
    return tail if t >= 0 else 1.0 - tail


@functools.lru_cache(maxsize=None)
def t_critical(alpha, df):
    """The t with upper-tail probability alpha (one-sided critical value), exact to ~1e-12."""
    if not 0.0 < alpha < 0.5:
        raise ValueError('alpha must be in (0, 0.5)')
    low, high = 0.0, 1.0
    while t_upper_tail(high, df) > alpha:
        low, high = high, high * 2.0
        if high > 1e15:
            raise ArithmeticError('critical value out of range')
    for _ in range(200):
        mid = 0.5 * (low + high)
        if t_upper_tail(mid, df) > alpha:
            low = mid
        else:
            high = mid
    return 0.5 * (low + high)


@functools.lru_cache(maxsize=None)
def f_critical(alpha, df1, df2):
    """The F with upper-tail probability alpha, df1 and df2 degrees of freedom."""
    if not 0.0 < alpha < 1.0 or df1 <= 0 or df2 <= 0:
        raise ValueError('alpha must be in (0, 1) and the degrees of freedom positive')
    low, high = 0.0, 1.0
    while _betai(df2 / 2.0, df1 / 2.0, df2 / (df2 + df1 * high)) > alpha:
        low, high = high, high * 2.0
        if high > 1e15:
            raise ArithmeticError('critical value out of range')
    for _ in range(200):
        mid = 0.5 * (low + high)
        if _betai(df2 / 2.0, df1 / 2.0, df2 / (df2 + df1 * mid)) > alpha:
            low = mid
        else:
            high = mid
    return 0.5 * (low + high)


def family_critical(k, n, looks=1):
    """Critical value of one test at one look: alpha over K tests and `looks` looks, n - 1 df."""
    return t_critical(C.ALPHA_FAMILY / (k * looks), n - 1)


@functools.lru_cache(maxsize=None)
def _pairs_needed(spread, k, looks, delta, z_beta, start, limit):
    n = max(2, start)
    while n < limit:
        if (family_critical(k, n, looks) + z_beta) * spread / math.sqrt(n) <= delta:
            return n
        n += 1
    return None


def pairs_needed(spread, k, looks=1, delta=C.DELTA, z_beta=C.Z_BETA, start=C.MIN_PAIRS,
                 limit=5000):
    """Smallest n >= start with (c + z_beta) * spread / sqrt(n) <= delta, c the critical value
    of one test at one of `looks` looks. None when it needs `limit` pairs or more."""
    return _pairs_needed(round(spread, 6), k, looks, delta, z_beta, start, limit)


# ----------------------------------------------------------------------------- frame data

def percentile(values, q):
    ordered = sorted(values)
    if not ordered:
        raise ValueError('no values')
    k = (len(ordered) - 1) * q / 100.0
    low = int(k)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (k - low)


def run_metrics(frame_ms):
    """median_fps and inverse_p99 of one run's frame times in milliseconds."""
    if len(frame_ms) < MIN_FRAMES:
        raise TelemetryError(f'only {len(frame_ms)} frames; at least {MIN_FRAMES} are needed',
                             'short')
    for value in frame_ms:
        if not (isinstance(value, (int, float)) and math.isfinite(value) and value > 0):
            raise TelemetryError('a frame time is not a positive finite number', 'invalid')
    return dict(frames=len(frame_ms),
                median_fps=1000.0 / statistics.median(frame_ms),
                inverse_p99=1000.0 / percentile(frame_ms, 99))


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def read_frame_times(path):
    """Strict reader of an emulator --frame-times log (Common::FrameTiming, frame_timing_log.h).

    Format, UTF-8 JSON Lines: a header line, one {"id","t"} line per frame, and an end line
    {"end":true,"recorded","lost","complete"} that must be the last line. A file without the end
    line, without its final newline, with a damaged line, with counts that disagree, or with
    times that go backwards is invalid (TelemetryError kind invalid); a missing file is kind
    missing. Returns dict(source, capacity, ids, times_ns, recorded, lost, complete).
    """
    try:
        with open(path, 'rb') as handle:
            raw = handle.read()
    except FileNotFoundError:
        raise TelemetryError(f'{os.path.basename(path)}: file not found', 'missing') from None
    except OSError as exc:
        raise TelemetryError(f'{os.path.basename(path)}: unreadable ({exc.strerror})',
                             'missing') from None
    name = os.path.basename(path)

    def bad(why):
        return TelemetryError(f'{name}: {why}', 'invalid')

    if not raw:
        raise bad('empty file')
    if not raw.endswith(b'\n'):
        raise bad('truncated: the last line is not complete')
    try:
        lines = raw.decode('utf-8').split('\n')[:-1]
    except UnicodeDecodeError:
        raise bad('not UTF-8') from None
    if len(lines) < 2:
        raise bad('truncated: no end line')

    def parse(line, number):
        try:
            value = json.loads(line)
        except ValueError:
            raise bad(f'line {number} is not JSON') from None
        if not isinstance(value, dict):
            raise bad(f'line {number} is not an object')
        return value

    header = parse(lines[0], 1)
    if set(header) != {'schema', 'kind', 'source', 'clock', 'capacity'}:
        raise bad('header has unexpected keys')
    if header['schema'] != C.FRAME_TIMES_SCHEMA or header['kind'] != 'frame_times':
        raise bad('not a frame_times schema 1 log')
    if header['source'] not in C.FRAME_TIMES_SOURCES:
        raise bad('unknown source')
    if header['clock'] != 'steady_ns':
        raise bad('unknown clock')
    if not _is_int(header['capacity']) or header['capacity'] <= 0:
        raise bad('bad capacity')

    ids, times = [], []
    last = 0
    for number, line in enumerate(lines[1:-1], start=2):
        sample = parse(line, number)
        if set(sample) != {'id', 't'} or not _is_int(sample['id']) or not _is_int(sample['t']):
            raise bad(f'line {number} is not an id/t sample')
        if sample['id'] < 0 or sample['t'] <= 0 or sample['t'] < last:
            raise bad(f'line {number} has an impossible id or a time that goes backwards')
        last = sample['t']
        ids.append(sample['id'])
        times.append(sample['t'])
    end = parse(lines[-1], len(lines))
    if set(end) != {'end', 'recorded', 'lost', 'complete'} or end['end'] is not True:
        raise bad('truncated: the last line is not the end line')
    if not _is_int(end['recorded']) or not _is_int(end['lost']) or end['recorded'] < 0 \
            or end['lost'] < 0 or not isinstance(end['complete'], bool):
        raise bad('end line has bad values')
    if end['recorded'] != len(times):
        raise bad(f'end line says {end["recorded"]} samples but the file holds {len(times)}')
    if end['recorded'] > header['capacity'] or (end['lost'] > 0 and
                                                 end['recorded'] != header['capacity']):
        raise bad('sample counts do not fit the capacity')
    if end['complete'] and end['lost'] != 0:
        raise bad('marked complete although samples were lost')
    return dict(source=header['source'], capacity=header['capacity'], ids=ids, times_ns=times,
                recorded=end['recorded'], lost=end['lost'], complete=end['complete'])


def frame_ms_from_times(times_ns):
    return [(b - a) / 1e6 for a, b in zip(times_ns, times_ns[1:]) if b > a]


def read_presentmon(path, window=None):
    """Frame times (ms) of a PresentMon CSV, optionally only rows with low <= time < high."""
    name = os.path.basename(path)
    try:
        handle = open(path, encoding='utf-8-sig', newline='')
    except OSError:
        raise TelemetryError(f'{name}: file not found', 'missing') from None
    with handle:
        reader = csv.DictReader(handle)
        names = {(column or '').lower(): column for column in (reader.fieldnames or [])}
        time_name = next((key for key in ('timeinms', 'timeinseconds') if key in names), None)
        if time_name is None or 'msbetweenpresents' not in names:
            raise TelemetryError(f'{name}: no time or MsBetweenPresents column', 'invalid')
        scale = 0.001 if time_name == 'timeinms' else 1.0
        rows, bad, total = [], 0, 0
        try:
            for row in reader:
                total += 1
                try:
                    t = float(row[names[time_name]]) * scale
                    ms = float(row[names['msbetweenpresents']])
                except (TypeError, ValueError):
                    bad += 1
                    continue
                if not (math.isfinite(t) and math.isfinite(ms)):
                    bad += 1
                    continue
                if ms > 0:
                    rows.append((t, ms))
        except csv.Error as exc:
            raise TelemetryError(f'{name}: malformed CSV ({exc})', 'invalid') from None
    if total == 0:
        raise TelemetryError(f'{name}: no data rows', 'invalid')
    if bad > BAD_ROW_FRACTION * total:
        raise TelemetryError(f'{name}: {bad} of {total} rows are not numbers', 'invalid')
    if window is not None:
        low, high = window
        rows = [row for row in rows if low <= row[0] < high]
    return [ms for _, ms in rows]


# ----------------------------------------------------------------------------- manifest

def _resolve(path, base_dir):
    return path if os.path.isabs(path) else os.path.join(base_dir, path)


def load_json(path, what):
    try:
        with open(path, encoding='utf-8') as handle:
            return json.load(handle)
    except FileNotFoundError:
        raise ManifestError(f'{what} not found') from None
    except (OSError, ValueError) as exc:
        raise ManifestError(f'{what} is unreadable or not JSON: {exc}') from None


def check_manifest(manifest):
    """Raise ManifestError when the manifest cannot be interpreted at all."""
    if not isinstance(manifest, dict):
        raise ManifestError('the runs manifest is not an object')
    if manifest.get('schema') != MANIFEST_SCHEMA or manifest.get('kind') != MANIFEST_KIND:
        raise ManifestError(f'not a {MANIFEST_KIND} schema {MANIFEST_SCHEMA} manifest')
    scenes = manifest.get('scenes')
    if not isinstance(scenes, list) or not scenes or len(set(scenes)) != len(scenes) \
            or not all(isinstance(item, str) and item for item in scenes):
        raise ManifestError('scenes must be a non-empty list of distinct names')
    for key in ('baseline_build', 'candidate_build'):
        if not isinstance(manifest.get(key), str) or not manifest[key]:
            raise ManifestError(f'{key} is required')
    runs = manifest.get('runs')
    if not isinstance(runs, list):
        raise ManifestError('runs must be a list')
    seen = set()
    for entry in runs:
        if not isinstance(entry, dict):
            raise ManifestError('a run entry is not an object')
        role, scene, pair = entry.get('role'), entry.get('scene'), entry.get('pair')
        if role not in ROLES or not isinstance(scene, str) or not _is_int(pair) or pair < 1:
            raise ManifestError(f'run {entry.get("id")!r} needs a known role, a scene and a '
                                f'pair number >= 1')
        if not entry.get('discarded'):
            if (role, scene, pair) in seen:
                raise ManifestError(f'two runs claim {role} pair {pair} of scene {scene}')
            seen.add((role, scene, pair))
        has_file = isinstance(entry.get('file'), str)
        has_inline = 'frame_ms' in entry
        if has_file == has_inline:
            raise ManifestError(f'run {entry.get("id")!r} needs exactly one of file or frame_ms')
        if has_file and entry.get('format') not in FORMATS:
            raise ManifestError(f'run {entry.get("id")!r} has an unknown format')
        if has_inline and entry.get('synthetic') is not True:
            raise ManifestError(f'run {entry.get("id")!r}: inline frame times are only accepted '
                                f'for synthetic runs')
    mode = manifest.get('mode', MODE_CHECK)
    if mode not in MODES:
        raise ManifestError(f'mode must be one of {", ".join(MODES)}')
    design = manifest.get('design')
    if design is not None:
        if not (isinstance(design, dict) and _is_int(design.get('min_pairs'))
                and _is_int(design.get('max_pairs')) and 2 <= design['min_pairs']
                <= design['max_pairs'] <= 1000):
            raise ManifestError('design needs integer min_pairs <= max_pairs, both >= 2')
    injection = manifest.get('injection')
    if mode == MODE_SELF_PROOF and injection is None:
        raise ManifestError('a self_proof manifest needs an injection block')
    if injection is not None:
        delays = injection.get('delays') if isinstance(injection, dict) else None
        single = isinstance(injection, dict) and _is_int(injection.get('delay_ns')) \
            and injection['delay_ns'] > 0
        per_scene = isinstance(delays, dict) and bool(delays) and all(
            isinstance(item, dict) and _is_int(item.get('delay_ns')) and item['delay_ns'] > 0
            for item in delays.values())
        if not (isinstance(injection, dict)
                and injection.get('target') in ('cpu-present', 'gpu-frame')
                and (single or per_scene)):
            raise ManifestError('injection needs a target and a positive delay_ns (or a '
                                'delays map of scene to delay_ns)')


GENERATED_SOURCES = ('generated_frames', 'frame_gen', 'lossless_scaling')


def _is_truthy_setting(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in ('true', '1', 'yes', 'on')
    return False


def _validate_run_metadata(entry, base_dir):
    """Reject incomplete metrics/shutdown and generated-frame inflation before scoring."""
    if entry.get('metrics_complete') is False:
        raise TelemetryError('metrics_complete is false', 'incomplete')
    if entry.get('shutdown_complete') is False:
        raise TelemetryError('shutdown_complete is false', 'incomplete')
    if entry.get('frame_gen') is True:
        raise TelemetryError('generated-frame inflation: frame_gen is true', 'integrity')
    gen_count = entry.get('generated_frames')
    if isinstance(gen_count, (int, float)) and not isinstance(gen_count, bool) and gen_count > 0:
        raise TelemetryError(f'generated-frame inflation: generated_frames={gen_count}',
                             'integrity')
    mult = entry.get('frame_gen_multiplier')
    if isinstance(mult, (int, float)) and not isinstance(mult, bool) and mult > 1:
        raise TelemetryError(f'generated-frame inflation: frame_gen_multiplier={mult}',
                             'integrity')
    if entry.get('source') in GENERATED_SOURCES:
        raise TelemetryError(f'generated-frame inflation: source {entry.get("source")!r}',
                             'integrity')
    report_ref = entry.get('report')
    if report_ref is None:
        return
    if not isinstance(report_ref, str) or not report_ref:
        raise TelemetryError('report path must be a non-empty string', 'invalid')
    report_path = _resolve(report_ref, base_dir)
    try:
        with open(report_path, encoding='utf-8') as handle:
            report = json.load(handle)
    except FileNotFoundError:
        raise TelemetryError(f'{os.path.basename(report_path)}: report not found', 'missing') from None
    except (OSError, ValueError) as exc:
        raise TelemetryError(f'{os.path.basename(report_path)}: unreadable or not JSON ({exc})',
                             'invalid') from None
    if not isinstance(report, dict):
        raise TelemetryError(f'{os.path.basename(report_path)}: report is not an object', 'invalid')
    exit_info = report.get('exit')
    if not isinstance(exit_info, dict):
        raise TelemetryError(f'{os.path.basename(report_path)}: report missing exit block',
                             'incomplete')
    if exit_info.get('metrics_complete') is not True:
        raise TelemetryError('report exit.metrics_complete is not true', 'incomplete')
    if exit_info.get('shutdown_complete') is not True:
        raise TelemetryError('report exit.shutdown_complete is not true', 'incomplete')
    settings = report.get('settings')
    if isinstance(settings, dict):
        for phase in ('initial', 'final'):
            block = settings.get(phase)
            if isinstance(block, dict) and _is_truthy_setting(block.get('frame_gen')):
                raise TelemetryError(
                    f'generated-frame inflation: report settings.{phase}.frame_gen is enabled',
                    'integrity')
    summary = report.get('summary')
    if isinstance(summary, dict):
        rep_gen = summary.get('generated_frames')
        if isinstance(rep_gen, (int, float)) and not isinstance(rep_gen, bool) and rep_gen > 0:
            raise TelemetryError(
                f'generated-frame inflation: report summary.generated_frames={rep_gen}',
                'integrity')


def load_run(entry, base_dir):
    """Telemetry of one run: dict(metrics, source, complete, synthetic); TelemetryError if not."""
    if entry.get('ok') is False:
        raise TelemetryError('the run failed: ' + '; '.join(entry.get('failures') or ['no detail'])
                             [:200], 'missing')
    _validate_run_metadata(entry, base_dir)
    if 'frame_ms' in entry:
        frames = entry['frame_ms']
        if not isinstance(frames, list):
            raise TelemetryError('frame_ms is not a list', 'invalid')
        return dict(metrics=run_metrics(frames), source=entry.get('source', 'synthetic'),
                    complete=True, synthetic=True, lost=0)
    path = _resolve(entry['file'], base_dir)
    if entry['format'] == 'frame_times':
        log = read_frame_times(path)
        expected = entry.get('source')
        if expected and expected != log['source']:
            raise TelemetryError(f'{os.path.basename(path)}: the log source is {log["source"]}, '
                                 f'the manifest says {expected}', 'invalid')
        frames = frame_ms_from_times(log['times_ns'])
        return dict(metrics=run_metrics(frames), source=log['source'], complete=log['complete'],
                    synthetic=entry.get('synthetic') is True, lost=log['lost'])
    window = entry.get('window')
    if window is not None and not (isinstance(window, list) and len(window) == 2):
        raise TelemetryError('window must be [low, high]', 'invalid')
    frames = read_presentmon(path, tuple(window) if window else None)
    return dict(metrics=run_metrics(frames), source='presentmon', complete=True,
                synthetic=entry.get('synthetic') is True, lost=0)


class Evidence:
    """Everything loaded from a manifest, with the reasons something could not be used."""

    def __init__(self):
        self.invalid = []          # malformed telemetry: the verdict becomes INVALID
        self.integrity = []        # incomparable or mislabelled telemetry: INCONCLUSIVE
        self.coverage = []         # missing or short telemetry: never PASS
        self.pairs = {}            # (family, scene) -> list of (pair_no, a_metrics, b_metrics)
        self.synthetic = False
        self.measured = False
        self.discarded = {}        # scene -> runs discarded by a restart


def _note(entry, why):
    return f'{entry.get("id") or (entry["role"] + " " + entry["scene"] + " p" + str(entry["pair"]))}: {why}'


def gather(manifest, base_dir):
    check_manifest(manifest)
    ev = Evidence()
    loaded = {}
    gone = set()
    live = []
    for entry in manifest['runs']:
        if entry.get('discarded'):
            ev.discarded[entry['scene']] = ev.discarded.get(entry['scene'], 0) + 1
            gone.add((entry['role'], entry['scene'], entry['pair']))
        else:
            live.append(entry)
    for entry in live:
        key = (entry['role'], entry['scene'], entry['pair'])
        try:
            loaded[key] = load_run(entry, base_dir)
        except TelemetryError as exc:
            if exc.kind == 'invalid':
                ev.invalid.append(_note(entry, str(exc)))
            elif exc.kind == 'integrity':
                ev.integrity.append(_note(entry, str(exc)))
            else:
                ev.coverage.append(_note(entry, str(exc)))
            continue
        run = loaded[key]
        ev.synthetic = ev.synthetic or run['synthetic']
        ev.measured = ev.measured or not run['synthetic']
        pinned_cache = manifest.get('cache_state')
        if pinned_cache is not None and entry.get('cache_state') != pinned_cache:
            ev.integrity.append(_note(entry, f'cache_state {entry.get("cache_state")!r} does not '
                                             f'match pinned {pinned_cache!r}'))
            del loaded[key]
            continue
        want_build = manifest['candidate_build'] if entry['role'] == ROLE_CANDIDATE \
            else manifest['baseline_build']
        if entry.get('build') != want_build:
            ev.integrity.append(_note(entry, f'build {entry.get("build")!r} is not the '
                                             f'{entry["role"]} build {want_build!r}'))
            del loaded[key]
            continue
        if not run['complete']:
            ev.coverage.append(_note(entry, f'the log is incomplete ({run["lost"]} sample(s) '
                                            f'lost or the run did not end normally)'))
            del loaded[key]
    entries = {(e['role'], e['scene'], e['pair']): e for e in live}
    if manifest.get('mode', MODE_CHECK) == MODE_CHECK:
        families = (('check', ROLE_BASELINE, ROLE_CANDIDATE),)
    else:
        families = (('aa', ROLE_BASELINE, ROLE_REPEAT), ('injection', ROLE_BASELINE, ROLE_INJECTED))
    families += (('calibration', ROLE_CAL_A, ROLE_CAL_B),)
    for family, role_a, role_b in families:
        for scene in manifest['scenes']:
            numbers = sorted({p for (role, s, p) in entries if s == scene
                              and role in (role_a, role_b)})
            kept = []
            cache_states = set()
            for number in numbers:
                if (role_b, scene, number) in gone and (role_b, scene, number) not in entries:
                    continue                    # that run was discarded on purpose (a restart)
                if (role_b, scene, number) not in entries and family in (
                        entries.get((role_a, scene, number)) or {}).get('solo', ()):
                    continue                    # that comparison was already decided: A1 is unpaired
                a, b = loaded.get((role_a, scene, number)), loaded.get((role_b, scene, number))
                if a is None or b is None:
                    ev.coverage.append(f'{family} pair {number} of {scene} lacks a usable run')
                    continue
                ea, eb = entries[(role_a, scene, number)], entries[(role_b, scene, number)]
                problems = []
                if a['source'] != b['source']:
                    problems.append(f'sources differ ({a["source"]} / {b["source"]})')
                for field in ('settings', 'device'):
                    if ea.get(field) is not None and eb.get(field) is not None \
                            and ea[field] != eb[field]:
                        problems.append(f'{field} differ')
                ca, cb = ea.get('cache_state'), eb.get('cache_state')
                if (ca is not None or cb is not None) and ca != cb:
                    problems.append(f'cache_state differ ({ca!r} / {cb!r})')
                if problems:
                    ev.integrity.append(f'{family} pair {number} of {scene} is incomparable: '
                                        + '; '.join(problems))
                    continue
                for val in (ca, cb):
                    if val is not None:
                        cache_states.add(val)
                kept.append((number, a['metrics'], b['metrics']))
            if len(cache_states) > 1:
                ev.integrity.append(
                    f'{family} {scene} mixes cache states ({", ".join(sorted(cache_states))}): '
                    f'cold and warm samples cannot be mixed')
                kept = []
            ev.pairs[(family, scene)] = kept
    known = set(manifest['scenes'])
    for entry in manifest['runs']:
        if entry['scene'] not in known:
            ev.integrity.append(_note(entry, 'its scene is not one of the declared scenes'))
    return ev


# ----------------------------------------------------------------------------- statistics

def log_ratios(pairs, metric):
    return [math.log(a[metric]) - math.log(b[metric]) for _, a, b in pairs]


def spread_of(values):
    return statistics.stdev(values) if len(values) > 1 else 0.0


def calibrate_from_pairs(by_scene, scenes, build):
    """Spread of ln(A/A') per scene and metric from A/A pairs: the calibration document."""
    document = dict(schema=CALIBRATION_SCHEMA, kind=CALIBRATION_KIND, build=build,
                    min_pairs=C.MIN_CALIBRATION_PAIRS, scenes={})
    for scene in scenes:
        pairs = by_scene.get(scene, [])
        entry = dict(pairs=len(pairs), metrics={})
        for metric in METRICS:
            values = log_ratios(pairs, metric)
            entry['metrics'][metric] = dict(
                spread=spread_of(values) if len(values) > 1 else None,
                mean=statistics.fmean(values) if values else None)
        document['scenes'][scene] = entry
    return document


def calibration_problems(calibration, scenes, build, k):
    """Why a calibration document cannot support a verdict (empty list = usable)."""
    problems = []
    if not isinstance(calibration, dict) or calibration.get('kind') != CALIBRATION_KIND \
            or calibration.get('schema') != CALIBRATION_SCHEMA:
        return ['calibration document is not a regression_calibration schema 1 document']
    if calibration.get('build') != build:
        problems.append(f'calibration was taken on build {calibration.get("build")!r}, not on '
                        f'the baseline build {build!r}')
    for scene in scenes:
        entry = (calibration.get('scenes') or {}).get(scene)
        if not entry:
            problems.append(f'no calibration for scene {scene}')
            continue
        if not _is_int(entry.get('pairs')) or entry['pairs'] < C.MIN_CALIBRATION_PAIRS:
            problems.append(f'scene {scene} has {entry.get("pairs")} A/A pairs; at least '
                            f'{C.MIN_CALIBRATION_PAIRS} are needed')
            continue
        for metric in METRICS:
            data = (entry.get('metrics') or {}).get(metric) or {}
            spread = data.get('spread')
            if not isinstance(spread, (int, float)) or not math.isfinite(spread) or spread <= 0:
                problems.append(f'scene {scene} {metric}: the A/A spread is zero or missing')
                continue
            mean = data.get('mean')
            if isinstance(mean, (int, float)):
                n = entry['pairs']
                limit = t_critical(BIAS_ALPHA / (2 * k), n - 1) * spread / math.sqrt(n)
                if abs(mean) > limit:
                    problems.append(f'scene {scene} {metric}: the A/A runs differ systematically '
                                    f'(mean log ratio {mean:+.4f}, allowed {limit:.4f})')
    return problems


def per_test_alpha(k, looks):
    """Alpha of one one-sided test at one look: the family alpha over K tests and every look."""
    return C.ALPHA_FAMILY / (k * looks)


def design_of(manifest, largest=0):
    """The look schedule the manifest declares. Without a design block the run is one fixed-
    sample look at however many pairs it holds (n was fixed in advance): looks = 1."""
    block = manifest.get('design')
    if block is None:
        return dict(kind='fixed', min_pairs=C.MIN_PAIRS, max_pairs=max(largest, C.MIN_PAIRS),
                    looks=1)
    return dict(kind='sequential', min_pairs=block['min_pairs'], max_pairs=block['max_pairs'],
                looks=block['max_pairs'] - block['min_pairs'] + 1)


def _spread(values, s_obs, cal):
    """(spread, degrees of freedom, source). A stored calibration of the same build tightens the
    estimate by pooling its variance, unless these pairs are noisier than it (F test, 1 %)."""
    n = len(values)
    if cal is None:
        return s_obs, n - 1, 'own pairs'
    m, s_cal = cal['pairs'], cal['spread']
    if s_obs * s_obs <= f_critical(CAL_COMPAT_ALPHA, n - 1, m - 1) * s_cal * s_cal:
        pooled = math.sqrt(((n - 1) * s_obs * s_obs + (m - 1) * s_cal * s_cal) / (n + m - 2))
        return pooled, n + m - 2, 'pooled with the stored calibration'
    return s_obs, n - 1, 'own pairs (noisier than the stored calibration)'


def evaluate_metric(scene, metric, pairs, cal, k, looks, gating=True, min_pairs=None,
                    with_needed=True):
    """One scene and metric at one look.

    FAIL: the loss is proven positive (lower bound > 0). PASS: no loss proven, the pairs could
    have detected DELTA (planning power) and the upper bound of the loss is below DELTA (a loss
    of 5 % or more is rejected at the per-test alpha). Otherwise INCONCLUSIVE.
    """
    n = len(pairs)
    values = log_ratios(pairs, metric)
    d_bar = statistics.fmean(values)
    s_obs = spread_of(values)
    s, df, source = _spread(values, s_obs, cal)
    alpha = per_test_alpha(k, looks)
    c = t_critical(alpha, df)
    se = s / math.sqrt(n)
    h = c * se
    mde = (c + C.Z_BETA) * se
    lower, upper = d_bar - h, d_bar + h
    sensitive = mde <= C.DELTA
    noninferior = upper < C.DELTA
    if lower > 0:
        verdict = C.VERDICT_FAIL
    elif sensitive and noninferior:
        verdict = C.VERDICT_PASS
    else:
        verdict = C.VERDICT_INCONCLUSIVE
    needed = None
    if with_needed and verdict == C.VERDICT_INCONCLUSIVE and gating:
        needed = pairs_needed(s, k, looks, start=min_pairs or 2)
    return dict(
        scene=scene, metric=metric, gating=gating, pairs=n, d_bar=d_bar, s_obs=s_obs,
        s_cal=cal['spread'] if cal else None, s=s, df=df, spread_source=source, c=c, h=h,
        alpha_test=alpha, loss_lower=lower, loss_upper=upper,
        loss_pct=100.0 * (1.0 - math.exp(-d_bar)),
        loss_lower_pct=100.0 * (1.0 - math.exp(-lower)),
        loss_upper_pct=100.0 * (1.0 - math.exp(-upper)),
        min_detectable=mde, min_detectable_pct=100.0 * (1.0 - math.exp(-mde)),
        sensitive=sensitive, noninferior=noninferior, pairs_needed=needed, verdict=verdict)


def low_end_statement(row):
    return (f'{row["scene"]}: low-end drops beyond {row["min_detectable_pct"]:.1f} % would have '
            f'been detected ({row["pairs"]} pairs)')


def _calibration_for(calibration, scene, metric):
    if not calibration:
        return None
    entry = (calibration.get('scenes') or {}).get(scene) or {}
    spread = ((entry.get('metrics') or {}).get(metric) or {}).get('spread')
    if not isinstance(spread, (int, float)) or not spread > 0 or not _is_int(entry.get('pairs')):
        return None
    return dict(spread=spread, pairs=entry['pairs'])


def _section(ev, family, scenes, design, k, calibration):
    """Rows, failures, missing power and coverage of one comparison (one pair family)."""
    sec = dict(rows={}, failing=[], powerless=[], coverage=[], low_end={}, counts={})
    for scene in scenes:
        pairs = ev.pairs.get((family, scene), [])
        sec['counts'][scene] = len(pairs)
        if len(pairs) < design['min_pairs']:
            sec['coverage'].append(f'{scene}: {len(pairs)} usable pair(s) in the {family} '
                                   f'comparison; at least {design["min_pairs"]} are needed')
            continue
        if design['kind'] == 'sequential' and len(pairs) > design['max_pairs']:
            pairs = pairs[:design['max_pairs']]      # looks beyond the maximum were not budgeted
        for metric in METRICS:
            row = evaluate_metric(scene, metric, pairs, _calibration_for(calibration, scene,
                                                                         metric),
                                  k, design['looks'], metric == GATING_METRIC,
                                  design['min_pairs'])
            sec['rows'][f'{scene}:{metric}'] = row
            if row['verdict'] == C.VERDICT_FAIL:
                sec['failing'].append(row)
            elif row['gating']:
                if row['verdict'] == C.VERDICT_INCONCLUSIVE:
                    sec['powerless'].append(row)
            else:
                sec['low_end'][scene] = dict(
                    sensitivity_pct=row['min_detectable_pct'], loss_pct=row['loss_pct'],
                    loss_upper_pct=row['loss_upper_pct'], reaches_5_percent=row['sensitive'],
                    statement=low_end_statement(row))
    return sec


def _progress(sec, scenes):
    out = {}
    for scene in scenes:
        median = sec['rows'].get(f'{scene}:{GATING_METRIC}')
        out[scene] = dict(
            pairs=sec['counts'].get(scene, 0), verdict=median['verdict'] if median else None,
            failed=any(row['scene'] == scene and row['verdict'] == C.VERDICT_FAIL
                       for row in sec['rows'].values()))
    return out


def _power_reasons(rows, label=''):
    out = []
    for row in rows:
        needed = row['pairs_needed']
        if row['sensitive']:
            out.append(f'{label}{row["scene"]} {row["metric"]}: the upper bound of the loss '
                       f'({row["loss_upper_pct"]:.2f} %) does not rule out a '
                       f'{C.INJECTED_LOSS * 100:.0f} % loss ({row["pairs"]} pairs; measured '
                       f'{row["loss_pct"]:.2f} %)')
            continue
        out.append(
            f'{label}{row["scene"]} {row["metric"]}: {row["pairs"]} pairs could not have detected '
            f'a {C.INJECTED_LOSS * 100:.0f} % loss with the required power (smallest detectable '
            f'{row["min_detectable_pct"]:.2f} %, upper bound of the loss {row["loss_upper_pct"]:.2f} '
            f'%); about {needed if needed else "more than 5000"} pairs are needed')
    return out


def _fail_reasons(rows, label=''):
    return [f'{label}{row["scene"]} {row["metric"]}: the loss is at least '
            f'{row["loss_lower_pct"]:.2f} % with the family confidence (measured '
            f'{row["loss_pct"]:.2f} %, {row["pairs"]} pairs)' for row in rows]


# ----------------------------------------------------------------------------- injection proof

def achieved_estimate(values):
    """Achieved loss of the injected delay: point estimate and a nominal 95 % t interval."""
    n = len(values)
    if n < 2:
        return None
    d_bar = statistics.fmean(values)
    se = spread_of(values) / math.sqrt(n)
    t = t_critical(CI_ALPHA / 2.0, n - 1)

    def loss(d):
        return 1.0 - math.exp(-d)
    return dict(pairs=n, loss=loss(d_bar), low=loss(d_bar - t * se), high=loss(d_bar + t * se))


def proof_validity(est):
    """point_in_band: the estimate lies in 4.5..5.5 %. interval_in_tolerance: its interval
    covers 5 % and lies within 4..6 %. Otherwise the injected slowdown is not a 5 % slowdown."""
    if est is None:
        return VALID_NONE
    low_band, high_band = C.INJECTION_BAND
    if low_band <= est['loss'] <= high_band:
        return VALID_POINT
    tol_low, tol_high = INJECTION_TOLERANCE
    if est['low'] <= C.INJECTED_LOSS <= est['high'] and tol_low <= est['low'] \
            and est['high'] <= tol_high:
        return VALID_INTERVAL
    return VALID_OUTSIDE


def retune_delay(delay_ns, frame_ns, achieved_loss, target=C.INJECTED_LOSS):
    """The delay that should cost `target`, from the loss a delay did cost. Model: a delay d on a
    frame of f ns shows as e * d / (f + d), e the share of the delay that reaches the frame rate.
    None when almost none of it does (no delay of sensible size reaches the target)."""
    if not delay_ns or not frame_ns or achieved_loss <= 0:
        return None
    share = achieved_loss * (frame_ns + delay_ns) / delay_ns
    if share <= target * 1.1:
        return None
    return int(round(target * frame_ns / (share - target)))


def injection_report(injection, ev, sec, scenes):
    """Per-scene achieved loss of the injected delay (from the injection rounds themselves), the
    validity of the proof, and whether the delay should be retuned once."""
    delays = injection.get('delays') or {}
    band = C.INJECTION_BAND
    per_scene, achieved = {}, {}
    for scene in scenes:
        entry = delays.get(scene) or {}
        pairs = ev.pairs.get(('injection', scene), [])
        est = achieved_estimate(log_ratios(pairs, GATING_METRIC))
        validity = proof_validity(est)
        row = sec['rows'].get(f'{scene}:{GATING_METRIC}')
        retune = None
        strong = bool(est and (est['high'] < band[0] or est['low'] > band[1]))
        if est and not entry.get('restarts') \
                and RESTART_WINDOW[0] <= est['pairs'] <= RESTART_WINDOW[1] \
                and (strong or (est['pairs'] >= RESTART_POINT_FROM and validity == VALID_OUTSIDE)):
            retune = dict(new_delay_ns=retune_delay(entry.get('delay_ns'), entry.get('frame_ns'),
                                                    est['loss']),
                          reason=f'the achieved loss {est["loss"] * 100:.2f} % '
                                 f'(95 % interval {est["low"] * 100:.2f} to '
                                 f'{est["high"] * 100:.2f} %) is off the target')
        if est:
            achieved[scene] = est['loss']
        per_scene[scene] = dict(
            pairs=len(pairs), achieved_pct=est['loss'] * 100 if est else None,
            interval_pct=[est['low'] * 100, est['high'] * 100] if est else None,
            validity=validity, caught=bool(row and row['verdict'] == C.VERDICT_FAIL),
            delay_ns=entry.get('delay_ns'), frame_ns=entry.get('frame_ns'),
            restarts=entry.get('restarts') or [], retune=retune)
    valid = all(item['validity'] in (VALID_POINT, VALID_INTERVAL) for item in per_scene.values())
    detail = ', '.join(
        f'{scene} {item["achieved_pct"]:.2f} % ({item["validity"].replace("_", " ")})'
        if item['achieved_pct'] is not None else f'{scene} no data'
        for scene, item in per_scene.items())
    return dict(
        target=injection['target'], delay_ns=injection.get('delay_ns'),
        delays=injection.get('delays'), expected_loss=C.INJECTED_LOSS,
        achieved_loss=achieved, band=list(band), tolerance=list(INJECTION_TOLERANCE),
        valid=valid, scenes=per_scene, detail=detail, proof=None)


def scene_decided(mode, progress):
    """Sequential stopping rule of one scene (pure; the driver and the simulation share it).
    check: a significant loss, or the median row has its answer (PASS needs 5 % sensitivity).
    self_proof: a false alarm, or A/A PASS with the injection decided (caught and valid, or a
    sensitive PASS = missed)."""
    if mode == MODE_CHECK:
        item = progress['check']
        return item['failed'] or item['verdict'] in (C.VERDICT_PASS, C.VERDICT_FAIL)
    aa, inj = progress['aa'], progress['injection']
    if aa['failed']:
        return True
    inj_done = (inj['verdict'] == C.VERDICT_FAIL and inj['valid']) \
        or inj['verdict'] == C.VERDICT_PASS
    return aa['verdict'] == C.VERDICT_PASS and inj_done


def analyze(manifest, base_dir='.', calibration=None):
    """The result document (regress_contract.validate_result clean) for a runs manifest."""
    ev = gather(manifest, base_dir)
    scenes = manifest['scenes']
    mode = manifest.get('mode', MODE_CHECK)
    families = ('check',) if mode == MODE_CHECK else ('aa', 'injection')
    k = len(scenes) * len(METRICS)
    largest = max((len(ev.pairs.get((f, s), [])) for f in families for s in scenes), default=0)
    design = design_of(manifest, largest)
    looks = list(range(design['min_pairs'], design['max_pairs'] + 1)) \
        if design['kind'] == 'sequential' else [largest]
    reasons_integrity = list(ev.integrity)
    result = dict(
        schema=C.REGRESSION_RESULT_SCHEMA, verdict=C.VERDICT_INCONCLUSIVE, family_k=k,
        alpha=C.ALPHA_FAMILY, delta=C.DELTA, calibration=None, metrics={}, pictures=None,
        census=None, reasons=[], tool=TOOL, mode=mode, scenes=list(scenes),
        baseline_build=manifest['baseline_build'], candidate_build=manifest['candidate_build'],
        evidence=EVIDENCE_SYNTHETIC if ev.synthetic else EVIDENCE_MEASURED,
        beta=C.BETA_PLAN, min_pairs=design['min_pairs'], injection=None,
        design=dict(kind=design['kind'], min_pairs=design['min_pairs'],
                    max_pairs=design['max_pairs'], looks=looks, k=k, alpha_family=C.ALPHA_FAMILY,
                    alpha_per_test=per_test_alpha(k, design['looks']), rule=SPENDING_RULE),
        accounting=dict(
            runs=len(manifest['runs']), discarded_runs=ev.discarded,
            cache_state=manifest.get('cache_state'),
            paired_ordering={
                scene: [f'{e["role"]}:p{e["pair"]}'
                        for e in manifest['runs']
                        if e.get('scene') == scene and not e.get('discarded')]
                for scene in scenes},
            probe_runs={scene: sum(1 for p in manifest.get('probes') or []
                                   if p.get('scene') == scene and 'role' in p)
                        for scene in scenes}),
        stops=manifest.get('stops'))

    if ev.invalid:
        result['verdict'] = C.VERDICT_INVALID
        result['reasons'] = ['malformed telemetry: ' + item for item in ev.invalid]
        return result
    if ev.synthetic and ev.measured:
        result['verdict'] = C.VERDICT_INVALID
        result['reasons'] = ['synthetic and measured runs are mixed in one manifest']
        return result

    has_cal_runs = any(e['role'] in (ROLE_CAL_A, ROLE_CAL_B) for e in manifest['runs'])
    if calibration is None and has_cal_runs:
        by_scene = {scene: ev.pairs.get(('calibration', scene), []) for scene in scenes}
        calibration = calibrate_from_pairs(by_scene, scenes, manifest['baseline_build'])
    usable = None
    if calibration is not None:
        cal_problems = calibration_problems(calibration, scenes, manifest['baseline_build'], k)
        result['calibration'] = dict(
            valid=not cal_problems, problems=cal_problems, build=calibration.get('build'),
            min_pairs=C.MIN_CALIBRATION_PAIRS,
            scenes={scene: dict(pairs=(calibration.get('scenes') or {}).get(scene, {}).get('pairs'),
                                metrics=(calibration.get('scenes') or {}).get(scene, {}).get(
                                    'metrics')) for scene in scenes})
        if cal_problems:
            reasons_integrity += ['calibration invalid: ' + item for item in cal_problems]
        else:
            usable = calibration

    reasons = []
    if mode == MODE_CHECK:
        sec = _section(ev, 'check', scenes, design, k, usable)
        result['metrics'] = dict(sec['rows'])
        result['low_end'] = sec['low_end']
        result['progress'] = {scene: dict(check=item)
                              for scene, item in _progress(sec, scenes).items()}
        if reasons_integrity:
            verdict = C.VERDICT_INCONCLUSIVE
            reasons = reasons_integrity + sec['coverage'] + ev.coverage
        elif sec['failing']:
            verdict = C.VERDICT_FAIL
            reasons = _fail_reasons(sec['failing'])
        elif sec['coverage'] or ev.coverage:
            verdict = C.VERDICT_INCONCLUSIVE
            reasons = sec['coverage'] + ev.coverage
        elif sec['powerless']:
            verdict = C.VERDICT_INCONCLUSIVE
            reasons = _power_reasons(sec['powerless'])
        else:
            verdict = C.VERDICT_PASS
    else:
        aa = _section(ev, 'aa', scenes, design, k, usable)
        inj = _section(ev, 'injection', scenes, design, k, usable)
        report = injection_report(manifest['injection'], ev, inj, scenes)
        caught = all(item['caught'] for item in report['scenes'].values())
        report['proof'] = 'invalid' if not report['valid'] else ('caught' if caught else 'missed')
        result['injection'] = report
        result['metrics'] = {**{f'aa:{key}': row for key, row in aa['rows'].items()},
                             **{f'injection:{key}': row for key, row in inj['rows'].items()}}
        result['aa'] = dict(verdict=None, low_end=aa['low_end'])
        report['low_end'] = inj['low_end']
        prog_aa, prog_inj = _progress(aa, scenes), _progress(inj, scenes)
        result['progress'] = {
            scene: dict(aa=prog_aa[scene],
                        injection=dict(prog_inj[scene],
                                       valid=report['scenes'][scene]['validity']
                                       in (VALID_POINT, VALID_INTERVAL),
                                       retune=report['scenes'][scene]['retune']))
            for scene in scenes}
        restarts = sum(len((manifest['injection'].get('delays') or {}).get(s, {}).get(
            'restarts') or []) > 1 for s in scenes)
        if restarts:
            reasons_integrity.append('the injection arm was restarted more than once')
        coverage = aa['coverage'] + inj['coverage'] + ev.coverage
        if reasons_integrity:
            verdict = C.VERDICT_INCONCLUSIVE
            reasons = reasons_integrity + coverage
            result['aa']['verdict'] = C.VERDICT_INCONCLUSIVE
        elif aa['failing']:
            verdict = C.VERDICT_FAIL
            result['aa']['verdict'] = C.VERDICT_FAIL
            reasons = ['false alarm on identical builds (A/A) - ' + text
                       for text in _fail_reasons(aa['failing'])]
        else:
            result['aa']['verdict'] = C.VERDICT_INCONCLUSIVE if (aa['coverage'] or
                                                                 aa['powerless']) \
                else C.VERDICT_PASS
            reasons = list(coverage)
            if not report['valid']:
                reasons += [f'{scene}: the injected slowdown is not shown to be 5 %: '
                            f'{item["validity"].replace("_", " ")}'
                            + (f' (achieved {item["achieved_pct"]:.2f} %)'
                               if item['achieved_pct'] is not None else '')
                            for scene, item in report['scenes'].items()
                            if item['validity'] not in (VALID_POINT, VALID_INTERVAL)]
            elif not caught:
                reasons += [f'{scene}: the injected 5 % slowdown was NOT caught (self-proof failed)'
                            for scene, item in report['scenes'].items() if not item['caught']]
            reasons += _power_reasons(aa['powerless'], 'A/A: ')
            verdict = C.VERDICT_INCONCLUSIVE if reasons else C.VERDICT_PASS
    result['verdict'] = verdict
    result['reasons'] = reasons
    return result


def exit_code(result):
    return C.VERDICT_EXIT[result['verdict']]


# ----------------------------------------------------------------------------- gain (compare)
# A gain measurement, not a non-inferiority check: two-sided interval of the candidate/baseline
# ratio from paired runs, judged by the 0.4 keep rule against the run-to-run spread.

GAIN, LOSS, NO_DIFFERENCE, INCOMPLETE = (
    'GAIN', 'LOSS', 'NO DIFFERENCE BEYOND SPREAD', 'INCOMPLETE')
GAIN_CONFIDENCE = 0.95


def ratio_interval(base, cand, confidence=GAIN_CONFIDENCE):
    """Paired ratio candidate/baseline of per-run values (both positive, same pair order): the
    exp of the mean pair log ratio with an exact two-sided Student-t interval. None below 2
    pairs. pair_sd is the sd of the pair log ratios (the noise of one pair)."""
    n = min(len(base), len(cand))
    if n < 2:
        return None
    values = [math.log(c) - math.log(b) for b, c in zip(base[:n], cand[:n])]
    mean_log = statistics.fmean(values)
    sd = spread_of(values)
    half = t_critical((1.0 - confidence) / 2.0, n - 1) * sd / math.sqrt(n)
    return dict(pairs=n, ratio=math.exp(mean_log), low=math.exp(mean_log - half),
                high=math.exp(mean_log + half), pair_sd=sd, confidence=confidence)


def relative_range(values):
    """The 0.4 table's run-to-run spread, relative: (max - min) / mean of one arm's runs."""
    if len(values) < 2:
        return None
    return (max(values) - min(values)) / statistics.fmean(values)


def gain_verdict(est, spread):
    """The 0.4 keep rule on a ratio interval. GAIN: the whole interval is above 1 by more than
    the spread (low > 1 + spread). LOSS: the whole interval is below 1 by more than the spread.
    Otherwise NO DIFFERENCE BEYOND SPREAD. INCOMPLETE without an interval or a spread."""
    if est is None or spread is None:
        return INCOMPLETE
    if est['low'] > 1.0 + spread:
        return GAIN
    if est['high'] < 1.0 - spread:
        return LOSS
    return NO_DIFFERENCE


# ----------------------------------------------------------------------------- simulation

def binomial_interval(successes, trials, confidence=0.95):
    """Exact (Clopper-Pearson) interval of a rate, from the beta quantiles."""
    tail = (1.0 - confidence) / 2.0

    def quantile(q, a, b):                          # p with I_p(a, b) = q
        low, high = 0.0, 1.0
        for _ in range(80):
            mid = 0.5 * (low + high)
            if _betai(a, b, mid) < q:
                low = mid
            else:
                high = mid
        return 0.5 * (low + high)
    lower = 0.0 if successes == 0 else quantile(tail, successes, trials - successes + 1)
    upper = 1.0 if successes == trials else quantile(1.0 - tail, successes + 1,
                                                     trials - successes)
    return lower, upper


def simulate_sequences(sequences, seed, scenes=2, sigma=0.008, sigma_low=0.02, loss=0.0,
                       min_pairs=SEQ_MIN_PAIRS, max_pairs=SEQ_MAX_PAIRS, spend=True):
    """Self-proof rounds on generated per-run values, judged by the real decision code.

    Each round and scene is a triplet: baseline A1, baseline A2, injected I = A1-type run slowed
    by `loss` (loss 0: the injected arm is also an A/A). Run values are lognormal around a fixed
    level; the low end is noisier (sigma_low) and correlated with the median (0.5). Nothing stops
    early here: every look of the schedule is evaluated, which can only raise the false-alarm
    count of a stopping schedule. With spend=False each look uses the unspent alpha / K.
    Returns counts: sequences, aa_alarm (any aa test at any look is FAIL), inj_alarm (the same on
    the injection comparison), either, both, caught (injection median FAIL by the cap),
    scene_stops (per scene, the first round the stopping rule would stop at; cap + 1 = never),
    corr (correlation of the A/A and injection log ratios that share run A1).
    """
    rng = random.Random(seed)
    k = scenes * len(METRICS)
    looks = max_pairs - min_pairs + 1 if spend else 1
    keep = {metric: scale for metric, scale in zip(METRICS, (sigma, sigma_low))}
    out = dict(sequences=sequences, aa_alarm=0, inj_alarm=0, either=0, both=0, caught=0,
               scene_stops=[], corr=None)
    xs, ys = [], []

    def run_values():
        shared = rng.gauss(0.0, 1.0)
        return {m: math.exp(keep[m] * (0.5 * shared + math.sqrt(0.75) * rng.gauss(0.0, 1.0)))
                for m in METRICS}

    for _ in range(sequences):
        data = {scene: dict(aa=[], inj=[]) for scene in range(scenes)}
        aa_alarm = inj_alarm = False
        caught = [False] * scenes
        stops = [None] * scenes
        for n in range(1, max_pairs + 1):
            for scene in range(scenes):
                a1, a2, inj = run_values(), run_values(), run_values()
                inj = {m: v * (1.0 - loss) for m, v in inj.items()}
                data[scene]['aa'].append((n, a1, a2))
                data[scene]['inj'].append((n, a1, inj))
                xs.append(math.log(a1[GATING_METRIC]) - math.log(a2[GATING_METRIC]))
                ys.append(math.log(a1[GATING_METRIC]) - math.log(inj[GATING_METRIC]))
            if n < min_pairs:
                continue
            progress = {}
            for scene in range(scenes):
                rows = {}
                for family in ('aa', 'inj'):
                    rows[family] = {m: evaluate_metric(scene, m, data[scene][family], None, k, looks,
                                                       m == GATING_METRIC, min_pairs,
                                                       with_needed=False) for m in METRICS}
                aa_alarm = aa_alarm or any(r['verdict'] == C.VERDICT_FAIL
                                           for r in rows['aa'].values())
                inj_alarm = inj_alarm or any(r['verdict'] == C.VERDICT_FAIL
                                             for r in rows['inj'].values())
                med_aa, med_inj = rows['aa'][GATING_METRIC], rows['inj'][GATING_METRIC]
                caught[scene] = med_inj['verdict'] == C.VERDICT_FAIL or caught[scene]
                progress[scene] = dict(
                    aa=dict(verdict=med_aa['verdict'],
                            failed=any(r['verdict'] == C.VERDICT_FAIL
                                       for r in rows['aa'].values())),
                    injection=dict(verdict=med_inj['verdict'], valid=True))
            for scene in range(scenes):
                if stops[scene] is None and scene_decided(MODE_SELF_PROOF, progress[scene]):
                    stops[scene] = n
        out['aa_alarm'] += aa_alarm
        out['inj_alarm'] += inj_alarm
        out['either'] += aa_alarm or inj_alarm
        out['both'] += aa_alarm and inj_alarm
        out['caught'] += all(caught)
        out['scene_stops'] += [stop or max_pairs + 1 for stop in stops]
    if len(xs) > 2:
        mx, my = statistics.fmean(xs), statistics.fmean(ys)
        sx, sy = statistics.pstdev(xs), statistics.pstdev(ys)
        out['corr'] = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (len(xs) * sx * sy)
    return out


# ----------------------------------------------------------------------------- text and CLI

def describe(result):
    design = result['design']
    lines = [f'verdict {result["verdict"]}  ({result["mode"].replace("_", "-")}, evidence '
             f'{result["evidence"]})',
             f'  design: {design["kind"]}, {design["min_pairs"]} to {design["max_pairs"]} pairs, '
             f'{len(design["looks"])} look(s); K={result["family_k"]} tests, family alpha '
             f'{result["alpha"]}, alpha per test per look {design["alpha_per_test"]:.2e}; delta '
             f'{result["delta"]:.6f} = {C.INJECTED_LOSS * 100:.0f} % loss']
    if result['evidence'] == EVIDENCE_SYNTHETIC:
        lines.append('SYNTHETIC DATA: this run tests the logic only, it is not acceptance '
                     'evidence')
    for key, row in result['metrics'].items():
        mark = 'gate' if row['gating'] else 'info'
        lines.append(f'  {key:30} [{mark}] n={row["pairs"]:2} loss {row["loss_pct"]:+6.2f} % '
                     f'(upper {row["loss_upper_pct"]:+6.2f} %) s={row["s"]:.4f} c={row["c"]:.2f} '
                     f'detectable {row["min_detectable_pct"]:5.2f} %  {row["verdict"]}')
    low_ends = dict(result.get('low_end') or {})
    if result.get('aa'):
        low_ends.update({f'A/A {s}': v for s, v in result['aa'].get('low_end', {}).items()})
    for item in low_ends.values():
        lines.append('  low end: ' + item['statement'])
    if result.get('injection'):
        inj = result['injection']
        lines.append(f'  injection {inj["target"]}: achieved {inj["detail"]}; proof '
                     f'{inj["proof"]}')
        for scene, item in inj['scenes'].items():
            for restart in item['restarts']:
                lines.append(f'    {scene}: delay retuned {restart["old_delay_ns"]} -> '
                             f'{restart["new_delay_ns"]} ns after {restart["discarded_runs"]} '
                             f'discarded run(s)')
    acct = result['accounting']
    if any(acct['discarded_runs'].values()) or any(acct['probe_runs'].values()):
        lines.append(f'  accounting: {acct["runs"]} runs, discarded {acct["discarded_runs"]}, '
                     f'probe runs {acct["probe_runs"]}')
    for reason in result['reasons']:
        lines.append(f'  - {reason}')
    return '\n'.join(lines)


def write_json(path, document):
    temp = path + '.tmp'
    with open(temp, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(document, handle, indent=2, sort_keys=False)
        handle.write('\n')
    os.replace(temp, path)


def validate_logs(paths):
    worst = 0
    for path in paths:
        try:
            log = read_frame_times(path)
        except TelemetryError as exc:
            print(f'INVALID {exc}')
            worst = max(worst, C.EXIT_BAD_INPUT)
            continue
        state = 'complete' if log['complete'] else 'INCOMPLETE'
        print(f'ok      {os.path.basename(path)}: {log["source"]}, {log["recorded"]} frames, '
              f'lost {log["lost"]}, {state}')
    return worst


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog=TOOL, description='Paired A/B regression statistics for the frame-rate check.',
        epilog='exit 0 PASS, 1 FAIL, 2 invalid input, 3 INCONCLUSIVE, 4 tool failure')
    parser.add_argument('--runs', help='runs manifest (JSON)')
    parser.add_argument('--calibration', help='stored A/A calibration document (JSON)')
    parser.add_argument('--calibrate', action='store_true',
                        help='write the A/A spread of the calibration runs and exit')
    parser.add_argument('--out', help='write the result (or calibration) JSON here')
    parser.add_argument('--validate-frame-times', nargs='+', metavar='FILE',
                        help='check emulator --frame-times logs and exit')
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return C.EXIT_BAD_INPUT if exc.code else 0
    try:
        if args.validate_frame_times:
            return validate_logs(args.validate_frame_times)
        if not args.runs:
            sys.stderr.write(f'{TOOL}: --runs is required\n')
            return C.EXIT_BAD_INPUT
        manifest = load_json(args.runs, 'the runs manifest')
        base_dir = os.path.dirname(os.path.abspath(args.runs))
        if args.calibrate:
            ev = gather(manifest, base_dir)
            if ev.invalid:
                print('INVALID ' + '; '.join(ev.invalid))
                return C.EXIT_BAD_INPUT
            by_scene = {scene: ev.pairs.get(('calibration', scene), [])
                        for scene in manifest['scenes']}
            document = calibrate_from_pairs(by_scene, manifest['scenes'],
                                            manifest['baseline_build'])
            problems = calibration_problems(document, manifest['scenes'],
                                            manifest['baseline_build'],
                                            len(manifest['scenes']) * len(METRICS))
            document['valid'] = not problems
            document['problems'] = problems
            if args.out:
                write_json(args.out, document)
            print(json.dumps(document, indent=2))
            return C.EXIT_PASS if not problems else C.EXIT_INCONCLUSIVE
        calibration = load_json(args.calibration, 'the calibration document') \
            if args.calibration else None
        result = analyze(manifest, base_dir, calibration)
        problems = C.validate_result(result)
        if problems:
            sys.stderr.write(f'{TOOL}: internal error, invalid result: {problems}\n')
            return C.EXIT_EXECUTION
        if args.out:
            write_json(args.out, result)
        print(describe(result))
        return exit_code(result)
    except ManifestError as exc:
        sys.stderr.write(f'{TOOL}: {exc}\n')
        return C.EXIT_BAD_INPUT
    except KeyboardInterrupt:
        return C.EXIT_INTERRUPTED
    except Exception as exc:  # noqa: BLE001 - a tool failure is exit 4, not a traceback verdict
        sys.stderr.write(f'{TOOL}: failed: {exc!r}\n')
        return C.EXIT_EXECUTION


if __name__ == '__main__':
    raise SystemExit(main())
