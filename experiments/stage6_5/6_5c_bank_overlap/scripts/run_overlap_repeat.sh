#!/usr/bin/env bash

set -euo pipefail

ROOT="$HOME/cgroup_research/experiments/stage6_5/6_5c_bank_overlap"
BIN="$ROOT/bin/oracle_bank_overlap"

RAW_DIR="$ROOT/raw/repeat"
RESULT_DIR="$ROOT/results"
PAIR_TSV="$RESULT_DIR/bank_overlap_pairs.tsv"

readonly POOL_MIB=2048
readonly WORKSET_MIB=64
readonly CLASS_COUNT=16

#
# IMPORTANT:
# Bash의 특수 변수 SECONDS를 절대로 사용하지 않는다.
#
readonly MEASURE_SECONDS=10

readonly PROTECTED_CPU=14
readonly INTERFERENCE_CPU=12

#
# 정상적인 10초 × 3조건 실험은 이보다 훨씬 빨리 끝나야 한다.
# pool 초기화/classification 시간을 고려해 넉넉하게 180초.
#
readonly RUN_TIMEOUT_SECONDS=180

SEEDS=(
    20260814
    20260815
    20260816
    20260817
    20260818
)

ORDERS=(
    "none,high,low"
    "none,low,high"
)

mkdir -p "$RAW_DIR" "$RESULT_DIR"


# ------------------------------------------------------------
# 로그가 정상적으로 완료된 실험인지 확인
# ------------------------------------------------------------

log_is_valid()
{
    local log="$1"

    [[ -f "$log" ]] || return 1

    #
    # 과거 SECONDS 버그로 생성된
    # 46 / 190 / 765초 로그를 재사용하지 않는다.
    #
    grep -q \
        "^\[CONFIG\] seconds_per_condition=${MEASURE_SECONDS}$" \
        "$log" \
        || return 1

    #
    # 실제 cache-line 기준 중첩 조건 확인
    #
    grep -q \
        '^\[OVERLAP-LINE\] victim_vs_high .*weighted_jaccard=1\.000000$' \
        "$log" \
        || return 1

    grep -q \
        '^\[OVERLAP-LINE\] victim_vs_low .*weighted_jaccard=0\.000000$' \
        "$log" \
        || return 1

    #
    # 세 조건 모두 결과가 있어야 한다.
    #
    grep -q '^RESULT_CSV,none,' "$log" || return 1
    grep -q '^RESULT_CSV,high,' "$log" || return 1
    grep -q '^RESULT_CSV,low,'  "$log" || return 1

    #
    # PFN이 실험 중 이동하지 않았어야 한다.
    #
    grep -q \
        '^\[VERIFY\] victim_pfn_moved=0 unreadable=0$' \
        "$log" \
        || return 1

    grep -q \
        '^\[VERIFY\] high_pfn_moved=0 unreadable=0$' \
        "$log" \
        || return 1

    grep -q \
        '^\[VERIFY\] low_pfn_moved=0 unreadable=0$' \
        "$log" \
        || return 1

    return 0
}


# ------------------------------------------------------------
# 로그 한 개를 TSV 한 줄로 변환
# ------------------------------------------------------------

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
        load_rate[condition] = $9 + 0.0
    }

    END {
        if (!("none" in latency) ||
            !("high" in latency) ||
            !("low" in latency)) {

            print "ERROR: incomplete RESULT_CSV: " FILENAME > "/dev/stderr"
            exit 1
        }

        # 줄바꿈 에러 병합 완료
        high_vs_none = (latency["high"] / latency["none"] - 1.0) * 100.0
        low_vs_none = (latency["low"] / latency["none"] - 1.0) * 100.0
        high_vs_low = (latency["high"] / latency["low"] - 1.0) * 100.0

        if (load_rate["low"] > 0.0) {
            rate_ratio = load_rate["high"] / load_rate["low"]
        } else {
            rate_ratio = 0.0
        }

        # printf 줄바꿈 에러 병합 완료
        printf "%s\t%s\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.6f\n", seed, order, latency["none"], latency["high"], latency["low"], high_vs_none, low_vs_none, high_vs_low, load_rate["high"], load_rate["low"], rate_ratio
    }
    ' "$log" >> "$PAIR_TSV"
}


# ------------------------------------------------------------
# TSV 새로 생성
# ------------------------------------------------------------

