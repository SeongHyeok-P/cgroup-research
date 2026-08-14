#!/usr/bin/env bash

set -euo pipefail

EXP="$HOME/cgroup_research/experiments/stage6_5/6_5b_real_workloads"

RESULT="$EXP/results/protected_corun.tsv"
SUMMARY="$EXP/results/protected_corun_summary.tsv"

if [ ! -f "$RESULT" ]; then
    echo "error: result file not found: $RESULT" >&2
    exit 1
fi

awk -F '\t' '
NR == 1 {
    next
}

{
    k = $2

    n[k]++
    sum[k] += $7
    sq[k] += ($7 * $7)
}

END {
    if (n["none"] == 0) {
        print "error: baseline condition none not found" > "/dev/stderr"
        exit 1
    }

    baseline = sum["none"] / n["none"]

    print "condition\truns\tmean_ns_per_read\tsd_ns_per_read\tslowdown_x\tdegradation_pct"

    order[1] = "none"
    order[2] = "hog1"
    order[3] = "hog4"
    order[4] = "bfs"
    order[5] = "pr"

    for (i = 1; i <= 5; i++) {
        k = order[i]

        if (n[k] == 0)
            continue

        mean = sum[k] / n[k]

        variance = 0.0
        sd = 0.0

        if (n[k] > 1) {
            variance = (sq[k] - n[k] * mean * mean) / (n[k] - 1)

            if (variance < 0.0)
                variance = 0.0

            sd = sqrt(variance)
        }

        slowdown = mean / baseline
        degradation = (slowdown - 1.0) * 100.0

        printf "%s\t%d\t%.3f\t%.3f\t%.3f\t%.1f\n",
            k,
            n[k],
            mean,
            sd,
            slowdown,
            degradation
    }
}
' "$RESULT" > "$SUMMARY"

echo "================ summary ========================="

if command -v column >/dev/null 2>&1; then
    column -t -s $'\t' "$SUMMARY"
else
    cat "$SUMMARY"
fi
