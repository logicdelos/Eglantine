"""Tests of regress_stats (slice E).

LOGIC ONLY, NOT ACCEPTANCE EVIDENCE. Every frame time below is generated from a seeded random
generator (or written as synthetic telemetry files); no game, capture, dump or emulator is
involved. These tests prove the decision logic (verdicts, exit codes, input validation, the
Student-t arithmetic); they prove nothing about the emulator's speed. The seeds are fixed, so a
failure is a logic change, never flakiness.

Run: python test_regress_stats.py
"""

import json
import math
import os
import random
import statistics
import tempfile
import unittest

import regress_contract as C
import regress_stats as R

SCENES = ['scene_a', 'scene_b']
RUN_SIGMA = 0.008          # run-to-run drift of the whole run (log units)
FRAME_SIGMA = 0.03
STUTTER_P = 0.05
STUTTER_MULT = 3.0
BASE_MS = 16.0
FRAMES = 1000


def frames(rng, run_sigma=RUN_SIGMA, stutter_mult=STUTTER_MULT, add_ms=0.0, base_ms=BASE_MS,
           count=FRAMES):
    factor = math.exp(rng.gauss(0.0, run_sigma))
    out = []
    for _ in range(count):
        value = base_ms * factor * math.exp(rng.gauss(0.0, FRAME_SIGMA))
        if rng.random() < STUTTER_P:
            value *= stutter_mult
        out.append(value + add_ms)
    return out


def run_entry(role, scene, pair, build, frame_ms):
    return dict(id=f'{role}-{scene}-{pair}', role=role, scene=scene, pair=pair, build=build,
                synthetic=True, frame_ms=frame_ms)


def manifest(seed=1, scenes=SCENES, cal_pairs=30, pairs=30, candidate=None, run_sigma=RUN_SIGMA,
             baseline_build='build-base', candidate_build='build-cand', injection=None):
    """A synthetic runs manifest; candidate = kwargs for the candidate arm's frames()."""
    rng = random.Random(seed)
    runs = []
    for scene in scenes:
        for pair in range(1, cal_pairs + 1):
            runs.append(run_entry(R.ROLE_CAL_A, scene, pair, baseline_build,
                                  frames(rng, run_sigma)))
            runs.append(run_entry(R.ROLE_CAL_B, scene, pair, baseline_build,
                                  frames(rng, run_sigma)))
        for pair in range(1, pairs + 1):
            runs.append(run_entry(R.ROLE_BASELINE, scene, pair, baseline_build,
                                  frames(rng, run_sigma)))
            runs.append(run_entry(R.ROLE_CANDIDATE, scene, pair, candidate_build,
                                  frames(rng, run_sigma, **(candidate or {}))))
    doc = dict(schema=R.MANIFEST_SCHEMA, kind=R.MANIFEST_KIND, scenes=list(scenes),
               baseline_build=baseline_build, candidate_build=candidate_build, runs=runs)
    if injection:
        doc['injection'] = injection
    return doc


def proof_manifest(seed=1, scenes=SCENES, rounds=6, add_ms=None, repeat_add_ms=0.0,
                   run_sigma=0.002, design=(4, 10), build='build-base'):
    """A synthetic self-proof manifest: per round and scene A1, A2 and I (baseline + delay)."""
    rng = random.Random(seed)
    base_ns = int(statistics.median(frames(random.Random(3), run_sigma=0.0)) * 1e6)
    if add_ms is None:
        add_ms = C.delay_for_loss(base_ns) / 1e6
    delay_ns = round(add_ms * 1e6)
    runs = []
    for scene in scenes:
        for pair in range(1, rounds + 1):
            runs.append(run_entry(R.ROLE_BASELINE, scene, pair, build, frames(rng, run_sigma)))
            runs.append(run_entry(R.ROLE_REPEAT, scene, pair, build,
                                  frames(rng, run_sigma, add_ms=repeat_add_ms)))
            injected = run_entry(R.ROLE_INJECTED, scene, pair, build,
                                 frames(rng, run_sigma, add_ms=add_ms))
            injected['delay_ns'] = delay_ns
            runs.append(injected)
    doc = dict(schema=R.MANIFEST_SCHEMA, kind=R.MANIFEST_KIND, scenes=list(scenes),
               baseline_build=build, candidate_build=build, mode=R.MODE_SELF_PROOF, runs=runs,
               injection=dict(target='cpu-present',
                              delays={scene: dict(delay_ns=delay_ns, frame_ns=base_ns)
                                      for scene in scenes}))
    if design:
        doc['design'] = dict(min_pairs=design[0], max_pairs=design[1])
    return doc


def verdict_of(doc, **kw):
    result = R.analyze(doc, '.', **kw)
    return result, R.exit_code(result)


class StudentT(unittest.TestCase):
    def test_known_critical_values(self):
        # Standard table values (one-sided upper tail).
        for alpha, df, expected in ((0.025, 10, 2.2281), (0.005, 29, 2.7564),
                                    (0.025, 1, 12.7062), (0.01, 5, 3.3649),
                                    (0.05, 100, 1.6602), (0.001, 29, 3.3962)):
            self.assertAlmostEqual(R.t_critical(alpha, df), expected, places=3)

    def test_tail_and_critical_are_inverse(self):
        for df in (2, 9, 29, 120):
            for alpha in (0.1, 0.01, 0.0025):
                self.assertAlmostEqual(R.t_upper_tail(R.t_critical(alpha, df), df), alpha,
                                       places=10)

    def test_large_df_approaches_normal(self):
        self.assertAlmostEqual(R.t_critical(0.025, 100000), 1.95996, places=3)

    def test_bad_arguments(self):
        with self.assertRaises(ValueError):
            R.t_critical(0.7, 5)
        with self.assertRaises(ValueError):
            R.t_upper_tail(1.0, 0)

    def test_critical_grows_with_family_size(self):
        self.assertLess(R.family_critical(2, 30), R.family_critical(6, 30))
        self.assertLess(R.family_critical(6, 30), R.family_critical(6, 10))

    def test_planned_pairs_cover_the_planning_constant(self):
        # K = 6, 30 pairs: the exact value must not exceed the contract's planning constant.
        self.assertLessEqual(R.family_critical(6, 30), C.C_PLANNING)
        n = R.pairs_needed(0.04, 6)
        self.assertGreaterEqual(n, C.MIN_PAIRS)
        c = R.family_critical(6, n)
        self.assertLessEqual((c + C.Z_BETA) * 0.04 / math.sqrt(n), C.DELTA)
        if n > C.MIN_PAIRS:
            c = R.family_critical(6, n - 1)
            self.assertGreater((c + C.Z_BETA) * 0.04 / math.sqrt(n - 1), C.DELTA)