printf \
'seed\torder\tnone_ns\thigh_ns\tlow_ns\thigh_vs_none_pct\tlow_vs_none_pct\thigh_vs_low_pct\thigh_Mload_per_sec\tlow_Mload_per_sec\thigh_low_rate_ratio\n' \
> "$PAIR_TSV"


# sudo 인증은 처음 한 번만
sudo -v


# ------------------------------------------------------------
# 반복 실험
# ------------------------------------------------------------

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
        echo "protected CPU    : $PROTECTED_CPU"
        echo "interference CPU : $INTERFERENCE_CPU"
        echo "log              : $log"
        echo "============================================================"

        #
        # 이미 정상 완료된 10초 실험이면 그대로 재사용
        #
        if log_is_valid "$log"; then

            echo "[REUSE] valid completed log"

            append_result \
                "$seed" \
                "$order" \
                "$log"

            continue
        fi


        #
        # 이전에 중단됐거나 SECONDS 버그로 생성된 로그 보존
        #
        if [[ -f "$log" ]]; then

            backup="${log}.invalid.$(date +%Y%m%d_%H%M%S)"

            echo "[INFO] old/invalid log -> $backup"

            mv "$log" "$backup"
        fi


        echo "[START] new measurement"


        #
        # 현재 v2 바이너리 CLI 이름은 그대로 사용한다.
        #
        # --victim-cpu     = 보호 대상 프로세스 CPU
        # --aggressor-cpu  = 간섭 프로세스 CPU
        #
        # 여기서는 기존 바이너리 인터페이스를 바꾸지 않는다.
        #
        if ! sudo timeout \
            --signal=TERM \
            --kill-after=5s \
            "${RUN_TIMEOUT_SECONDS}s" \
            "$BIN" \
                --pool-mib "$POOL_MIB" \
                --workset-mib "$WORKSET_MIB" \
                --classes "$CLASS_COUNT" \
                --seconds "$MEASURE_SECONDS" \
                --victim-cpu "$PROTECTED_CPU" \
                --aggressor-cpu "$INTERFERENCE_CPU" \
                --seed "$seed" \
                --order "$order" \
            | tee "$log"
        then
            echo
            echo "[ERROR] experiment failed or exceeded ${RUN_TIMEOUT_SECONDS}s"
            echo "[ERROR] log: $log"
            exit 1
        fi


        #
        # 실행 직후 반드시 검증
        #
        if ! log_is_valid "$log"; then

            echo
            echo "[ERROR] experiment finished but validation failed"
            echo "[ERROR] log: $log"

            exit 1
        fi


        append_result \
            "$seed" \
            "$order" \
            "$log"

        echo "[PASS] measurement + validation completed"

    done
done


# ------------------------------------------------------------
# 개별 결과
# ------------------------------------------------------------

echo
echo "============================================================"
echo "PAIR RESULTS"
echo "============================================================"

column -t -s $'\t' "$PAIR_TSV" || cat "$PAIR_TSV"


# ------------------------------------------------------------
# 전체 통계
# ------------------------------------------------------------

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

    high_low = $8 + 0.0

    sum_high_low += high_low
    sumsq_high_low += high_low * high_low

    sum_high_none += $6
    sum_low_none += $7

    sum_rate_ratio += $11

    if (high_low > 0.0)
        high_slower_count++
}

END {
    if (n == 0) {
        print "no results"
        exit 1
    }

    # 줄바꿈 에러 병합 완료
    mean_high_low = sum_high_low / n

    if (n > 1) {
        variance = (sumsq_high_low - n * mean_high_low * mean_high_low) / (n - 1)

        if (variance < 0.0)
            variance = 0.0

        sd_high_low = sqrt(variance)
    } else {
        sd_high_low = 0.0
    }

    # printf 줄바꿈 에러 병합 완료
    printf "runs                               : %d\n", n
    printf "high-overlap slower than low       : %d / %d\n", high_slower_count, n
    printf "mean high vs low latency           : %.3f %%\n", mean_high_low
    printf "stddev high vs low latency         : %.3f %%\n", sd_high_low
    printf "mean high vs no-interference       : %.3f %%\n", sum_high_none / n
    printf "mean low vs no-interference        : %.3f %%\n", sum_low_none / n
    printf "mean high/low interference-rate    : %.6f\n", sum_rate_ratio / n
}
' "$PAIR_TSV"


echo
echo "[DONE]"
echo "result: $PAIR_TSV"
