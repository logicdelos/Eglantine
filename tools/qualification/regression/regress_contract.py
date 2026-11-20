"""Frozen constants and schemas shared by the 0.6 lab tools (slice A of plan_0.6).

Owned by slice A. Slices E and F import this module and never redefine a value from it:
  speed_matrix.py --regression, regress_stats.py   (slice E)
  frame_compare.py, gate_table.py, make_release.py, publish_release.py, release_gate.py (slice F)
Pure data and small pure functions. No file access except where a function takes a path, no
network, no emulator run. The format of the capture file itself is documented in
plans/contracts_0.6.md; this module only holds what the lab needs to recognise it.
"""

import json
import math

# ---- exit codes of every regression and comparison tool ------------------------------------
EXIT_PASS = 0           # every required check passed
EXIT_REGRESSION = 1     # a measured regression, a picture difference or a census failure
EXIT_BAD_INPUT = 2      # bad arguments or unreadable / malformed input files
EXIT_INCONCLUSIVE = 3   # evidence missing, incomparable, invalid calibration, or too noisy
EXIT_EXECUTION = 4      # a tool or I/O failure while running
EXIT_INTERRUPTED = 130  # Ctrl-C; partial output is marked incomplete, never a PASS

VERDICT_PASS = "PASS"
VERDICT_FAIL = "FAIL"
VERDICT_INCONCLUSIVE = "INCONCLUSIVE"
VERDICT_INVALID = "INVALID"
VERDICTS = (VERDICT_PASS, VERDICT_FAIL, VERDICT_INCONCLUSIVE, VERDICT_INVALID)
VERDICT_EXIT = {VERDICT_PASS: EXIT_PASS, VERDICT_FAIL: EXIT_REGRESSION,
                VERDICT_INCONCLUSIVE: EXIT_INCONCLUSIVE, VERDICT_INVALID: EXIT_BAD_INPUT}

# ---- picture comparison (0.6 gate) ----------------------------------------------------------
# Same measure as lab_kit.imgdiff: grayscale, resized to 160 x 90, mean absolute difference / 255.
IMGDIFF_WIDTH = 160
IMGDIFF_HEIGHT = IMGDIFF_WIDTH * 9 // 16
IMGDIFF_LIMIT = 0.06            # strict: a pair passes only when diff < IMGDIFF_LIMIT
SAME_BUILD_LIMIT = 0.0          # two replays of the same capture on the same build: identical
# Proposed for 0.7 (plan_fastpath 10.1); recorded, not used as a gate in 0.6.
PROPOSED_MAE_LIMIT = 1.0 / 255.0
PROPOSED_OUTLIER_FRACTION = 0.001
PROPOSED_OUTLIER_CHANNEL_DELTA = 2.0 / 255.0

# ---- replay image directory -----------------------------------------------------------------
REPLAY_FRAMES_FILE = "frames.jsonl"
REPLAY_LINE_SCHEMA = 1
REPLAY_LINE_KEYS = ("schema", "frame_id", "width", "height", "live_checksum",
                    "replay_checksum", "identical", "semantic_mismatches", "replay_ns",
                    "live_png", "replay_png")
FRAME_DIFF_FILE = "frame_diff.jsonl"
FRAME_DIFF_SCHEMA = 1
FRAME_DIFF_KEYS = ("schema", "frame_id", "imgdiff", "limit", "result", "live_png", "replay_png")
FRAME_RESULTS = ("pass", "fail", "missing", "size_mismatch")

# ---- frame timing log (--frame-times) -------------------------------------------------------
FRAME_TIMES_SCHEMA = 1
FRAME_TIMES_SOURCES = ("host_present", "replay_frame")

# ---- regression statistics (plan 6.1, frozen) ------------------------------------------------
ALPHA_FAMILY = 0.01             # false regression target for the whole family
BETA_PLAN = 0.05                # planning miss rate for a real injected loss
MIN_PAIRS = 10                  # smallest FIXED-sample A/B schedule; the sequential design (the
                                # default of speed_matrix) uses SEQ_MIN_PAIRS..SEQ_MAX_PAIRS of
                                # regress_stats instead