class FrameMetrics(unittest.TestCase):
    def test_median_and_p99(self):
        ms = [10.0] * 990 + [20.0] * 10
        m = R.run_metrics(ms)
        self.assertAlmostEqual(m['median_fps'], 100.0)
        self.assertAlmostEqual(m['inverse_p99'], 1000.0 / R.percentile(ms, 99))
        self.assertLess(m['inverse_p99'], m['median_fps'])

    def test_short_run_is_refused(self):
        with self.assertRaises(R.TelemetryError) as ctx:
            R.run_metrics([10.0] * (R.MIN_FRAMES - 1))
        self.assertEqual(ctx.exception.kind, 'short')

    def test_non_positive_frame_time_is_invalid(self):
        with self.assertRaises(R.TelemetryError) as ctx:
            R.run_metrics([10.0] * R.MIN_FRAMES + [0.0])
        self.assertEqual(ctx.exception.kind, 'invalid')

    def test_percentile_interpolates(self):
        self.assertAlmostEqual(R.percentile([1, 2, 3, 4, 5], 50), 3.0)
        self.assertAlmostEqual(R.percentile([1, 2, 3, 4, 5], 99), 4.96)


class Verdicts(unittest.TestCase):
    def test_a_a_passes(self):
        result, code = verdict_of(manifest(seed=11))
        self.assertEqual(result['verdict'], C.VERDICT_PASS, result['reasons'])
        self.assertEqual(code, C.EXIT_PASS)
        self.assertEqual(C.validate_result(result), [])
        self.assertEqual(result['family_k'], len(SCENES) * len(C.METRICS_DEFAULT))
        self.assertEqual(result['evidence'], R.EVIDENCE_SYNTHETIC)
        self.assertEqual(result['reasons'], [])
        self.assertEqual(len(result['metrics']), 4)
        for row in result['metrics'].values():
            self.assertLessEqual(row['min_detectable'], C.DELTA)

    def test_five_percent_median_loss_fails(self):
        base = statistics.median(frames(random.Random(3), run_sigma=0.0))
        delay_ms = C.delay_for_loss(base * 1e6) / 1e6
        doc = manifest(seed=12, candidate=dict(add_ms=delay_ms))
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_FAIL, result['reasons'])
        self.assertEqual(code, C.EXIT_REGRESSION)
        self.assertEqual(C.validate_result(result), [])
        for scene in SCENES:
            row = result['metrics'][f'{scene}:median_fps']
            self.assertEqual(row['verdict'], C.VERDICT_FAIL)
            self.assertGreater(row['loss_lower_pct'], 0.0)
            self.assertTrue(4.5 <= row['loss_pct'] <= 5.5, row['loss_pct'])
        self.assertIsNone(result['injection'])

    def test_five_percent_loss_fails_with_the_minimum_pairs(self):
        base = statistics.median(frames(random.Random(3), run_sigma=0.0))
        doc = manifest(seed=13, pairs=C.MIN_PAIRS,
                       candidate=dict(add_ms=C.delay_for_loss(base * 1e6) / 1e6))
        result, _ = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_FAIL, result['reasons'])

    def test_low_end_only_loss_fails_through_inverse_p99(self):
        # Stutter frames 10 % longer, median frame time untouched.
        doc = manifest(seed=14, candidate=dict(stutter_mult=STUTTER_MULT * 1.10))
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_FAIL, result['reasons'])
        self.assertEqual(code, C.EXIT_REGRESSION)
        for scene in SCENES:
            self.assertEqual(result['metrics'][f'{scene}:inverse_p99']['verdict'], C.VERDICT_FAIL)
            self.assertNotEqual(result['metrics'][f'{scene}:median_fps']['verdict'],
                                C.VERDICT_FAIL)

    def test_self_proof_manifest_needs_an_injection_block(self):
        doc = proof_manifest(seed=15)
        del doc['injection']
        with self.assertRaises(R.ManifestError):
            R.check_manifest(doc)


    def test_injection_delays_must_be_positive_integers(self):
        for bad in ({}, {SCENES[0]: dict(delay_ns=0)}, {SCENES[0]: dict(delay_ns='5')},
                    {SCENES[0]: 7}):
            doc = manifest(seed=19, injection=dict(target='cpu-present', delays=bad))
            with self.assertRaises(R.ManifestError):
                R.check_manifest(doc)

    def test_under_powered_is_inconclusive(self):
        doc = manifest(seed=17, pairs=C.MIN_PAIRS, run_sigma=0.06, cal_pairs=30)
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE, result['reasons'])
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertEqual(C.validate_result(result), [])
        self.assertTrue(any('could not have detected' in r for r in result['reasons']))
        self.assertTrue(all(row['pairs_needed'] is None or row['pairs_needed'] > C.MIN_PAIRS
                            for row in result['metrics'].values()
                            if row['gating'] and row['verdict'] == C.VERDICT_INCONCLUSIVE))

    def test_too_few_pairs_is_inconclusive(self):
        result, code = verdict_of(manifest(seed=18, pairs=C.MIN_PAIRS - 1))
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertEqual(result['metrics'], {})

    def test_too_few_calibration_pairs_is_inconclusive(self):
        result, code = verdict_of(manifest(seed=19, cal_pairs=C.MIN_CALIBRATION_PAIRS - 1))
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertFalse(result['calibration']['valid'])

    def test_a_regression_is_never_masked_by_a_thin_scene(self):
        doc = manifest(seed=20, candidate=dict(stutter_mult=STUTTER_MULT * 1.10))
        doc['runs'] = [r for r in doc['runs']
                       if not (r['scene'] == 'scene_b' and r['role'] in (R.ROLE_BASELINE,
                                                                         R.ROLE_CANDIDATE))]
        result, _ = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_FAIL)
        self.assertTrue(any('scene_b' in r for r in result['reasons']) or result['reasons'])

    def test_zero_variation_calibration_is_refused(self):
        doc = manifest(seed=21, cal_pairs=30)
        flat = [10.0] * R.MIN_FRAMES
        for run in doc['runs']:
            if run['role'] in (R.ROLE_CAL_A, R.ROLE_CAL_B):
                run['frame_ms'] = list(flat)
        result, _ = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertFalse(result['calibration']['valid'])

    def test_biased_calibration_is_refused(self):
        doc = manifest(seed=22, cal_pairs=30)
        for run in doc['runs']:
            if run['role'] == R.ROLE_CAL_B:
                run['frame_ms'] = [x * 1.04 for x in run['frame_ms']]
        result, _ = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertTrue(any('systematically' in p for p in result['calibration']['problems']))


