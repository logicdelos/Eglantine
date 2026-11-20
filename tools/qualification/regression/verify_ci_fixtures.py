#!/usr/bin/env python3
"""Deterministic synthetic CI fixture generator and verifier for Issue #6.

LOGIC AND CONTRACT VERIFICATION ONLY (marked evidence="synthetic").
Generates checked-in synthetic --frame-times JSONL logs, report.json files, and
manifests under tools/qualification/regression/fixtures/, then invokes
regress_stats.py on every positive and negative control and asserts verdicts
and exit codes.
"""

import argparse
import json
import math
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REGRESS_STATS = os.path.join(HERE, 'regress_stats.py')
FIXTURES_DIR = os.path.join(HERE, 'fixtures')

sys.path.insert(0, HERE)
import regress_contract as C  # noqa: E402

SCENE = 'synthetic_scene'
BASE_BUILD = 'synthetic-baseline-v1'
CAND_BUILD = 'synthetic-candidate-v1'
SETTINGS_PIN = 'baseline_uncapped_real'
DEVICE_PIN = 'synthetic-ci-gpu-driver-0'
FRAMES_PER_RUN = 400
BASE_MS = 16.0
BASE_NS = 16_000_000
DELAY_5PCT_NS = round(C.delay_for_loss(BASE_NS))  # 842105 ns (~5.0% loss)


def _synth_frames_ms(rng, add_ms=0.0, run_sigma=0.002, frame_sigma=0.015, count=FRAMES_PER_RUN):
    factor = math.exp(rng.gauss(0.0, run_sigma))
    out = []
    for _ in range(count):
        val = BASE_MS * factor * math.exp(rng.gauss(0.0, frame_sigma))
        if rng.random() < 0.04:
            val *= 2.5
        out.append(val + add_ms)
    return out