MIN_CALIBRATION_PAIRS = 30      # A/A pairs before a spread estimate is trusted (--calibrate-only)
SCENES_DEFAULT = 3
METRICS_DEFAULT = ("median_fps", "inverse_p99")   # median and low end
FAMILY_K_DEFAULT = SCENES_DEFAULT * len(METRICS_DEFAULT)   # 6 one-sided tests
INJECTED_LOSS = 0.05
DELTA = -math.log(1.0 - INJECTED_LOSS)           # 0.051293: the effect that must be caught
C_PLANNING = 3.3                # conservative critical value for K=6, N>=30 (planning only)
Z_BETA = 1.645
INJECTION_BAND = (0.045, 0.055)  # achieved loss the calibration must land in (5.0 +- 0.5 points)


def delay_for_loss(frame_ns, loss=INJECTED_LOSS):
    """Added delay that lowers throughput by `loss`: T/(1-loss) - T, i.e. T/19 for 5 %."""
    return frame_ns * loss / (1.0 - loss)


def planned_pairs(spread, c=C_PLANNING, z_beta=Z_BETA, delta=DELTA):
    """n = ceil(((c + z_beta) * spread / delta)^2), never below MIN_PAIRS."""
    if not spread > 0:
        raise ValueError("spread must be positive")
    return max(MIN_PAIRS, math.ceil(((c + z_beta) * spread / delta) ** 2))


# ---- capture files never leave the machine ---------------------------------------------------
CAPTURE_MAGIC = b"EGLCAP\x00\x00"
CAPTURE_SUFFIXES = (".egcap", ".egcap.partial")
PRIVATE_MARKER = ".egl-private"                 # written into every replay image directory
RELEASE_STAGING_MARKER = ".egl-release-staging"  # written by make_release into its staging dir
PRIVATE_DIR_FILE_PREFIXES = ("live_", "replay_")  # pictures that come from a game
PRIVATE_DIR_FILES = (REPLAY_FRAMES_FILE, FRAME_DIFF_FILE)


def capture_reason(name, head=b""):
    """Why a file must not be published, or None. `name` is any path; `head` its first bytes.

    Catches the suffix, a renamed file (by magic) and a private directory marker, so a capture
    cannot be shipped by renaming it or by putting it inside an archive entry of any name.
    """
    lowered = name.replace("\\", "/").lower()
    base = lowered.rsplit("/", 1)[-1]
    if lowered.endswith(CAPTURE_SUFFIXES):
        return "capture file suffix"
    if head[:len(CAPTURE_MAGIC)] == CAPTURE_MAGIC:
        return "capture file magic"
    if base == PRIVATE_MARKER:
        return "private directory marker"
    return None


# ---- result documents ----------------------------------------------------------------------
REGRESSION_RESULT_SCHEMA = 1
REGRESSION_RESULT_KEYS = ("schema", "verdict", "family_k", "alpha", "delta", "calibration",
                          "metrics", "pictures", "census", "reasons")


def validate_result(doc):
    """Return a list of problems with a regression result document (empty list = valid)."""
    problems = []
    if not isinstance(doc, dict):
        return ["result is not an object"]
    for key in REGRESSION_RESULT_KEYS:
        if key not in doc:
            problems.append(f"missing key {key}")
    if doc.get("schema") != REGRESSION_RESULT_SCHEMA:
        problems.append("unsupported schema")
    if doc.get("verdict") not in VERDICTS:
        problems.append("unknown verdict")
    if doc.get("verdict") == VERDICT_PASS and doc.get("reasons"):
        problems.append("a PASS carries reasons")
    if doc.get("verdict") != VERDICT_PASS and not doc.get("reasons"):
        problems.append("a non-PASS verdict needs at least one reason")
    return problems


def validate_frame_line(line):
    """Return problems with one line of frames.jsonl (a JSON string)."""
    try:
        doc = json.loads(line)
    except ValueError:
        return ["not JSON"]
    problems = [f"missing key {key}" for key in REPLAY_LINE_KEYS if key not in doc]
    if doc.get("schema") != REPLAY_LINE_SCHEMA:
        problems.append("unsupported schema")
    return problems