class Comparability(unittest.TestCase):
    def test_wrong_build_is_inconclusive(self):
        doc = manifest(seed=31)
        for run in doc['runs']:
            if run['role'] == R.ROLE_CANDIDATE and run['pair'] == 3:
                run['build'] = 'some-other-build'
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('some-other-build' in r for r in result['reasons']))

    def test_run_without_build_identity_is_inconclusive(self):
        doc = manifest(seed=32)
        del doc['runs'][5]['build']
        result, _ = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)

    def test_calibration_from_another_build_is_inconclusive(self):
        doc = manifest(seed=33)
        by_scene = {s: [] for s in SCENES}
        ev = R.gather(doc, '.')
        for s in SCENES:
            by_scene[s] = ev.pairs[('calibration', s)]
        cal = R.calibrate_from_pairs(by_scene, SCENES, 'an-older-baseline')
        result, code = verdict_of(doc, calibration=cal)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)

    def test_stored_calibration_of_the_same_build_is_used(self):
        doc = manifest(seed=34)
        ev = R.gather(doc, '.')
        cal = R.calibrate_from_pairs({s: ev.pairs[('calibration', s)] for s in SCENES}, SCENES,
                                     'build-base')
        doc['runs'] = [r for r in doc['runs']
                       if r['role'] not in (R.ROLE_CAL_A, R.ROLE_CAL_B)]
        result, _ = verdict_of(doc, calibration=cal)
        self.assertEqual(result['verdict'], C.VERDICT_PASS, result['reasons'])

    def test_undeclared_scene_is_inconclusive(self):
        doc = manifest(seed=35)
        rng = random.Random(1)
        doc['runs'].append(run_entry(R.ROLE_BASELINE, 'elsewhere', 1, 'build-base', frames(rng)))
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)

    def test_pair_from_two_scene_settings_is_inconclusive(self):
        doc = manifest(seed=36)
        for run in doc['runs']:
            if run['role'] == R.ROLE_BASELINE and run['scene'] == 'scene_a' and run['pair'] == 1:
                run['settings'] = '1x'
            if run['role'] == R.ROLE_CANDIDATE and run['scene'] == 'scene_a' and run['pair'] == 1:
                run['settings'] = '2x'
        result, _ = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertTrue(any('incomparable' in r for r in result['reasons']))

    def test_missing_run_in_a_pair_never_passes(self):
        doc = manifest(seed=37, pairs=C.MIN_PAIRS)
        doc['runs'] = [r for r in doc['runs'] if not (r['role'] == R.ROLE_CANDIDATE
                                                      and r['scene'] == 'scene_a'
                                                      and r['pair'] == 4)]
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)

    def test_failed_run_never_passes(self):
        doc = manifest(seed=38, pairs=C.MIN_PAIRS)
        for run in doc['runs']:
            if run['role'] == R.ROLE_BASELINE and run['scene'] == 'scene_b' and run['pair'] == 2:
                run['ok'] = False
                run['failures'] = ['emulator exited early']
        result, _ = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)

    def test_mixed_synthetic_and_measured_is_invalid(self):
        doc = manifest(seed=39, pairs=C.MIN_PAIRS)
        doc['runs'][0] = dict(id='x', role=R.ROLE_CAL_A, scene='scene_a', pair=1,
                              build='build-base', format='presentmon', file='nothing.csv')
        # a missing file is only missing, so give the mixed case a readable measured run
        with tempfile.TemporaryDirectory() as tmp:
            write_presentmon(os.path.join(tmp, 'nothing.csv'), frames(random.Random(2)))
            result, code = verdict_of_dir(doc, tmp)
        self.assertEqual(result['verdict'], C.VERDICT_INVALID)
        self.assertEqual(code, C.EXIT_BAD_INPUT)

    def test_malformed_manifest_raises(self):
        for bad in ({}, dict(schema=1, kind=R.MANIFEST_KIND, scenes=[], baseline_build='a',
                             candidate_build='b', runs=[])):
            with self.assertRaises(R.ManifestError):
                R.analyze(bad)

    def test_inline_frames_need_the_synthetic_flag(self):
        doc = manifest(seed=40, pairs=C.MIN_PAIRS)
        del doc['runs'][0]['synthetic']
        with self.assertRaises(R.ManifestError):
            R.analyze(doc)

    def test_duplicate_run_is_a_malformed_manifest(self):
        doc = manifest(seed=41, pairs=C.MIN_PAIRS)
        doc['runs'].append(dict(doc['runs'][0]))
        with self.assertRaises(R.ManifestError):
            R.analyze(doc)


def verdict_of_dir(doc, directory):
    result = R.analyze(doc, directory)
    return result, R.exit_code(result)