def _write_frame_times(path, frame_ms, start_ns=1_000_000_000, truncate=False):
    times = [start_ns]
    now = start_ns
    for ms in frame_ms:
        now += int(round(ms * 1e6))
        times.append(now)
    lines = [json.dumps(dict(schema=1, kind='frame_times', source='host_present',
                             clock='steady_ns', capacity=len(times)), separators=(',', ':'))]
    lines += [json.dumps(dict(id=i, t=t), separators=(',', ':')) for i, t in enumerate(times)]
    if not truncate:
        lines.append(json.dumps(dict(end=True, recorded=len(times), lost=0, complete=True),
                                separators=(',', ':')))
    with open(path, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write('\n'.join(lines) + '\n')


def _write_report(path, metrics_complete=True, shutdown_complete=True, frame_gen=False,
                  generated_frames=0):
    doc = dict(
        schema_version=2,
        exit=dict(code=0, reason='normal_end', metrics_complete=metrics_complete,
                  shutdown_complete=shutdown_complete),
        settings=dict(initial=dict(frame_gen=frame_gen, allow_speed_up=True,
                                   use_speed_limit=False, use_vsync=0),
                      final=dict(frame_gen=frame_gen, allow_speed_up=True,
                                 use_speed_limit=False, use_vsync=0)),
        summary=dict(frames=FRAMES_PER_RUN, generated_frames=generated_frames))
    with open(path, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(doc, handle, indent=2)
        handle.write('\n')


def _write_json(path, doc):
    with open(path, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(doc, handle, indent=2)
        handle.write('\n')


def generate_fixtures(out_dir=FIXTURES_DIR):
    logs_dir = os.path.join(out_dir, 'logs')
    os.makedirs(logs_dir, exist_ok=True)
    rng = random.Random(20261119)
    add_5pct_ms = DELAY_5PCT_NS / 1e6

    ok_report = 'logs/ok.report.json'
    _write_report(os.path.join(out_dir, ok_report))
    _write_report(os.path.join(out_dir, 'logs/bad_incomplete_metrics.report.json'),
                  metrics_complete=False)
    _write_report(os.path.join(out_dir, 'logs/bad_incomplete_shutdown.report.json'),
                  shutdown_complete=False)
    _write_report(os.path.join(out_dir, 'logs/bad_frame_gen.report.json'),
                  frame_gen=True, generated_frames=400)

    def make_run(role, pair, build, add_ms=0.0, tag='ok', truncate=False):
        fname = f'logs/{role}_{pair}_{tag}.jsonl'
        _write_frame_times(os.path.join(out_dir, fname),
                           _synth_frames_ms(rng, add_ms=add_ms), truncate=truncate)
        return dict(id=f'{role}-{SCENE}-p{pair}', role=role, scene=SCENE, pair=pair,
                    build=build, format='frame_times', file=fname, source='host_present',
                    synthetic=True, cache_state='warm', settings=SETTINGS_PIN,
                    device=DEVICE_PIN, report=ok_report)

    rounds = 5
    aa_runs = []
    self_proof_runs = []
    check_5pct_runs = []

    # Balanced rotating triplet order for self_proof (A1, A2, I) and AB/BA for check
    triplet_orders = (
        ('baseline', 'baseline_repeat', 'injected'),
        ('baseline_repeat', 'injected', 'baseline'),
        ('injected', 'baseline', 'baseline_repeat'),
        ('baseline', 'injected', 'baseline_repeat'),
        ('injected', 'baseline_repeat', 'baseline'),
    )
    for pair in range(1, rounds + 1):
        a1 = make_run('baseline', pair, BASE_BUILD)
        a2_aa = dict(a1)
        a2_aa.update(id=f'candidate-{SCENE}-p{pair}', role='candidate', build=BASE_BUILD)
        # Separate A/A repeat run for self-proof and A/A check
        a2 = make_run('baseline_repeat', pair, BASE_BUILD)
        cand_aa = dict(a2)
        cand_aa.update(id=f'candidate-{SCENE}-p{pair}', role='candidate', build=BASE_BUILD)
        if pair % 2 == 1:
            aa_runs.extend([a1, cand_aa])
        else:
            aa_runs.extend([cand_aa, a1])

        inj = make_run('injected', pair, BASE_BUILD, add_ms=add_5pct_ms)
        inj['delay_ns'] = DELAY_5PCT_NS
        by_role = {'baseline': a1, 'baseline_repeat': a2, 'injected': inj}
        for rname in triplet_orders[pair - 1]:
            self_proof_runs.append(dict(by_role[rname]))

        cand_slow = dict(inj)
        cand_slow.update(id=f'candidate-{SCENE}-p{pair}', role='candidate', build=CAND_BUILD)
        if pair % 2 == 1:
            check_5pct_runs.extend([dict(a1), cand_slow])
        else:
            check_5pct_runs.extend([cand_slow, dict(a1)])

    _write_frame_times(os.path.join(out_dir, 'logs/truncated.jsonl'),
                       _synth_frames_ms(rng), truncate=True)

    design = dict(min_pairs=4, max_pairs=10)
    aa_doc = dict(schema=1, kind='regression_runs', mode='check', scenes=[SCENE],
                  baseline_build=BASE_BUILD, candidate_build=BASE_BUILD,
                  cache_state='warm', design=design, runs=aa_runs)
    _write_json(os.path.join(out_dir, 'aa_control_runs.json'), aa_doc)

    sp_doc = dict(
        schema=1, kind='regression_runs', mode='self_proof', scenes=[SCENE],
        baseline_build=BASE_BUILD, candidate_build=BASE_BUILD,
        cache_state='warm', design=design,
        injection=dict(target='cpu-present',
                       delays={SCENE: dict(delay_ns=DELAY_5PCT_NS, frame_ns=BASE_NS)}),
        runs=self_proof_runs)
    _write_json(os.path.join(out_dir, 'self_proof_5pct_runs.json'), sp_doc)

    fail_doc = dict(schema=1, kind='regression_runs', mode='check', scenes=[SCENE],
                    baseline_build=BASE_BUILD, candidate_build=CAND_BUILD,
                    cache_state='warm', design=design, runs=check_5pct_runs)
    _write_json(os.path.join(out_dir, 'check_5pct_regression_runs.json'), fail_doc)

    # Negative control 1: incomplete metrics
    neg_metrics = json.loads(json.dumps(aa_doc))
    neg_metrics['runs'][1]['report'] = 'logs/bad_incomplete_metrics.report.json'
    _write_json(os.path.join(out_dir, 'neg_incomplete_metrics_runs.json'), neg_metrics)

    # Negative control 2: incomplete shutdown
    neg_shut = json.loads(json.dumps(aa_doc))
    neg_shut['runs'][1]['report'] = 'logs/bad_incomplete_shutdown.report.json'
    _write_json(os.path.join(out_dir, 'neg_incomplete_shutdown_runs.json'), neg_shut)

    # Negative control 3: generated-frame inflation
    neg_gen = json.loads(json.dumps(aa_doc))
    neg_gen['runs'][1]['report'] = 'logs/bad_frame_gen.report.json'
    _write_json(os.path.join(out_dir, 'neg_generated_frames_runs.json'), neg_gen)

    # Negative control 4: mixed cold/warm samples
    neg_mix = json.loads(json.dumps(aa_doc))
    del neg_mix['cache_state']
    neg_mix['runs'][0]['cache_state'] = 'cold'
    neg_mix['runs'][1]['cache_state'] = 'cold'
    _write_json(os.path.join(out_dir, 'neg_mixed_cache_runs.json'), neg_mix)

    # Negative control 5: truncated --frame-times log
    neg_trunc = json.loads(json.dumps(aa_doc))
    neg_trunc['runs'][0]['file'] = 'logs/truncated.jsonl'
    _write_json(os.path.join(out_dir, 'neg_truncated_log_runs.json'), neg_trunc)


def verify_fixtures(fixtures_dir=FIXTURES_DIR):
    cases = [
        ('aa_control_runs.json', C.EXIT_PASS, C.VERDICT_PASS, None),
        ('self_proof_5pct_runs.json', C.EXIT_PASS, C.VERDICT_PASS, None),
        ('check_5pct_regression_runs.json', C.EXIT_REGRESSION, C.VERDICT_FAIL, None),
        ('neg_incomplete_metrics_runs.json', C.EXIT_INCONCLUSIVE, C.VERDICT_INCONCLUSIVE,
         'metrics_complete'),
        ('neg_incomplete_shutdown_runs.json', C.EXIT_INCONCLUSIVE, C.VERDICT_INCONCLUSIVE,
         'shutdown_complete'),
        ('neg_generated_frames_runs.json', C.EXIT_INCONCLUSIVE, C.VERDICT_INCONCLUSIVE,
         'generated-frame inflation'),
        ('neg_mixed_cache_runs.json', C.EXIT_INCONCLUSIVE, C.VERDICT_INCONCLUSIVE,
         'mixes cache states'),
        ('neg_truncated_log_runs.json', C.EXIT_BAD_INPUT, C.VERDICT_INVALID, 'truncated'),
    ]
    for name, want_code, want_verdict, want_substr in cases:
        manifest_path = os.path.join(fixtures_dir, name)
        out_path = os.path.join(os.environ.get('TEMP', '/tmp'), f'ci_verify_{name}')
        proc = subprocess.run(
            [sys.executable, '-B', REGRESS_STATS, '--runs', manifest_path, '--out', out_path],
            capture_output=True, text=True, check=False)
        if proc.returncode != want_code:
            print(f'FAIL {name}: exit={proc.returncode} expected={want_code}\n{proc.stdout}\n{proc.stderr}')
            return 1
        with open(out_path, encoding='utf-8') as handle:
            res = json.load(handle)
        problems = C.validate_result(res)
        if problems:
            print(f'FAIL {name}: result schema invalid: {problems}')
            return 1
        if res['verdict'] != want_verdict:
            print(f'FAIL {name}: verdict={res["verdict"]} expected={want_verdict}')
            return 1
        if want_substr and not any(want_substr in r for r in res['reasons']):
            print(f'FAIL {name}: missing {want_substr!r} in reasons={res["reasons"]}')
            return 1
        if name == 'self_proof_5pct_runs.json':
            if res['aa']['verdict'] != C.VERDICT_PASS or res['injection']['proof'] != 'caught':
                print(f'FAIL {name}: aa/injection unexpected: {res.get("aa")}, {res.get("injection")}')
                return 1
        print(f'ok   {name}: exit={proc.returncode} verdict={res["verdict"]} evidence={res["evidence"]}')
    print(f'{len(cases)} of {len(cases)} synthetic CI fixture checks passed')
    return 0


def main():
    parser = argparse.ArgumentParser(description='Build and/or verify synthetic Issue #6 CI fixtures.')
    parser.add_argument('--write', action='store_true', help='(re)generate checked-in fixtures/')
    args = parser.parse_args()
    if args.write:
        generate_fixtures(FIXTURES_DIR)
    return verify_fixtures(FIXTURES_DIR)


if __name__ == '__main__':
    raise SystemExit(main())
