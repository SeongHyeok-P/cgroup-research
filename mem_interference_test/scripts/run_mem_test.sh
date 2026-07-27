#!/usr/bin/env bash

set -u


# --------------------------------------------------
# 기본 설정
# --------------------------------------------------

MEASURE_CPU=2

HOG_CPUS=(4 6 8 10 12 14)

HOG_TOTAL_MIB=384

READS_PER_ROUND=1000000

ROUNDS=10

WARMUP_SECONDS=10


# --------------------------------------------------
# 인자 확인
# --------------------------------------------------

if [ "$#" -ne 1 ]; then
    echo "사용법: $0 <메모리_방해_프로그램_개수>"
    echo "허용 값: 0, 1, 2, 4, 6"
    exit 1
fi


COUNT="$1"


case "$COUNT" in
    0|1|2|4|6)
        ;;
    *)
        echo "오류: 방해 프로그램 수는 0, 1, 2, 4, 6 중 하나여야 합니다."
        exit 1
        ;;
esac


# --------------------------------------------------
# 우리가 시작한 프로세스 PID 저장
# --------------------------------------------------

PIDS=()


# --------------------------------------------------
# 종료 함수
# --------------------------------------------------

cleanup()
{
    if [ "${#PIDS[@]}" -gt 0 ]; then

        echo
        echo "[정리] 메모리 방해 프로그램 종료"

        kill "${PIDS[@]}" 2>/dev/null || true

        wait "${PIDS[@]}" 2>/dev/null || true
    fi
}


# 스크립트가 중간에 종료되어도 정리
trap cleanup EXIT INT TERM


# --------------------------------------------------
# 시작 정보
# --------------------------------------------------

echo "========================================"
echo "메모리 방해 수 : $COUNT"
echo "측정 CPU       : $MEASURE_CPU"
echo "방해 CPU 후보  : ${HOG_CPUS[*]}"
echo "방해당 데이터  : ${HOG_TOTAL_MIB} MiB"
echo "안정화 시간    : ${WARMUP_SECONDS}초"
echo "========================================"


# --------------------------------------------------
# 메모리 방해 프로그램 실행
# --------------------------------------------------

if [ "$COUNT" -gt 0 ]; then

    echo
    echo "[1] 메모리 방해 프로그램 시작"

    for ((i = 0; i < COUNT; i++)); do

        cpu="${HOG_CPUS[$i]}"

        log_file="mem_hog_cpu${cpu}.log"

        taskset -c "$cpu" \
            ./mem_hog "$HOG_TOTAL_MIB" \
            > /dev/null \
            2> "$log_file" &

        pid=$!

        PIDS+=("$pid")

        echo "  CPU $cpu → PID $pid"
    done


    echo
    echo "[2] 모든 프로세스 초기화 및 부하 안정화 대기"
    sleep "$WARMUP_SECONDS"


    echo
    echo "[3] 프로세스 생존 확인"

    for pid in "${PIDS[@]}"; do

        if ! kill -0 "$pid" 2>/dev/null; then

            echo "오류: PID $pid 가 종료되었습니다."
            echo "로그를 확인하세요."

            exit 1
        fi
    done

    echo "  모든 방해 프로그램 실행 중"
fi


# --------------------------------------------------
# 측정
# --------------------------------------------------

echo
echo "[4] 측정 시작"


OUTPUT_FILE="mem_hog_${COUNT}_64.csv"


taskset -c "$MEASURE_CPU" \
    ./measure_mem \
    64 \
    "$READS_PER_ROUND" \
    "$ROUNDS" \
    | tee "$OUTPUT_FILE"


echo
echo "[5] 결과 저장 완료: $OUTPUT_FILE"


# cleanup은 trap EXIT에 의해 자동 실행
