#!/usr/bin/env bash

set -euo pipefail


MEASURE_CPU=2
HOG_CPUS=(4 6 8 10 12 14)

HOG_TOTAL_MIB=384
HOG_WARMUP_SECONDS=10
PROBE_SECONDS=10


if [ "$#" -ne 2 ]; then
    echo "사용법:"
    echo "  $0 <size_MiB> <hog_count>"
    echo
    echo "예:"
    echo "  $0 8 0"
    echo "  $0 8 6"
    echo "  $0 64 0"
    echo "  $0 64 6"
    exit 1
fi


SIZE_MIB="$1"
COUNT="$2"


case "$COUNT" in
    0|1|2|4|6)
        ;;
    *)
        echo "hog_count는 0,1,2,4,6 중 하나여야 합니다."
        exit 1
        ;;
esac


HOG_PIDS=()
VICTIM_PID=""


cleanup()
{
    if [ -n "$VICTIM_PID" ]; then
        kill -CONT "$VICTIM_PID" 2>/dev/null || true
        kill "$VICTIM_PID" 2>/dev/null || true
    fi

    if [ "${#HOG_PIDS[@]}" -gt 0 ]; then
        kill "${HOG_PIDS[@]}" 2>/dev/null || true
        wait "${HOG_PIDS[@]}" 2>/dev/null || true
    fi
}


trap cleanup EXIT INT TERM


echo
echo "========================================"
echo "TMA metric test"
echo "size      : ${SIZE_MIB} MiB"
echo "hog count : ${COUNT}"
echo "victim CPU: ${MEASURE_CPU}"
echo "========================================"


# ----------------------------------------
# memory hog 시작
# ----------------------------------------

if [ "$COUNT" -gt 0 ]; then

    echo "[1] memory hog 시작"

    for ((i = 0; i < COUNT; i++)); do

        cpu="${HOG_CPUS[$i]}"

        taskset -c "$cpu" \
            ./mem_hog "$HOG_TOTAL_MIB" \
            >/dev/null 2>&1 &

        HOG_PIDS+=("$!")
    done


    echo "[2] hog 안정화"
    sleep "$HOG_WARMUP_SECONDS"
fi


# ----------------------------------------
# victim 준비
# ----------------------------------------

VICTIM_LOG="tma_victim_${SIZE_MIB}_hog${COUNT}.log"

echo "[3] victim 준비"

taskset -c "$MEASURE_CPU" \
    ./perf_mem_probe \
    "$SIZE_MIB" \
    "$PROBE_SECONDS" \
    >"$VICTIM_LOG" \
    2>&1 &

VICTIM_PID="$!"


READY=0

for _ in $(seq 1 600); do

    if ! kill -0 "$VICTIM_PID" 2>/dev/null; then
        echo "오류: victim 조기 종료"
        cat "$VICTIM_LOG"
        exit 1
    fi

    state="$(
        ps -o stat= -p "$VICTIM_PID" \
        2>/dev/null \
        | tr -d ' '
    )"

    if [[ "$state" == *T* ]]; then
        READY=1
        break
    fi

    sleep 0.1
done


if [ "$READY" -ne 1 ]; then
    echo "오류: victim이 SIGSTOP 상태에 들어가지 않음"
    exit 1
fi


echo "  victim PID $VICTIM_PID 준비 완료"


# ----------------------------------------
# TMA perf 측정
# ----------------------------------------

PERF_LOG="perf_tma_${SIZE_MIB}_hog${COUNT}.txt"

echo "[4] TMA perf attach"


sudo perf stat \
    -p "$VICTIM_PID" \
    -M tma_memory_bound,tma_l3_bound,tma_dram_bound \
    -- \
    sleep $((PROBE_SECONDS + 2)) \
    2>"$PERF_LOG" &

PERF_PID="$!"


sleep 1


echo "[5] victim 재개"

kill -CONT "$VICTIM_PID"


wait "$VICTIM_PID"

VICTIM_PID=""


wait "$PERF_PID"


echo
echo "===== victim result ====="
cat "$VICTIM_LOG"

echo
echo "===== TMA perf result ====="
cat "$PERF_LOG"

echo
echo "완료"
