#!/usr/bin/env bash
set -euo pipefail

EXP="${EXP:-$HOME/cgroup_research/experiments/stage6_5/6_5e_multicandidate_control}"
BIN="${BIN:-$EXP/bin/oracle_multicandidate_control}"
RAW_DIR="$EXP/raw"
RESULT_DIR="$EXP/results"
STAMP="$(date +%Y%m%d_%H%M%S)"

FORWARD_LOG="$RAW_DIR/e_forward_${STAMP}.log"
REVERSE_LOG="$RAW_DIR/e_reverse_${STAMP}.log"
RESULT_TSV="$RESULT_DIR/e_multicandidate_${STAMP}.tsv"
COMPARE_TSV="$RESULT_DIR/e_multicandidate_compare_${STAMP}.tsv"

mkdir -p "$RAW_DIR" "$RESULT_DIR"

if [[ ! -x "$BIN" ]]; then
    echo "[ERROR] executable not found: $BIN" >&2
    exit 1
fi

COMMON_ARGS=(
    --pool-mib 2048
    --workset-mib 64
    --classes 16
    --seconds 10
    --victim-cpu 14
    --o0-cpu 12
    --o25-cpu 10
    --o50-cpu 8
    --o75-cpu 6
    --o100-cpu 4
    --throttle-quota-us 50000
    --throttle-period-us 100000
    --seed 20260814
)

echo "[RUN] Experiment E forward"
sudo "$BIN" "${COMMON_ARGS[@]}" \
    --order none,uncontrolled,lowest,highest,top2,all \
    | tee "$FORWARD_LOG"

echo "[RUN] Experiment E reverse"
sudo "$BIN" "${COMMON_ARGS[@]}" \
    --order all,top2,highest,lowest,uncontrolled,none \
    | tee "$REVERSE_LOG"

{
    printf 'direction\tcondition\tvictim_elapsed_sec\tvictim_reads\tvictim_ns_per_read\to0_Mload_per_sec\to25_Mload_per_sec\to50_Mload_per_sec\to75_Mload_per_sec\to100_Mload_per_sec\ttotal_bg_Mload_per_sec\n'

    awk -F',' 'BEGIN {OFS="\t"}
        /^RESULT_CSV,/ {
            print "forward",$2,$3,$4,$5,$6,$7,$8,$9,$10,$11
        }' "$FORWARD_LOG"

    awk -F',' 'BEGIN {OFS="\t"}
        /^RESULT_CSV,/ {
            print "reverse",$2,$3,$4,$5,$6,$7,$8,$9,$10,$11
        }' "$REVERSE_LOG"
} > "$RESULT_TSV"

{
    printf 'direction\tmetric\tvalue\n'

    sed -n 's/^\[COMPARE\] \([^=]*\)=\(.*\)$/forward\t\1\t\2/p' \
        "$FORWARD_LOG"

    sed -n 's/^\[COMPARE\] \([^=]*\)=\(.*\)$/reverse\t\1\t\2/p' \
        "$REVERSE_LOG"
} > "$COMPARE_TSV"

echo
echo "[DONE] Experiment E"
echo "  forward raw : $FORWARD_LOG"
echo "  reverse raw : $REVERSE_LOG"
echo "  result TSV  : $RESULT_TSV"
echo "  compare TSV : $COMPARE_TSV"
