#!/usr/bin/env bash

set -euo pipefail

ROOT="$HOME/cgroup_research/experiments/stage6_5/6_5d_selective_control"
BIN="$ROOT/bin/oracle_selective_control"

RAW_DIR="$ROOT/raw/repeat"
RESULT_DIR="$ROOT/results"
PAIR_TSV="$RESULT_DIR/selective_control_pairs.tsv"

readonly POOL_MIB=2048
readonly WORKSET_MIB=64
readonly CLASS_COUNT=16
readonly MEASURE_SECONDS=10

readonly VICTIM_CPU=14
readonly HIGH_CPU=12
readonly LOW_CPU=10

readonly THROTTLE_QUOTA_US=50000
readonly THROTTLE_PERIOD_US=100000

readonly RUN_TIMEOUT_SECONDS=240

SEEDS=(
    20260814
    20260815
    20260816
    20260817
    20260818
)

ORDERS=(
    "none,uncontrolled,selective,all"
    "all,selective,uncontrolled,none"
)

mkdir -p "$RAW_DIR" "$RESULT_DIR"


log_is_valid()
{
    local log="$1"
    local order="$2"

    [[ -f "$log" ]] || return 1

    grep -Fqx "[CONFIG] seconds_per_condition=${MEASURE_SECONDS}" "$log" ||
        return 1

    grep -Fqx "[CONFIG] order=${order}" "$log" ||
        return 1

    grep -q \
        '^\[OVERLAP-LINE\] victim_vs_high .*weighted_jaccard=1\.000000$' \
        "$log" ||
        return 1

    grep -q \
        '^\[OVERLAP-LINE\] victim_vs_low .*weighted_jaccard=0\.000000$' \
        "$log" ||
        return 1

    grep -q '^RESULT_CSV,none,' "$log" || return 1
    grep -q '^RESULT_CSV,uncontrolled,' "$log" || return 1
    grep -q '^RESULT_CSV,selective,' "$log" || return 1
    grep -q '^RESULT_CSV,all,' "$log" || return 1

    grep -Fqx \
        '[VERIFY] victim_pfn_moved=0 unreadable=0' \
        "$log" ||
        return 1

    grep -Fqx \
        '[VERIFY] high_pfn_moved=0 unreadable=0' \
        "$log" ||
        return 1

    grep -Fqx \
        '[VERIFY] low_pfn_moved=0 unreadable=0' \
        "$log" ||
        return 1

    return 0
}


append_result()
{
    local seed="$1"
    local order="$2"
    local log="$3"

    awk -F',' \
        -v seed="$seed" \
        -v order="$order" '
    $1 == "RESULT_CSV" {
        condition = $2

        latency[condition] = $5 + 0.0
        high_rate[condition] = $8 + 0.0
        low_rate[condition] = $11 + 0.0
        total_rate[condition] = $12 + 0.0
    }

    END {
        if (!("none" in latency) ||
            !("uncontrolled" in latency) ||
            !("selective" in latency) ||
            !("all" in latency)) {
            print "ERROR: incomplete RESULT_CSV: " FILENAME > "/dev/stderr"
            exit 1
        }

        excess = latency["uncontrolled"] - latency["none"]

        # awk 줄바꿈 에러 병합 완료 (수식들)
        if (excess > 0.0) {
            selective_recovery = (latency["uncontrolled"] - latency["selective"]) / excess * 100.0
            all_recovery = (latency["uncontrolled"] - latency["all"]) / excess * 100.0
        } else {
            selective_recovery = 0.0
            all_recovery = 0.0
        }

        if (total_rate["uncontrolled"] > 0.0) {
            selective_retention = total_rate["selective"] / total_rate["uncontrolled"] * 100.0
            all_retention = total_rate["all"] / total_rate["uncontrolled"] * 100.0
        } else {
            selective_retention = 0.0
            all_retention = 0.0
        }

        if (total_rate["all"] > 0.0) {
            selective_gain = (total_rate["selective"] / total_rate["all"] - 1.0) * 100.0
        } else {
            selective_gain = 0.0
        }

        selective_vs_all = (latency["selective"] / latency["all"] - 1.0) * 100.0

        # awk printf 에러 병합 완료 (한 줄로)
        printf "%s\t%s\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\n", seed, order, latency["none"], latency["uncontrolled"], latency["selective"], latency["all"], selective_recovery, all_recovery, selective_vs_all, total_rate["uncontrolled"], total_rate["selective"], total_rate["all"], selective_retention, all_retention, selective_gain, high_rate["uncontrolled"], low_rate["uncontrolled"], high_rate["selective"], low_rate["selective"]
    }
    ' "$log" >> "$PAIR_TSV"
}


