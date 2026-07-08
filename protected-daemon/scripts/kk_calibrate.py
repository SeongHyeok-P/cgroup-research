#!/usr/bin/env python3
"""
kk_calibrate.py

Protected-daemon calibration worker for Knock-Knock.

Goals
-----
1. Collect repeated timing data through `main --timing`.
2. Bootstrap a latency threshold independently for each run.
3. Keep only address pairs whose repeated measurements are label-consistent.
4. Recover candidate PA->bank parity masks with GF(2) nullspace subsampling.
5. Select an independent mask set on a tune split.
6. Validate the frozen mask set on an untouched run (different process/anchor).
7. Atomically export a machine-readable JSON result for protected_daemon.

This implementation intentionally uses only Python's standard library.
It does not depend on NumPy, pandas, matplotlib, galois, or scipy.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import random
import statistics
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence


@dataclass(frozen=True)
class PairRecord:
    run_id: int
    a1: int
    a2: int
    diff: int
    median_cycles: float
    label_conflict: bool
    repeats: int


@dataclass(frozen=True)
class ThresholdInfo:
    threshold_cycles: int
    low_edge_cycles: float
    high_edge_cycles: float
    gap_cycles: float
    total_pairs: int
    low_pairs: int
    high_pairs: int


@dataclass(frozen=True)
class Metrics:
    tp: int
    tn: int
    fp: int
    fn: int
    accuracy: float
    precision: float
    recall: float
    f1: float


class CalibrationError(RuntimeError):
    pass


def log(msg: str) -> None:
    print(f"[kk-calibrate] {msg}", flush=True)


def parse_hex_address(text: str) -> int:
    value = text.strip()
    if not value:
        raise ValueError("empty address")
    return int(value, 16)


def default_kk_main() -> Path:
    # .../cgroup_research/protected-daemon/scripts/kk_calibrate.py
    # -> .../cgroup_research/Knock-Knock/main
    script_path = Path(__file__).resolve()
    return script_path.parents[2] / "Knock-Knock" / "main"


def read_cpu_model() -> str:
    cpuinfo = Path("/proc/cpuinfo")
    if not cpuinfo.exists():
        return "unknown"
    for line in cpuinfo.read_text(errors="replace").splitlines():
        lower = line.lower()
        if lower.startswith("model name") or lower.startswith("hardware"):
            _, _, value = line.partition(":")
            return value.strip() or "unknown"
    return "unknown"


def read_mem_total_kib() -> int | None:
    meminfo = Path("/proc/meminfo")
    if not meminfo.exists():
        return None
    for line in meminfo.read_text(errors="replace").splitlines():
        if line.startswith("MemTotal:"):
            parts = line.split()
            if len(parts) >= 2:
                return int(parts[1])
    return None


def hardware_fingerprint() -> dict:
    return {
        "hostname": platform.node(),
        "machine": platform.machine(),
        "platform": platform.platform(),
        "kernel": platform.release(),
        "cpu_model": read_cpu_model(),
        "mem_total_kib": read_mem_total_kib(),
    }


def atomic_write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent)
    )
    tmp = Path(tmp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, sort_keys=True)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        if tmp.exists():
            tmp.unlink(missing_ok=True)


def validate_csv_header(path: Path) -> None:
    if not path.exists() or path.stat().st_size == 0:
        raise CalibrationError(f"timing CSV was not created: {path}")
    with path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        try:
            header = [x.strip() for x in next(reader)]
        except StopIteration as exc:
            raise CalibrationError(f"empty timing CSV: {path}") from exc

    required = {"a1", "a2", "elapsed_cycles", "v_a1", "v_a2"}
    if not required.issubset(header):
        raise CalibrationError(
            f"unexpected CSV header in {path}: {header}; required={sorted(required)}"
        )


def collect_timing_run(
    kk_main: Path,
    output_csv: Path,
    memory_percent: float,
    measurements: int,
    timing_rounds: int,
    extra_args: Sequence[str],
) -> None:
    if os.geteuid() != 0:
        raise CalibrationError(
            "timing collection requires root. Run kk_calibrate.py as root "
            "or invoke it from the root-owned protected_daemon."
        )
    if not kk_main.exists() or not os.access(kk_main, os.X_OK):
        raise CalibrationError(f"Knock-Knock executable not found/executable: {kk_main}")

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    output_csv.unlink(missing_ok=True)

    cmd = [
        str(kk_main),
        "--timing",
        "-p",
        str(memory_percent),
        "-n",
        str(measurements),
        "-r",
        str(timing_rounds),
        "-o",
        str(output_csv),
        *extra_args,
    ]
    log("exec: " + " ".join(cmd))
    completed = subprocess.run(cmd, check=False)
    if completed.returncode != 0:
        raise CalibrationError(
            f"Knock-Knock timing collection failed with exit code "
            f"{completed.returncode}"
        )
    validate_csv_header(output_csv)


def load_pair_latencies(path: Path) -> dict[tuple[int, int], list[int]]:
    pairs: dict[tuple[int, int], list[int]] = defaultdict(list)

    with path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        required = {"a1", "a2", "elapsed_cycles"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise CalibrationError(
                f"{path}: missing required columns {sorted(required)}"
            )

        for line_no, row in enumerate(reader, start=2):
            try:
                a1 = parse_hex_address(row["a1"])
                a2 = parse_hex_address(row["a2"])
                cycles = int(float(row["elapsed_cycles"].strip()))
            except (KeyError, TypeError, ValueError) as exc:
                raise CalibrationError(
                    f"{path}:{line_no}: malformed timing row: {row}"
                ) from exc

            pairs[(a1, a2)].append(cycles)

    if not pairs:
        raise CalibrationError(f"{path}: no address pairs loaded")

    return dict(pairs)


def bootstrap_threshold(
    pairs: dict[tuple[int, int], list[int]],
    *,
    min_high_pairs: int,
    min_high_fraction: float,
    min_low_fraction: float,
    min_gap_cycles: float,
) -> ThresholdInfo:
    medians = sorted(float(statistics.median(xs)) for xs in pairs.values())
    n = len(medians)

    required_high = max(min_high_pairs, math.ceil(n * min_high_fraction))
    required_low = max(1, math.ceil(n * min_low_fraction))

    if required_low + required_high > n:
        raise CalibrationError(
            f"threshold bootstrap constraints impossible: n={n}, "
            f"required_low={required_low}, required_high={required_high}"
        )

    best: tuple[float, int, float, float] | None = None

    for i in range(required_low - 1, n - required_high):
        lo = medians[i]
        hi = medians[i + 1]
        gap = hi - lo

        if gap <= 0:
            continue

        candidate = (gap, i, lo, hi)
        if best is None or candidate[0] > best[0]:
            best = candidate

    if best is None:
        raise CalibrationError("no threshold gap candidate found")

    gap, split_i, low_edge, high_edge = best

    if gap < min_gap_cycles:
        raise CalibrationError(
            f"largest admissible median gap too small: {gap:.1f} cycles "
            f"(minimum {min_gap_cycles:.1f})"
        )

    # Conflict iff cycles > threshold.
    threshold = int(math.floor((low_edge + high_edge) / 2.0))

    return ThresholdInfo(
        threshold_cycles=threshold,
        low_edge_cycles=low_edge,
        high_edge_cycles=high_edge,
        gap_cycles=gap,
        total_pairs=n,
        low_pairs=split_i + 1,
        high_pairs=n - split_i - 1,
    )


def consistency_filter(
    run_id: int,
    pairs: dict[tuple[int, int], list[int]],
    threshold: int,
) -> tuple[list[PairRecord], int]:
    kept: list[PairRecord] = []
    dropped = 0

    for (a1, a2), values in pairs.items():
        labels = [x > threshold for x in values]
        first = labels[0]

        if any(x != first for x in labels[1:]):
            dropped += 1
            continue

        kept.append(
            PairRecord(
                run_id=run_id,
                a1=a1,
                a2=a2,
                diff=a1 ^ a2,
                median_cycles=float(statistics.median(values)),
                label_conflict=first,
                repeats=len(values),
            )
        )

    return kept, dropped


def stratified_split(
    records: Sequence[PairRecord],
    test_fraction: float,
    rng: random.Random,
) -> tuple[list[PairRecord], list[PairRecord]]:
    by_label = {
        False: [r for r in records if not r.label_conflict],
        True: [r for r in records if r.label_conflict],
    }

    left: list[PairRecord] = []
    right: list[PairRecord] = []

    for group in by_label.values():
        group = list(group)
        rng.shuffle(group)

        if not group:
            continue

        n_test = int(round(len(group) * test_fraction))

        if len(group) >= 2:
            n_test = min(max(1, n_test), len(group) - 1)
        else:
            n_test = 0

        right.extend(group[:n_test])
        left.extend(group[n_test:])

    rng.shuffle(left)
    rng.shuffle(right)

    return left, right


def compress_diff(diff: int, bit_positions: Sequence[int]) -> int:
    out = 0

    for j, original_bit in enumerate(bit_positions):
        if (diff >> original_bit) & 1:
            out |= 1 << j

    return out


def expand_mask(mask: int, bit_positions: Sequence[int]) -> int:
    out = 0

    for j, original_bit in enumerate(bit_positions):
        if (mask >> j) & 1:
            out |= 1 << original_bit

    return out


def gf2_rank(rows: Iterable[int]) -> int:
    pivots: dict[int, int] = {}

    for value in rows:
        x = int(value)

        while x:
            p = x.bit_length() - 1

            if p in pivots:
                x ^= pivots[p]
            else:
                pivots[p] = x
                break

    return len(pivots)


def gf2_nullspace(rows: Sequence[int], ncols: int) -> list[int]:
    # Reduced row-echelon form over GF(2).
    matrix = [int(x) & ((1 << ncols) - 1) for x in rows if x]
    matrix = list(dict.fromkeys(matrix))

    pivot_cols: list[int] = []
    r = 0

    for col in range(ncols):
        pivot = None

        for i in range(r, len(matrix)):
            if (matrix[i] >> col) & 1:
                pivot = i
                break

        if pivot is None:
            continue

        matrix[r], matrix[pivot] = matrix[pivot], matrix[r]
        pivot_row = matrix[r]

        for i in range(len(matrix)):
            if i != r and ((matrix[i] >> col) & 1):
                matrix[i] ^= pivot_row

        pivot_cols.append(col)
        r += 1

        if r == len(matrix):
            break

    matrix = matrix[:r]
    pivot_set = set(pivot_cols)
    free_cols = [c for c in range(ncols) if c not in pivot_set]

    basis: list[int] = []

    for free in free_cols:
        vec = 1 << free

        for row, pivot_col in zip(matrix, pivot_cols):
            if (row >> free) & 1:
                vec |= 1 << pivot_col

        basis.append(vec)

    return basis


def parity(x: int) -> int:
    return x.bit_count() & 1


def predict_same_bank(diff: int, masks: Sequence[int]) -> bool:
    return bool(masks) and all(parity(diff & mask) == 0 for mask in masks)


def evaluate(records: Sequence[PairRecord], masks: Sequence[int]) -> Metrics:
    tp = tn = fp = fn = 0

    for rec in records:
        pred = predict_same_bank(rec.diff, masks)
        actual = rec.label_conflict

        if pred and actual:
            tp += 1
        elif pred and not actual:
            fp += 1
        elif not pred and actual:
            fn += 1
        else:
            tn += 1

    total = tp + tn + fp + fn
    accuracy = (tp + tn) / total if total else 0.0
    precision = tp / (tp + fp) if (tp + fp) else 0.0
    recall = tp / (tp + fn) if (tp + fn) else 0.0
    f1 = (
        2.0 * precision * recall / (precision + recall)
        if precision + recall
        else 0.0
    )

    return Metrics(tp, tn, fp, fn, accuracy, precision, recall, f1)


def mask_quality(mask: int, records: Sequence[PairRecord]) -> tuple[float, float]:
    positives = [r for r in records if r.label_conflict]
    negatives = [r for r in records if not r.label_conflict]

    pos_zero = (
        sum(parity(r.diff & mask) == 0 for r in positives) / len(positives)
        if positives
        else 0.0
    )

    neg_zero = (
        sum(parity(r.diff & mask) == 0 for r in negatives) / len(negatives)
        if negatives
        else 0.0
    )

    return pos_zero, neg_zero


def discover_candidates(
    fit_records: Sequence[PairRecord],
    *,
    rounds: int,
    subsample: int,
    seed: int,
    min_positive_zero_rate: float,
    max_candidates: int,
) -> tuple[list[int], dict[int, int], dict]:
    conflicts = [r for r in fit_records if r.label_conflict]

    if not conflicts:
        raise CalibrationError("no fit conflicts available for GF(2)")

    active_union = 0

    for rec in fit_records:
        active_union |= rec.diff

    bit_positions = [
        bit
        for bit in range(max(1, active_union.bit_length()))
        if (active_union >> bit) & 1
    ]

    if not bit_positions:
        raise CalibrationError("no varying physical-address bits found")

    compressed_conflicts = [
        compress_diff(r.diff, bit_positions)
        for r in conflicts
    ]

    rng = random.Random(seed)
    actual_subsample = min(subsample, len(compressed_conflicts))

    if actual_subsample <= 0:
        raise CalibrationError("invalid GF(2) subsample size")

    counts: Counter[int] = Counter()
    successful_rounds = 0
    nullity_hist: Counter[int] = Counter()

    for _ in range(rounds):
        if actual_subsample == len(compressed_conflicts):
            sample = list(compressed_conflicts)
        else:
            sample = rng.sample(compressed_conflicts, actual_subsample)

        basis = gf2_nullspace(sample, len(bit_positions))
        nullity_hist[len(basis)] += 1

        if not basis:
            continue

        successful_rounds += 1

        for compressed_mask in basis:
            mask = expand_mask(compressed_mask, bit_positions)

            if mask:
                counts[mask] += 1

    if not counts:
        raise CalibrationError(
            "GF(2) subsampling produced no candidate masks "
            f"({successful_rounds}/{rounds} successful rounds)"
        )

    scored: list[tuple[tuple, int]] = []

    for mask, freq in counts.items():
        pos_zero, neg_zero = mask_quality(mask, fit_records)

        if pos_zero < min_positive_zero_rate:
            continue

        score = (
            -pos_zero,
            -freq,
            mask.bit_count(),
            abs(neg_zero - 0.5),
            mask,
        )

        scored.append((score, mask))

    scored.sort(key=lambda x: x[0])
    candidates = [mask for _, mask in scored[:max_candidates]]

    if not candidates:
        raise CalibrationError(
            "candidate masks exist, but none passed fit positive-zero-rate filter"
        )

    diagnostics = {
        "active_bit_positions": bit_positions,
        "conflict_records": len(conflicts),
        "subsample_size": actual_subsample,
        "rounds": rounds,
        "successful_rounds": successful_rounds,
        "successful_round_rate": successful_rounds / rounds if rounds else 0.0,
        "nullity_histogram": {
            str(k): v for k, v in sorted(nullity_hist.items())
        },
        "raw_candidate_count": len(counts),
        "filtered_candidate_count": len(candidates),
    }

    return candidates, dict(counts), diagnostics


def select_masks_greedy(
    candidates: Sequence[int],
    candidate_counts: dict[int, int],
    tune_records: Sequence[PairRecord],
    *,
    max_masks: int,
    min_improvement: float,
) -> tuple[list[int], list[dict]]:
    selected: list[int] = []
    history: list[dict] = []
    current = evaluate(tune_records, selected)
    remaining = list(candidates)

    for _ in range(max_masks):
        best_mask: int | None = None
        best_metrics: Metrics | None = None
        best_key: tuple | None = None
        current_rank = gf2_rank(selected)

        for mask in remaining:
            if gf2_rank([*selected, mask]) == current_rank:
                continue

            metrics = evaluate(tune_records, [*selected, mask])

            key = (
                metrics.f1,
                metrics.precision,
                metrics.recall,
                candidate_counts.get(mask, 0),
                -mask.bit_count(),
            )

            if best_key is None or key > best_key:
                best_key = key
                best_mask = mask
                best_metrics = metrics

        if best_mask is None or best_metrics is None:
            break

        if best_metrics.f1 < current.f1 + min_improvement:
            break

        selected.append(best_mask)
        remaining.remove(best_mask)
        current = best_metrics

        history.append(
            {
                "mask": f"0x{best_mask:016x}",
                "weight": best_mask.bit_count(),
                "frequency": candidate_counts.get(best_mask, 0),
                "tune_metrics": asdict(best_metrics),
            }
        )

    return selected, history


def ensure_class_counts(
    name: str,
    records: Sequence[PairRecord],
    *,
    min_conflicts: int,
) -> None:
    conflicts = sum(r.label_conflict for r in records)
    non_conflicts = len(records) - conflicts

    if conflicts < min_conflicts:
        raise CalibrationError(
            f"{name}: only {conflicts} conflict pairs; "
            f"minimum required is {min_conflicts}"
        )

    if non_conflicts == 0:
        raise CalibrationError(f"{name}: no non-conflict pairs")


def mask_set_id(masks: Sequence[int]) -> str:
    raw = ",".join(f"{m:016x}" for m in masks).encode()
    return hashlib.sha256(raw).hexdigest()[:16]


def analyze_runs(
    csv_paths: Sequence[Path],
    args: argparse.Namespace,
) -> tuple[dict, list[int]]:
    all_runs: list[list[PairRecord]] = []
    run_summaries: list[dict] = []

    for run_id, path in enumerate(csv_paths):
        log(f"loading run {run_id}: {path}")

        pairs = load_pair_latencies(path)

        threshold = bootstrap_threshold(
            pairs,
            min_high_pairs=args.min_high_pairs,
            min_high_fraction=args.min_high_fraction,
            min_low_fraction=args.min_low_fraction,
            min_gap_cycles=args.min_gap_cycles,
        )

        kept, dropped = consistency_filter(
            run_id,
            pairs,
            threshold.threshold_cycles,
        )

        conflicts = sum(r.label_conflict for r in kept)

        run_summary = {
            "run_id": run_id,
            "csv": str(path),
            "threshold": asdict(threshold),
            "raw_unique_pairs": len(pairs),
            "consistent_pairs": len(kept),
            "dropped_inconsistent_pairs": dropped,
            "consistent_conflicts": conflicts,
            "consistent_non_conflicts": len(kept) - conflicts,
        }

        run_summaries.append(run_summary)
        all_runs.append(kept)

        log(
            f"run {run_id}: threshold={threshold.threshold_cycles}, "
            f"gap={threshold.gap_cycles:.1f}, "
            f"consistent={len(kept)}, conflicts={conflicts}, dropped={dropped}"
        )

        if conflicts < args.min_run_conflicts:
            raise CalibrationError(
                f"run {run_id}: only {conflicts} consistent conflicts; "
                f"minimum required is {args.min_run_conflicts}"
            )

    rng = random.Random(args.seed)

    if len(all_runs) >= 2:
        # Last process/run is untouched cross-run holdout.
        holdout = list(all_runs[-1])
        pre_holdout = [r for run in all_runs[:-1] for r in run]

        fit, tune = stratified_split(
            pre_holdout,
            args.tune_fraction,
            rng,
        )

        split_mode = "cross-run-holdout"

    else:
        # One-run fallback.
        train_tune, holdout = stratified_split(
            all_runs[0],
            args.holdout_fraction,
            rng,
        )

        fit, tune = stratified_split(
            train_tune,
            args.tune_fraction,
            rng,
        )

        split_mode = "single-run-pair-split"

    ensure_class_counts(
        "fit",
        fit,
        min_conflicts=args.min_fit_conflicts,
    )

    ensure_class_counts(
        "tune",
        tune,
        min_conflicts=args.min_tune_conflicts,
    )

    ensure_class_counts(
        "holdout",
        holdout,
        min_conflicts=args.min_holdout_conflicts,
    )

    candidates, candidate_counts, gf2_diag = discover_candidates(
        fit,
        rounds=args.gf2_rounds,
        subsample=args.gf2_subsample,
        seed=args.seed + 101,
        min_positive_zero_rate=args.min_positive_zero_rate,
        max_candidates=args.max_candidates,
    )

    selected_masks, selection_history = select_masks_greedy(
        candidates,
        candidate_counts,
        tune,
        max_masks=args.max_masks,
        min_improvement=args.min_mask_improvement,
    )

    if not selected_masks:
        raise CalibrationError("greedy tune selection produced no masks")

    fit_metrics = evaluate(fit, selected_masks)
    tune_metrics = evaluate(tune, selected_masks)
    holdout_metrics = evaluate(holdout, selected_masks)

    success = (
        gf2_diag["successful_round_rate"] >= args.min_successful_round_rate
        and holdout_metrics.precision >= args.min_holdout_precision
        and holdout_metrics.recall >= args.min_holdout_recall
        and len(selected_masks) > 0
    )

    result = {
        "status": "success" if success else "failed",
        "schema_version": 1,
        "generated_unix_ns": time.time_ns(),
        "hardware": hardware_fingerprint(),
        "split_mode": split_mode,
        "runs": run_summaries,
        "splits": {
            "fit_pairs": len(fit),
            "fit_conflicts": sum(r.label_conflict for r in fit),
            "tune_pairs": len(tune),
            "tune_conflicts": sum(r.label_conflict for r in tune),
            "holdout_pairs": len(holdout),
            "holdout_conflicts": sum(r.label_conflict for r in holdout),
        },
        "gf2": gf2_diag,
        "selection": {
            "candidate_masks": len(candidates),
            "selected_mask_count": len(selected_masks),
            "history": selection_history,
        },
        "bank_masks": [
            f"0x{m:016x}"
            for m in selected_masks
        ],
        "mask_set_id": mask_set_id(selected_masks),
        "metrics": {
            "fit": asdict(fit_metrics),
            "tune": asdict(tune_metrics),
            "holdout": asdict(holdout_metrics),
        },
        "acceptance": {
            "min_successful_round_rate": args.min_successful_round_rate,
            "min_holdout_precision": args.min_holdout_precision,
            "min_holdout_recall": args.min_holdout_recall,
            "passed": success,
        },
        "notes": [
            "Threshold is bootstrapped independently for each timing run.",
            "Consistency filtering is strict: all repeated measurements for a pair must agree.",
            "The final holdout run is not used for GF(2) discovery or mask selection.",
            "Row-mask recovery is intentionally omitted; Strategy B needs PA-to-bank class only.",
        ],
    }

    return result, selected_masks


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Knock-Knock calibration worker for protected_daemon"
    )

    p.add_argument(
        "--kk-main",
        type=Path,
        default=default_kk_main(),
        help="path to patched Knock-Knock main executable",
    )

    p.add_argument(
        "--input-csv",
        type=Path,
        action="append",
        default=[],
        help="analyze an existing timing CSV; repeat for multiple runs",
    )

    p.add_argument(
        "--runs",
        type=int,
        default=3,
        help="number of timing processes/runs to collect when --input-csv is absent",
    )

    p.add_argument(
        "--work-dir",
        type=Path,
        default=Path("/run/protected-daemon/kk-calibration"),
    )

    p.add_argument(
        "--output",
        type=Path,
        default=Path("/run/protected-daemon/dram-map.json"),
    )

    p.add_argument("--memory-percent", type=float, default=25.0)

    p.add_argument(
        "--measurements",
        type=int,
        default=500,
        help="value passed to patched Knock-Knock -n",
    )

    p.add_argument("--timing-rounds", type=int, default=50)

    p.add_argument(
        "--kk-extra",
        action="append",
        default=[],
        help="extra single argument passed to Knock-Knock; repeat as needed",
    )

    p.add_argument("--min-high-pairs", type=int, default=20)
    p.add_argument("--min-high-fraction", type=float, default=0.001)
    p.add_argument("--min-low-fraction", type=float, default=0.50)
    p.add_argument("--min-gap-cycles", type=float, default=8.0)
    p.add_argument("--min-run-conflicts", type=int, default=50)

    p.add_argument("--seed", type=int, default=20260708)
    p.add_argument("--tune-fraction", type=float, default=0.25)
    p.add_argument("--holdout-fraction", type=float, default=0.20)
    p.add_argument("--min-fit-conflicts", type=int, default=50)
    p.add_argument("--min-tune-conflicts", type=int, default=20)
    p.add_argument("--min-holdout-conflicts", type=int, default=20)

    p.add_argument("--gf2-rounds", type=int, default=60)
    p.add_argument("--gf2-subsample", type=int, default=100)
    p.add_argument("--min-positive-zero-rate", type=float, default=0.95)
    p.add_argument("--max-candidates", type=int, default=128)
    p.add_argument("--max-masks", type=int, default=16)
    p.add_argument("--min-mask-improvement", type=float, default=0.001)

    p.add_argument("--min-successful-round-rate", type=float, default=0.80)
    p.add_argument("--min-holdout-precision", type=float, default=0.90)
    p.add_argument("--min-holdout-recall", type=float, default=0.80)

    p.add_argument(
        "--keep-csv",
        action="store_true",
        help="keep collected timing CSV files after completion",
    )

    return p


def validate_args(args: argparse.Namespace) -> None:
    if args.runs < 1:
        raise CalibrationError("--runs must be >= 1")

    if not (0 < args.memory_percent <= 100):
        raise CalibrationError("--memory-percent must be in (0, 100]")

    if args.measurements < 1 or args.timing_rounds < 1:
        raise CalibrationError("measurement counts must be positive")

    for name in (
        "min_high_fraction",
        "min_low_fraction",
        "tune_fraction",
        "holdout_fraction",
        "min_positive_zero_rate",
        "min_successful_round_rate",
        "min_holdout_precision",
        "min_holdout_recall",
    ):
        value = getattr(args, name)

        if not (0.0 <= value <= 1.0):
            raise CalibrationError(
                f"--{name.replace('_', '-')} must be in [0,1]"
            )


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
        validate_args(args)
        collected: list[Path] = []

        if args.input_csv:
            csv_paths = [
                p.resolve()
                for p in args.input_csv
            ]

            for path in csv_paths:
                validate_csv_header(path)

        else:
            args.work_dir.mkdir(
                parents=True,
                exist_ok=True,
            )

            csv_paths = []

            for run_id in range(args.runs):
                path = (
                    args.work_dir / f"timing_run_{run_id:02d}.csv"
                ).resolve()

                collect_timing_run(
                    kk_main=args.kk_main.resolve(),
                    output_csv=path,
                    memory_percent=args.memory_percent,
                    measurements=args.measurements,
                    timing_rounds=args.timing_rounds,
                    extra_args=args.kk_extra,
                )

                csv_paths.append(path)
                collected.append(path)

        result, masks = analyze_runs(
            csv_paths,
            args,
        )

        result["collector"] = {
            "kk_main": str(args.kk_main.resolve()),
            "memory_percent": args.memory_percent,
            "measurements": args.measurements,
            "timing_rounds": args.timing_rounds,
            "run_count": len(csv_paths),
            "input_csv_mode": bool(args.input_csv),
        }

        atomic_write_json(
            args.output.resolve(),
            result,
        )

        log(
            f"result written atomically: {args.output.resolve()}"
        )

        log(
            f"status={result['status']}, masks={len(masks)}, "
            f"holdout_precision={result['metrics']['holdout']['precision']:.4f}, "
            f"holdout_recall={result['metrics']['holdout']['recall']:.4f}"
        )

        if (
            collected
            and not args.keep_csv
            and result["status"] == "success"
        ):
            for path in collected:
                path.unlink(missing_ok=True)

        return 0 if result["status"] == "success" else 2

    except CalibrationError as exc:
        error_payload = {
            "status": "failed",
            "schema_version": 1,
            "generated_unix_ns": time.time_ns(),
            "hardware": hardware_fingerprint(),
            "error": str(exc),
        }

        try:
            atomic_write_json(
                args.output.resolve(),
                error_payload,
            )
        except Exception:
            pass

        print(
            f"[kk-calibrate] ERROR: {exc}",
            file=sys.stderr,
            flush=True,
        )

        return 2


if __name__ == "__main__":
    raise SystemExit(main())