def write_frame_times(path, frame_ms, source='host_present', capacity=None, lost=0,
                      complete=True, start_ns=1_000_000_000):
    times, now = [start_ns], start_ns
    for ms in frame_ms:
        now += int(round(ms * 1e6))
        times.append(now)
    capacity = capacity or len(times)
    lines = [json.dumps(dict(schema=1, kind='frame_times', source=source, clock='steady_ns',
                             capacity=capacity), separators=(',', ':'))]
    lines += [json.dumps(dict(id=i, t=t), separators=(',', ':')) for i, t in enumerate(times)]
    lines.append(json.dumps(dict(end=True, recorded=len(times), lost=lost, complete=complete),
                            separators=(',', ':')))
    with open(path, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write('\n'.join(lines) + '\n')


def write_presentmon(path, frame_ms, extra_rows=()):
    t = 0.0
    with open(path, 'w', encoding='utf-8', newline='') as handle:
        handle.write('Application,ProcessID,TimeInSeconds,MsBetweenPresents\n')
        for ms in frame_ms:
            t += ms / 1000.0
            handle.write(f'app.exe,1,{t:.6f},{ms:.6f}\n')
        for row in extra_rows:
            handle.write(row + '\n')


class FrameTimesInput(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def path(self, name):
        return os.path.join(self.dir, name)

    def sample_log(self, name='ok.jsonl', **kw):
        path = self.path(name)
        write_frame_times(path, frames(random.Random(5), count=400), **kw)
        return path

    def test_valid_log_reads(self):
        log = R.read_frame_times(self.sample_log())
        self.assertEqual(log['source'], 'host_present')
        self.assertEqual(log['recorded'], 401)
        self.assertTrue(log['complete'])
        self.assertEqual(len(R.frame_ms_from_times(log['times_ns'])), 400)

    def test_replay_frame_source_reads(self):
        log = R.read_frame_times(self.sample_log(source='replay_frame'))
        self.assertEqual(log['source'], 'replay_frame')

    def test_every_truncation_is_invalid(self):
        raw = open(self.sample_log(), 'rb').read()
        # A cut at every line boundary and in the middle of lines, near both ends and sampled
        # through the middle (a full sweep of 400 lines is covered by the step).
        cuts = set(range(0, 400)) | set(range(len(raw) - 400, len(raw))) \
            | set(range(0, len(raw), 997))
        for cut in sorted(cuts):
            if cut >= len(raw):
                continue
            path = self.path('cut.jsonl')
            with open(path, 'wb') as handle:
                handle.write(raw[:cut])
            with self.assertRaises(R.TelemetryError, msg=f'cut at {cut}') as ctx:
                R.read_frame_times(path)
            self.assertEqual(ctx.exception.kind, 'invalid', f'cut at {cut}')

    def test_missing_trailing_newline_is_invalid(self):
        raw = open(self.sample_log(), 'rb').read()
        path = self.path('nonl.jsonl')
        with open(path, 'wb') as handle:
            handle.write(raw[:-1])
        with self.assertRaises(R.TelemetryError):
            R.read_frame_times(path)

    def damaged(self, mutate, name='bad.jsonl'):
        lines = open(self.sample_log(), encoding='utf-8').read().split('\n')[:-1]
        mutate(lines)
        path = self.path(name)
        with open(path, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write('\n'.join(lines) + '\n')
        return path

    def test_damage_is_invalid(self):
        cases = {
            'garbage line': lambda L: L.__setitem__(7, '{not json'),
            'blank line': lambda L: L.insert(7, ''),
            'wrong keys': lambda L: L.__setitem__(7, '{"id":6,"time":99}'),
            'float time': lambda L: L.__setitem__(7, '{"id":6,"t":1.5}'),
            'string id': lambda L: L.__setitem__(7, '{"id":"6","t":2000000000}'),
            'time backwards': lambda L: L.__setitem__(
                7, json.dumps(dict(id=6, t=1))),
            'bad schema': lambda L: L.__setitem__(
                0, L[0].replace('"schema":1', '"schema":2')),
            'bad source': lambda L: L.__setitem__(
                0, L[0].replace('host_present', 'somewhere')),
            'bad clock': lambda L: L.__setitem__(0, L[0].replace('steady_ns', 'wall')),
            'count mismatch': lambda L: L.__setitem__(
                -1, '{"end":true,"recorded":400,"lost":0,"complete":true}'),
            'no end line': lambda L: L.pop(),
            'lines after the end': lambda L: L.append('{"id":1,"t":5}'),
            'complete with loss': lambda L: L.__setitem__(
                -1, '{"end":true,"recorded":401,"lost":3,"complete":true}'),
            'string complete': lambda L: L.__setitem__(
                -1, '{"end":true,"recorded":401,"lost":0,"complete":"yes"}'),
        }
        for name, mutate in cases.items():
            path = self.damaged(mutate)
            with self.assertRaises(R.TelemetryError, msg=name) as ctx:
                R.read_frame_times(path)
            self.assertEqual(ctx.exception.kind, 'invalid', name)

    def test_empty_and_binary_files_are_invalid(self):
        for name, data in (('empty', b''), ('bin', b'\xff\xfe\x00\n'), ('nl', b'\n')):
            path = self.path(name)
            with open(path, 'wb') as handle:
                handle.write(data)
            with self.assertRaises(R.TelemetryError) as ctx:
                R.read_frame_times(path)
            self.assertEqual(ctx.exception.kind, 'invalid', name)

    def test_missing_file_is_missing_not_invalid(self):
        with self.assertRaises(R.TelemetryError) as ctx:
            R.read_frame_times(self.path('nope.jsonl'))
        self.assertEqual(ctx.exception.kind, 'missing')

    def test_lossy_log_is_accepted_by_the_reader_and_marked(self):
        path = self.path('lossy.jsonl')
        times = [1_000_000_000 + i * 16_000_000 for i in range(50)]
        lines = [json.dumps(dict(schema=1, kind='frame_times', source='host_present',
                                 clock='steady_ns', capacity=50))]
        lines += [json.dumps(dict(id=i, t=t)) for i, t in enumerate(times)]
        lines.append(json.dumps(dict(end=True, recorded=50, lost=9, complete=False)))
        with open(path, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write('\n'.join(lines) + '\n')
        log = R.read_frame_times(path)
        self.assertEqual((log['lost'], log['complete']), (9, False))

    def test_presentmon_reader_and_window(self):
        path = self.path('pm.csv')
        write_presentmon(path, [10.0] * 500)
        self.assertEqual(len(R.read_presentmon(path)), 500)
        windowed = R.read_presentmon(path, (1.0, 3.0))
        self.assertTrue(190 <= len(windowed) <= 210)

    def test_presentmon_damage_is_invalid(self):
        path = self.path('bad.csv')
        with open(path, 'w', encoding='utf-8') as handle:
            handle.write('A,B\n1,2\n')
        with self.assertRaises(R.TelemetryError) as ctx:
            R.read_presentmon(path)
        self.assertEqual(ctx.exception.kind, 'invalid')
        path = self.path('junk.csv')
        write_presentmon(path, [10.0] * 100, extra_rows=['app.exe,1,x,y'] * 20)
        with self.assertRaises(R.TelemetryError):
            R.read_presentmon(path)

    def test_manifest_with_a_truncated_log_is_invalid_and_a_missing_one_inconclusive(self):
        doc = file_manifest(self.dir, seed=51, cal_pairs=R.C.MIN_CALIBRATION_PAIRS,
                            pairs=C.MIN_PAIRS)
        entry = next(r for r in doc['runs'] if r['role'] == R.ROLE_CANDIDATE and r['pair'] == 1)
        path = os.path.join(self.dir, entry['file'])
        raw = open(path, 'rb').read()
        with open(path, 'wb') as handle:
            handle.write(raw[:len(raw) // 2])
        result, code = verdict_of_dir(doc, self.dir)
        self.assertEqual(result['verdict'], C.VERDICT_INVALID)
        self.assertEqual(code, C.EXIT_BAD_INPUT)
        self.assertEqual(C.validate_result(result), [])
        os.unlink(path)
        result, code = verdict_of_dir(doc, self.dir)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)

    def test_lossy_run_in_a_manifest_is_excluded_not_trusted(self):
        doc = file_manifest(self.dir, seed=52, cal_pairs=C.MIN_CALIBRATION_PAIRS,
                            pairs=C.MIN_PAIRS)
        entry = next(r for r in doc['runs'] if r['role'] == R.ROLE_BASELINE and r['pair'] == 2)
        write_frame_times(os.path.join(self.dir, entry['file']),
                          frames(random.Random(9), count=300), capacity=301, lost=40,
                          complete=False)
        result, _ = verdict_of_dir(doc, self.dir)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertTrue(any('incomplete' in r for r in result['reasons']))

    def test_log_source_must_match_the_manifest(self):
        doc = file_manifest(self.dir, seed=53, cal_pairs=C.MIN_CALIBRATION_PAIRS,
                            pairs=C.MIN_PAIRS)
        entry = next(r for r in doc['runs'] if r['role'] == R.ROLE_CANDIDATE and r['pair'] == 1)
        entry['source'] = 'replay_frame'
        result, _ = verdict_of_dir(doc, self.dir)
        self.assertEqual(result['verdict'], C.VERDICT_INVALID)

    def test_files_end_to_end_a_a_passes_and_loss_fails(self):
        doc = file_manifest(self.dir, seed=54, cal_pairs=C.MIN_CALIBRATION_PAIRS, pairs=12)
        result, code = verdict_of_dir(doc, self.dir)
        self.assertEqual(result['verdict'], C.VERDICT_PASS, result['reasons'])
        self.assertEqual(code, C.EXIT_PASS)
        self.assertEqual(result['evidence'], R.EVIDENCE_MEASURED)
        slow = file_manifest(os.path.join(self.dir, 'slow'), seed=54,
                             cal_pairs=C.MIN_CALIBRATION_PAIRS, pairs=12, candidate_add_ms=0.9)
        result, code = verdict_of_dir(slow, os.path.join(self.dir, 'slow'))
        self.assertEqual(result['verdict'], C.VERDICT_FAIL, result['reasons'])
        self.assertEqual(code, C.EXIT_REGRESSION)

    def test_cli_exit_codes(self):
        doc = file_manifest(self.dir, seed=55, cal_pairs=C.MIN_CALIBRATION_PAIRS, pairs=12)
        runs = os.path.join(self.dir, 'runs.json')
        with open(runs, 'w', encoding='utf-8') as handle:
            json.dump(doc, handle)
        out = os.path.join(self.dir, 'result.json')
        self.assertEqual(R.main(['--runs', runs, '--out', out]), C.EXIT_PASS)
        self.assertEqual(C.validate_result(json.load(open(out, encoding='utf-8'))), [])
        cal = os.path.join(self.dir, 'cal.json')
        self.assertEqual(R.main(['--runs', runs, '--calibrate', '--out', cal]), C.EXIT_PASS)
        self.assertEqual(R.main(['--runs', runs, '--calibration', cal]), C.EXIT_PASS)
        self.assertEqual(R.main(['--runs', os.path.join(self.dir, 'none.json')]),
                         C.EXIT_BAD_INPUT)
        self.assertEqual(R.main([]), C.EXIT_BAD_INPUT)
        self.assertEqual(R.main(['--bogus']), C.EXIT_BAD_INPUT)
        good = self.sample_log('good.jsonl')
        cut = self.path('cut.jsonl')
        with open(good, 'rb') as src, open(cut, 'wb') as dst:
            dst.write(src.read()[:-9])
        self.assertEqual(R.main(['--validate-frame-times', good]), C.EXIT_PASS)
        self.assertEqual(R.main(['--validate-frame-times', good, cut]), C.EXIT_BAD_INPUT)
        self.assertEqual(R.main(['--validate-frame-times', self.path('gone.jsonl')]),
                         C.EXIT_BAD_INPUT)


def file_manifest(directory, seed, cal_pairs, pairs, candidate_add_ms=0.0, scenes=('scene_a',)):
    """A manifest of synthetic telemetry FILES (frame_times logs, one PresentMon CSV)."""
    os.makedirs(directory, exist_ok=True)
    rng = random.Random(seed)
    runs = []

    def add(role, scene, pair, build, **kw):
        name = f'{role}-{scene}-{pair}.jsonl'
        write_frame_times(os.path.join(directory, name), frames(rng, count=500, **kw))
        runs.append(dict(id=f'{role}-{scene}-{pair}', role=role, scene=scene, pair=pair,
                         build=build, format='frame_times', file=name, source='host_present'))

    for scene in scenes:
        for pair in range(1, cal_pairs + 1):
            add(R.ROLE_CAL_A, scene, pair, 'b1')
            add(R.ROLE_CAL_B, scene, pair, 'b1')
        for pair in range(1, pairs + 1):
            add(R.ROLE_BASELINE, scene, pair, 'b1')
            add(R.ROLE_CANDIDATE, scene, pair, 'b2', add_ms=candidate_add_ms)
    return dict(schema=R.MANIFEST_SCHEMA, kind=R.MANIFEST_KIND, scenes=list(scenes),
                baseline_build='b1', candidate_build='b2', runs=runs)


class ResultDocument(unittest.TestCase):
    def test_keys_and_types(self):
        result, _ = verdict_of(manifest(seed=61, pairs=C.MIN_PAIRS))
        for key in C.REGRESSION_RESULT_KEYS:
            self.assertIn(key, result)
        json.dumps(result)          # serialisable as is
        self.assertEqual(result['alpha'], C.ALPHA_FAMILY)
        self.assertAlmostEqual(result['delta'], C.DELTA)

    def test_result_never_names_a_path_or_machine(self):
        result, _ = verdict_of(manifest(seed=62, pairs=C.MIN_PAIRS))
        text = json.dumps(result)
        self.assertNotIn('Users', text)
        self.assertNotIn(':\\', text)

    def test_delay_for_loss_is_a_nineteenth(self):
        self.assertAlmostEqual(C.delay_for_loss(19_000_000), 1_000_000, places=3)


def tail_frames(rng, tail_scale=1.0, count=1000):
    """A steady 16 ms median with a noisy 2 % tail: the median is quiet, the p99 is not."""
    out = [16.0 * math.exp(rng.gauss(0.0, 0.01)) for _ in range(count - 20)]
    out += [rng.uniform(25.0, 60.0) * tail_scale for _ in range(20)]
    return out


def tail_manifest(seed, pairs=10, candidate_scale=1.0, design=(4, 10)):
    rng = random.Random(seed)
    runs = []
    for scene in SCENES:
        for pair in range(1, pairs + 1):
            runs.append(run_entry(R.ROLE_BASELINE, scene, pair, 'build-base', tail_frames(rng)))
            runs.append(run_entry(R.ROLE_CANDIDATE, scene, pair, 'build-cand',
                                  tail_frames(rng, candidate_scale)))
    doc = dict(schema=R.MANIFEST_SCHEMA, kind=R.MANIFEST_KIND, scenes=list(SCENES),
               baseline_build='build-base', candidate_build='build-cand', runs=runs)
    if design:
        doc['design'] = dict(min_pairs=design[0], max_pairs=design[1])
    return doc


class SequentialDesign(unittest.TestCase):
    def test_alpha_is_spent_over_tests_and_looks(self):
        doc = manifest(seed=71, cal_pairs=0, pairs=5, run_sigma=0.002)
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        result, _ = verdict_of(doc)
        design = result['design']
        self.assertEqual(design['kind'], 'sequential')
        self.assertEqual(design['looks'], list(range(4, 11)))
        self.assertAlmostEqual(design['alpha_per_test'], C.ALPHA_FAMILY / (4 * 7))
        for row in result['metrics'].values():
            self.assertAlmostEqual(row['alpha_test'], C.ALPHA_FAMILY / 28)
        self.assertIn('union bound', design['rule'])

    def test_a_manifest_without_a_design_is_one_fixed_look(self):
        result, _ = verdict_of(manifest(seed=72, cal_pairs=0, pairs=12, run_sigma=0.002))
        self.assertEqual(result['design']['kind'], 'fixed')
        self.assertEqual(result['design']['looks'], [12])
        for row in result['metrics'].values():
            self.assertAlmostEqual(row['alpha_test'], C.ALPHA_FAMILY / 4)

    def test_early_pass_with_the_minimum_pairs(self):
        doc = manifest(seed=73, cal_pairs=0, pairs=4, run_sigma=0.001)
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_PASS, result['reasons'])
        self.assertEqual(code, C.EXIT_PASS)
        self.assertEqual(C.validate_result(result), [])
        self.assertIsNone(result['calibration'])
        for scene in SCENES:
            row = result['metrics'][f'{scene}:median_fps']
            self.assertTrue(row['sensitive'] and row['noninferior'])
            self.assertLessEqual(row['min_detectable_pct'], 5.0)

    def test_early_fail_with_the_minimum_pairs(self):
        base = statistics.median(frames(random.Random(3), run_sigma=0.0))
        doc = manifest(seed=74, cal_pairs=0, pairs=4, run_sigma=0.001,
                       candidate=dict(add_ms=C.delay_for_loss(base * 1e6) / 1e6))
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_FAIL, result['reasons'])
        self.assertEqual(code, C.EXIT_REGRESSION)

    def test_too_few_pairs_for_the_first_look_is_inconclusive(self):
        doc = manifest(seed=75, cal_pairs=0, pairs=3, run_sigma=0.001)
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(result['metrics'], {})

    def test_pairs_beyond_the_declared_maximum_are_not_analysed(self):
        doc = manifest(seed=76, cal_pairs=0, pairs=12, run_sigma=0.001)
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        result, _ = verdict_of(doc)
        for row in result['metrics'].values():
            self.assertEqual(row['pairs'], 10)

    def test_noisy_scene_at_the_cap_is_inconclusive_with_the_pairs_needed(self):
        doc = manifest(seed=77, cal_pairs=0, pairs=10, run_sigma=0.05)
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE, result['reasons'])
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('pairs are needed' in r for r in result['reasons']))
        needed = [row['pairs_needed'] for row in result['metrics'].values()
                  if row['gating'] and row['verdict'] == C.VERDICT_INCONCLUSIVE]
        self.assertTrue(needed and all(n is None or n > 10 for n in needed))

    def test_bad_mode_and_design_are_malformed(self):
        doc = manifest(seed=78, cal_pairs=0, pairs=4)
        for bad in (dict(mode='fast'), dict(design=dict(min_pairs=1, max_pairs=10)),
                    dict(design=dict(min_pairs=8, max_pairs=4)), dict(design=7),
                    dict(mode='self_proof')):
            with self.assertRaises(R.ManifestError):
                R.check_manifest(dict(doc, **bad))

    def test_scene_decided_rule(self):
        P, F, I = C.VERDICT_PASS, C.VERDICT_FAIL, C.VERDICT_INCONCLUSIVE

        def prog(aa, inj, valid=True, failed=False):
            return dict(aa=dict(verdict=aa, failed=failed), injection=dict(verdict=inj, valid=valid))
        sp = R.MODE_SELF_PROOF
        self.assertTrue(R.scene_decided(sp, prog(P, F)))
        self.assertFalse(R.scene_decided(sp, prog(P, F, valid=False)))
        self.assertFalse(R.scene_decided(sp, prog(I, F)))
        self.assertFalse(R.scene_decided(sp, prog(P, I)))
        self.assertTrue(R.scene_decided(sp, prog(P, P)))          # missed: also an answer
        self.assertTrue(R.scene_decided(sp, prog(F, I, failed=True)))
        ck = R.MODE_CHECK
        self.assertTrue(R.scene_decided(ck, dict(check=dict(verdict=P, failed=False))))
        self.assertTrue(R.scene_decided(ck, dict(check=dict(verdict=F, failed=True))))
        self.assertFalse(R.scene_decided(ck, dict(check=dict(verdict=I, failed=False))))
        self.assertFalse(R.scene_decided(ck, dict(check=dict(verdict=None, failed=False))))

    def test_pairs_needed_grows_with_the_looks(self):
        once = R.pairs_needed(0.02, 4, 1, start=4)
        seven = R.pairs_needed(0.02, 4, 7, start=4)
        self.assertGreater(seven, once)
        c = R.family_critical(4, seven, 7)
        self.assertLessEqual((c + C.Z_BETA) * 0.02 / math.sqrt(seven), C.DELTA)
        self.assertGreater((R.family_critical(4, seven - 1, 7) + C.Z_BETA) * 0.02
                           / math.sqrt(seven - 1), C.DELTA)
        self.assertIsNone(R.pairs_needed(5.0, 4, 7, start=4, limit=100))

    def test_f_critical_known_values(self):
        self.assertAlmostEqual(R.f_critical(0.05, 4, 20), 2.866, places=2)
        self.assertAlmostEqual(R.f_critical(0.01, 5, 10), 5.636, places=2)


class LowEnd(unittest.TestCase):
    def test_an_insensitive_low_end_does_not_block_pass_but_is_reported(self):
        result, code = verdict_of(tail_manifest(81, pairs=6))
        self.assertEqual(result['verdict'], C.VERDICT_PASS, result['reasons'])
        self.assertEqual(code, C.EXIT_PASS)
        self.assertEqual(result['reasons'], [])
        for scene in SCENES:
            low = result['metrics'][f'{scene}:inverse_p99']
            self.assertFalse(low['gating'])
            self.assertFalse(low['sensitive'])
            self.assertEqual(low['verdict'], C.VERDICT_INCONCLUSIVE)
            entry = result['low_end'][scene]
            self.assertIn('low-end drops beyond', entry['statement'])
            self.assertGreater(entry['sensitivity_pct'], 5.0)
            self.assertFalse(entry['reaches_5_percent'])
            self.assertAlmostEqual(entry['sensitivity_pct'], low['min_detectable_pct'])
            self.assertTrue(result['metrics'][f'{scene}:median_fps']['gating'])
        text = R.describe(result)
        self.assertIn('low-end drops beyond', text)
        self.assertIn('[info]', text)

    def test_a_significant_low_end_loss_still_fails(self):
        result, code = verdict_of(tail_manifest(82, pairs=10, candidate_scale=1.5))
        self.assertEqual(result['verdict'], C.VERDICT_FAIL, result['reasons'])
        self.assertEqual(code, C.EXIT_REGRESSION)
        for scene in SCENES:
            self.assertEqual(result['metrics'][f'{scene}:inverse_p99']['verdict'],
                             C.VERDICT_FAIL)
            self.assertNotIn(scene, result['low_end'])

    def test_a_quiet_low_end_states_a_sensitivity_within_five_percent(self):
        doc = manifest(seed=83, cal_pairs=0, pairs=10, run_sigma=0.001)
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        result, _ = verdict_of(doc)
        for scene in SCENES:
            self.assertIn(scene, result['low_end'])
            self.assertIn('would have been detected', result['low_end'][scene]['statement'])


class OptionalCalibration(unittest.TestCase):
    def merged(self, cal_sigma, run_sigma):
        cal = manifest(seed=91, cal_pairs=30, pairs=0, run_sigma=cal_sigma)
        doc = manifest(seed=92, cal_pairs=0, pairs=8, run_sigma=run_sigma)
        doc['runs'] += cal['runs']
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        return doc

    def test_no_calibration_is_not_a_problem(self):
        result, _ = verdict_of(manifest(seed=93, cal_pairs=0, pairs=10, run_sigma=0.001))
        self.assertIsNone(result['calibration'])
        self.assertEqual(result['verdict'], C.VERDICT_PASS, result['reasons'])

    def test_a_calibration_of_the_same_build_pools_the_variance(self):
        result, _ = verdict_of(self.merged(0.004, 0.004))
        row = result['metrics']['scene_a:median_fps']
        self.assertEqual(row['spread_source'], 'pooled with the stored calibration')
        self.assertEqual(row['df'], 8 + 30 - 2)
        self.assertTrue(result['calibration']['valid'])

    def test_pairs_noisier_than_the_calibration_keep_their_own_spread(self):
        result, _ = verdict_of(self.merged(0.002, 0.03))
        row = result['metrics']['scene_a:median_fps']
        self.assertTrue(row['spread_source'].startswith('own pairs'), row['spread_source'])
        self.assertEqual(row['df'], 8 - 1)
        self.assertAlmostEqual(row['s'], row['s_obs'])

    def test_a_supplied_bad_calibration_is_an_integrity_problem(self):
        doc = manifest(seed=94, cal_pairs=5, pairs=6, run_sigma=0.001)
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        result, _ = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertFalse(result['calibration']['valid'])


class SelfProof(unittest.TestCase):
    def test_a_proved_self_proof_passes_with_separate_verdicts(self):
        result, code = verdict_of(proof_manifest(seed=101, rounds=5))
        self.assertEqual(result['verdict'], C.VERDICT_PASS, result['reasons'])
        self.assertEqual(code, C.EXIT_PASS)
        self.assertEqual(C.validate_result(result), [])
        self.assertEqual(result['mode'], 'self_proof')
        self.assertEqual(result['aa']['verdict'], C.VERDICT_PASS)
        self.assertEqual(result['injection']['proof'], 'caught')
        self.assertTrue(result['injection']['valid'])
        for scene in SCENES:
            self.assertEqual(result['metrics'][f'aa:{scene}:median_fps']['verdict'],
                             C.VERDICT_PASS)
            self.assertEqual(result['metrics'][f'injection:{scene}:median_fps']['verdict'],
                             C.VERDICT_FAIL)
            item = result['injection']['scenes'][scene]
            self.assertEqual(item['validity'], R.VALID_POINT)
            self.assertTrue(4.5 <= item['achieved_pct'] <= 5.5)
            self.assertTrue(item['caught'])
        self.assertIn('proof caught', R.describe(result))
        self.assertIn('point in band', R.describe(result))

    def test_a_false_alarm_on_identical_builds_fails(self):
        result, code = verdict_of(proof_manifest(seed=102, rounds=6, repeat_add_ms=0.8))
        self.assertEqual(result['verdict'], C.VERDICT_FAIL, result['reasons'])
        self.assertEqual(code, C.EXIT_REGRESSION)
        self.assertEqual(result['aa']['verdict'], C.VERDICT_FAIL)
        self.assertTrue(any('A/A' in r for r in result['reasons']))

    def test_a_slowdown_that_is_not_five_percent_is_not_a_proof(self):
        result, code = verdict_of(proof_manifest(seed=103, rounds=6, add_ms=0.3))
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertEqual(result['injection']['proof'], 'invalid')
        self.assertFalse(result['injection']['valid'])
        self.assertTrue(any('not shown to be 5 %' in r for r in result['reasons']))

    def test_a_missed_injection_is_reported_not_passed(self):
        doc = proof_manifest(seed=104, rounds=6, run_sigma=0.002)
        for run in doc['runs']:
            if run['role'] == R.ROLE_INJECTED:       # the delay "has no effect" on the frames
                run['frame_ms'] = [x - 0.76 for x in run['frame_ms']]
        result, code = verdict_of(doc)
        self.assertNotEqual(result['verdict'], C.VERDICT_PASS)
        self.assertNotEqual(code, C.EXIT_PASS)

    def test_a_noisy_self_proof_is_inconclusive_at_the_cap(self):
        result, code = verdict_of(proof_manifest(seed=105, rounds=10, run_sigma=0.05))
        self.assertNotEqual(result['verdict'], C.VERDICT_PASS, result['reasons'])
        self.assertNotEqual(code, C.EXIT_PASS)

    def test_the_achieved_loss_interval_can_make_a_proof_valid(self):
        # Point estimate 5.7 % (outside 4.5..5.5) but the interval covers 5 % inside 4..6 %.
        est = dict(pairs=8, loss=0.057, low=0.045, high=0.059)
        self.assertEqual(R.proof_validity(est), R.VALID_INTERVAL)
        self.assertEqual(R.proof_validity(dict(pairs=8, loss=0.049, low=0.03, high=0.07)),
                         R.VALID_POINT)
        self.assertEqual(R.proof_validity(dict(pairs=8, loss=0.057, low=0.043, high=0.071)),
                         R.VALID_OUTSIDE)
        self.assertEqual(R.proof_validity(dict(pairs=8, loss=0.02, low=0.01, high=0.03)),
                         R.VALID_OUTSIDE)
        self.assertEqual(R.proof_validity(None), R.VALID_NONE)
        self.assertIsNone(R.achieved_estimate([0.05]))
        est = R.achieved_estimate([0.05, 0.052, 0.051, 0.049])
        self.assertLess(est['low'], est['loss'])
        self.assertLess(est['loss'], est['high'])

    def test_retune_delay_solves_the_model(self):
        f, d = 16_000_000, 1_000_000
        achieved = 0.5 * d / (f + d)                  # half of the delay reaches the frame rate
        new = R.retune_delay(d, f, achieved)
        self.assertAlmostEqual(new, 0.05 * f / (0.5 - 0.05), delta=2)
        self.assertAlmostEqual(0.5 * new / (f + new), 0.05, places=4)
        self.assertIsNone(R.retune_delay(d, f, 0.001))
        self.assertIsNone(R.retune_delay(d, f, 0.0))
        self.assertIsNone(R.retune_delay(0, f, 0.03))

    def test_an_off_target_injection_advises_one_retune(self):
        doc = proof_manifest(seed=106, rounds=4, add_ms=0.3)       # about 1.8 % loss
        result, _ = verdict_of(doc)
        advice = result['progress']['scene_a']['injection']['retune']
        self.assertIsNotNone(advice)
        self.assertGreater(advice['new_delay_ns'], doc['injection']['delays']['scene_a']['delay_ns'])
        done = proof_manifest(seed=106, rounds=4, add_ms=0.3)
        done['injection']['delays']['scene_a']['restarts'] = [
            dict(at_pair=3, old_delay_ns=1, new_delay_ns=2, discarded_runs=3)]
        result, _ = verdict_of(done)
        self.assertIsNone(result['progress']['scene_a']['injection']['retune'])
        self.assertIn('retuned', R.describe(result))

    def test_a_second_restart_is_an_integrity_problem(self):
        doc = proof_manifest(seed=107, rounds=5)
        doc['injection']['delays']['scene_a']['restarts'] = [dict(at_pair=3), dict(at_pair=5)]
        result, _ = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertTrue(any('more than once' in r for r in result['reasons']))

    def test_discarded_runs_are_skipped_and_counted(self):
        doc = proof_manifest(seed=108, rounds=6)
        for run in doc['runs']:
            if run['role'] == R.ROLE_INJECTED and run['scene'] == 'scene_a' and run['pair'] <= 2:
                run['discarded'] = True
        result, _ = verdict_of(doc)
        self.assertEqual(result['accounting']['discarded_runs']['scene_a'], 2)
        self.assertEqual(result['metrics']['injection:scene_a:median_fps']['pairs'], 4)
        self.assertEqual(result['metrics']['aa:scene_a:median_fps']['pairs'], 6)
        self.assertFalse(any('lacks a usable run' in r for r in result['reasons']))

    def test_a_failed_shared_baseline_run_costs_both_comparisons(self):
        doc = proof_manifest(seed=109, rounds=5)
        for run in doc['runs']:
            if run['role'] == R.ROLE_BASELINE and run['scene'] == 'scene_a' and run['pair'] == 1:
                run['ok'] = False
        result, _ = verdict_of(doc)
        self.assertEqual(result['metrics']['aa:scene_a:median_fps']['pairs'], 4)
        self.assertEqual(result['metrics']['injection:scene_a:median_fps']['pairs'], 4)

    def test_the_two_comparisons_share_the_first_baseline_run(self):
        doc = proof_manifest(seed=110, rounds=4)
        ev = R.gather(doc, '.')
        a = ev.pairs[('aa', 'scene_a')][0]
        i = ev.pairs[('injection', 'scene_a')][0]
        self.assertIs(a[1], i[1])


class Simulation(unittest.TestCase):
    def test_repeated_looks_do_not_inflate_the_false_alarm_rate(self):
        out = R.simulate_sequences(300, seed=5)
        self.assertEqual(out['sequences'], 300)
        self.assertLessEqual(out['either'] / 300.0, C.ALPHA_FAMILY * 2)
        self.assertLessEqual(out['aa_alarm'], out['either'])
        self.assertGreater(out['corr'], 0.4)             # the shared run A1 correlates them
        self.assertLess(out['corr'], 0.6)

    def test_an_injected_five_percent_loss_is_caught_in_simulation(self):
        out = R.simulate_sequences(100, seed=6, loss=0.05)
        self.assertGreaterEqual(out['caught'], 90)
        self.assertLess(statistics.fmean(out['scene_stops']), 8)

    def test_clopper_pearson_interval(self):
        low, high = R.binomial_interval(0, 100)
        self.assertEqual(low, 0.0)
        self.assertAlmostEqual(high, 0.0362, places=3)
        low, high = R.binomial_interval(50, 100)
        self.assertAlmostEqual(low, 0.398, places=3)
        self.assertAlmostEqual(high, 0.602, places=3)
        self.assertEqual(R.binomial_interval(10, 10)[1], 1.0)


def _write_report(path, metrics_complete=True, shutdown_complete=True, frame_gen=False,
                  generated_frames=0):
    doc = dict(
        schema_version=2,
        exit=dict(code=0, reason='normal_end', metrics_complete=metrics_complete,
                  shutdown_complete=shutdown_complete),
        settings=dict(initial=dict(frame_gen=frame_gen, allow_speed_up=True),
                      final=dict(frame_gen=frame_gen, allow_speed_up=True)),
        summary=dict(frames=400, generated_frames=generated_frames))
    with open(path, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(doc, handle, indent=2)


class Issue6RejectionAndProofGuards(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def _file_manifest_with_reports(self, seed=201, pairs=10, cache_state='warm',
                                    report_kwargs=None):
        doc = file_manifest(self.dir, seed=seed, cal_pairs=0, pairs=pairs)
        doc['cache_state'] = cache_state
        for entry in doc['runs']:
            entry['cache_state'] = cache_state
            entry['settings'] = 'baseline_uncapped_real'
            entry['device'] = 'synthetic_gpu'
            rep_name = entry['id'] + '.report.json'
            kw = (report_kwargs or {}).get(entry['id'], {})
            _write_report(os.path.join(self.dir, rep_name), **kw)
            entry['report'] = rep_name
        return doc

    def test_uniform_cache_state_and_complete_reports_pass_with_ordering(self):
        doc = self._file_manifest_with_reports(seed=201, pairs=10)
        doc['design'] = dict(min_pairs=4, max_pairs=10)
        result, code = verdict_of_dir(doc, self.dir)
        self.assertEqual(result['verdict'], C.VERDICT_PASS, result['reasons'])
        self.assertEqual(code, C.EXIT_PASS)
        self.assertEqual(result['accounting']['cache_state'], 'warm')
        ordering = result['accounting']['paired_ordering']['scene_a']
        self.assertEqual(ordering[:4], ['baseline:p1', 'candidate:p1', 'baseline:p2', 'candidate:p2'])

    def test_incomplete_metrics_inline_and_report_are_rejected(self):
        doc = manifest(seed=202, pairs=10)
        doc['runs'][0]['metrics_complete'] = False
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('metrics_complete' in r for r in result['reasons']))

        doc2 = self._file_manifest_with_reports(
            seed=203, pairs=10, report_kwargs={'candidate-scene_a-1': dict(metrics_complete=False)})
        result2, code2 = verdict_of_dir(doc2, self.dir)
        self.assertEqual(result2['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code2, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('metrics_complete' in r for r in result2['reasons']))

    def test_incomplete_shutdown_inline_and_report_are_rejected(self):
        doc = manifest(seed=204, pairs=10)
        doc['runs'][1]['shutdown_complete'] = False
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('shutdown_complete' in r for r in result['reasons']))

        doc2 = self._file_manifest_with_reports(
            seed=205, pairs=10, report_kwargs={'baseline-scene_a-2': dict(shutdown_complete=False)})
        result2, code2 = verdict_of_dir(doc2, self.dir)
        self.assertEqual(result2['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code2, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('shutdown_complete' in r for r in result2['reasons']))

    def test_generated_frame_inflation_is_rejected(self):
        for mut in (
            dict(frame_gen=True),
            dict(generated_frames=120),
            dict(frame_gen_multiplier=2),
            dict(source='generated_frames'),
        ):
            doc = manifest(seed=206, pairs=10)
            doc['runs'][0].update(mut)
            result, code = verdict_of(doc)
            self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE, mut)
            self.assertEqual(code, C.EXIT_INCONCLUSIVE)
            self.assertTrue(any('generated-frame inflation' in r for r in result['reasons']))

        doc2 = self._file_manifest_with_reports(
            seed=207, pairs=10, report_kwargs={'candidate-scene_a-3': dict(frame_gen=True)})
        result2, code2 = verdict_of_dir(doc2, self.dir)
        self.assertEqual(result2['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code2, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('generated-frame inflation' in r for r in result2['reasons']))

    def test_mixed_cold_warm_within_pair_is_rejected(self):
        doc = manifest(seed=208, pairs=10)
        for run in doc['runs']:
            if run['scene'] == 'scene_a' and run['pair'] == 1:
                run['cache_state'] = 'cold' if run['role'] == R.ROLE_BASELINE else 'warm'
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('cache_state differ' in r for r in result['reasons']))

    def test_mixed_cold_warm_across_pairs_in_scene_is_rejected(self):
        doc = manifest(seed=209, pairs=10)
        for run in doc['runs']:
            if run['scene'] == 'scene_a' and run['role'] in (R.ROLE_BASELINE, R.ROLE_CANDIDATE):
                run['cache_state'] = 'cold' if run['pair'] == 1 else 'warm'
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('mixes cache states' in r for r in result['reasons']))

    def test_pinned_cache_state_mismatch_is_rejected(self):
        doc = manifest(seed=210, pairs=10)
        doc['cache_state'] = 'warm'
        for run in doc['runs']:
            run['cache_state'] = 'warm'
        doc['runs'][-1]['cache_state'] = 'cold'
        result, code = verdict_of(doc)
        self.assertEqual(result['verdict'], C.VERDICT_INCONCLUSIVE)
        self.assertEqual(code, C.EXIT_INCONCLUSIVE)
        self.assertTrue(any('does not match pinned' in r for r in result['reasons']))


if __name__ == '__main__':
    unittest.main(verbosity=1)