printf \
'seed\torder\tnone_ns\tuncontrolled_ns\tselective_ns\tall_ns\tselective_recovery_pct\tall_recovery_pct\tselective_vs_all_latency_pct\tuncontrolled_bg\tselective_bg\tall_bg\tselective_bg_retention_pct\tall_bg_retention_pct\tselective_bg_gain_vs_all_pct\tuncontrolled_high\tuncontrolled_low\tselective_high\tselective_low\n' \
> "$PAIR_TSV"

sudo -v

total_runs=$((
    ${#SEEDS[@]} *
    ${#ORDERS[@]}
))

run_index=0

for seed in "${SEEDS[@]}"; do
    for order in "${ORDERS[@]}"; do

        run_index=$((run_index + 1))

        order_tag="${order//,/_}"
        log="$RAW_DIR/seed_${seed}_${order_tag}.log"

        echo
        echo "============================================================"
        echo "[RUN $run_index / $total_runs]"
        echo "seed             : $seed"
        echo "order            : $order"
        echo "measure seconds  : $MEASURE_SECONDS"
        echo "victim CPU       : $VICTIM_CPU"
        echo "high CPU         : $HIGH_CPU"
        echo "low CPU          : $LOW_CPU"
        echo "log              : $log"
        echo "============================================================"

        if log_is_valid "$log" "$order"; then
            echo "[REUSE] valid completed log"
            append_result "$seed" "$order" "$log"
            continue
        fi

        if [[ -f "$log" ]]; then
            backup="${log}.invalid.$(date +%Y%m%d_%H%M%S)"
            echo "[INFO] old/invalid log -> $backup"
            mv "$log" "$backup"
        fi

        echo "[START] new measurement"

        if ! sudo timeout \
            --signal=TERM \
            --kill-after=5s \
            "${RUN_TIMEOUT_SECONDS}s" \
            "$BIN" \
                --pool-mib "$POOL_MIB" \
                --workset-mib "$WORKSET_MIB" \
                --classes "$CLASS_COUNT" \
                --seconds "$MEASURE_SECONDS" \
                --victim-cpu "$VICTIM_CPU" \
                --high-cpu "$HIGH_CPU" \
                --low-cpu "$LOW_CPU" \
                --throttle-quota-us "$THROTTLE_QUOTA_US" \
                --throttle-period-us "$THROTTLE_PERIOD_US" \
                --seed "$seed" \
                --order "$order" \
            | tee "$log"
        then
            echo
            echo "[ERROR] experiment failed or timed out"
            echo "[ERROR] log: $log"
            exit 1
        fi

        if ! log_is_valid "$log" "$order"; then
            echo
            echo "[ERROR] validation failed"
            echo "[ERROR] log: $log"
            exit 1
        fi

        append_result "$seed" "$order" "$log"

        echo "[PASS] measurement + validation completed"
    done
done


echo
echo "============================================================"
echo "PAIR RESULTS"
echo "============================================================"

column -t -s $'\t' "$PAIR_TSV" || cat "$PAIR_TSV"


echo
echo "============================================================"
echo "SUMMARY"
echo "============================================================"

awk -F '\t' '
NR == 1 {
    next
}

{
    n++

    recovery = $7 + 0.0
    retention = $13 + 0.0
    gain = $15 + 0.0
    latency_cost = $9 + 0.0

    sum_recovery += recovery
    sumsq_recovery += recovery * recovery

    sum_retention += retention
    sum_gain += gain
    sum_latency_cost += latency_cost

    if (recovery > 0.0)
        positive_recovery++

    if (retention > ($14 + 0.0))
        selective_preserves_more++
}

END {
    if (n == 0) {
        print "no results"
        exit 1
    }

    mean_recovery = sum_recovery / n

    if (n > 1) {
        # awk 줄바꿈 에러 병합 완료 (수식)
        variance = (sumsq_recovery - n * mean_recovery * mean_recovery) / (n - 1)

        if (variance < 0.0)
            variance = 0.0

        sd_recovery = sqrt(variance)
    } else {
        sd_recovery = 0.0
    }

    printf "runs                                : %d\n", n
    printf "positive selective recovery         : %d / %d\n", positive_recovery, n
    printf "selective preserves more BG than all   : %d / %d\n", selective_preserves_more, n
    printf "mean selective recovery             : %.3f %%\n", mean_recovery
    printf "stddev selective recovery           : %.3f %%\n", sd_recovery
    printf "mean selective BG retention         : %.3f %%\n", sum_retention / n
    printf "mean selective BG gain vs all       : %.3f %%\n", sum_gain / n
    printf "mean selective latency cost vs all  : %.3f %%\n", sum_latency_cost / n
}
' "$PAIR_TSV"

echo
echo "[DONE]"
echo "result: $PAIR_TSV"
