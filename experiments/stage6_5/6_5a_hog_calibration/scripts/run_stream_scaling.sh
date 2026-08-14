#!/usr/bin/env bash

set -euo pipefail

STREAM="$HOME/cgroup_research/benchmark/stream/stream_20m"
EXP="$HOME/cgroup_research/experiments/stage6_5/6_5a_hog_calibration"

RAW="$EXP/raw/stream"
RESULT="$EXP/results/stream_scaling.tsv"

mkdir -p "$RAW" "$EXP/results"

printf "threads\trun\ttriad_MBps\n" > "$RESULT"

export OMP_DYNAMIC=FALSE
export OMP_PLACES=cores
export OMP_PROC_BIND=close

for threads in 1 2 4 8 16
do
    for run in 1 2 3
    do
        outfile="$RAW/stream_${threads}t_run${run}.txt"

        echo "========================================"
        echo "STREAM: threads=$threads run=$run"
        echo "========================================"

        OMP_NUM_THREADS="$threads" \
            "$STREAM" | tee "$outfile"

        triad=$(
            awk '
            /^Triad:/ {
                print $2
            }
            ' "$outfile"
        )

        printf "%d\t%d\t%s\n" \
            "$threads" \
            "$run" \
            "$triad" >> "$RESULT"

        sleep 2
    done
done

echo
echo "===== STREAM scaling results ====="
cat "$RESULT"
