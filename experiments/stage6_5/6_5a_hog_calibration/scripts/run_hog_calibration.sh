#!/usr/bin/env bash

set -euo pipefail

ROOT="$HOME/cgroup_research"
EXP="$ROOT/experiments/stage6_5/6_5a_hog_calibration"

HOG="$ROOT/mem_interference_test/mem_hog"
PERF="$HOME/kernel_workspace/linux-hwe-7.0-7.0.0/tools/perf/perf"

RAW="$EXP/raw/hog"
RESULT="$EXP/results/hog_calibration.tsv"

HOG_TOTAL_MIB=384
MEASURE_SECONDS=10
RUNS=3

COUNTS=(1 2 4 6)

mkdir -p "$RAW" "$EXP/results"

if [ ! -x "$HOG" ]; then
    echo "error: mem_hog binary not found: $HOG" >&2
    exit 1
fi

if [ ! -x "$PERF" ]; then
    echo "error: perf binary not found: $PERF" >&2
    exit 1
fi


# --------------------------------------------------
# one logical CPU from each physical core
# reserve first physical core for OS/perf
# --------------------------------------------------

mapfile -t PHYSICAL_CPUS < <(
    lscpu -p=CPU,CORE,SOCKET |
    awk -F, '
        !/^#/ {
            key = $2 ":" $3
            if (!seen[key]++)
                print $1
        }
    '
)

if [ "${#PHYSICAL_CPUS[@]}" -lt 7 ]; then
    echo "error: need at least 7 physical cores" >&2
    exit 1
fi

HOG_CPUS=(
    "${PHYSICAL_CPUS[1]}"
    "${PHYSICAL_CPUS[2]}"
    "${PHYSICAL_CPUS[3]}"
    "${PHYSICAL_CPUS[4]}"
    "${PHYSICAL_CPUS[5]}"
    "${PHYSICAL_CPUS[6]}"
)

echo "physical-core representative CPUs:"
printf '  %s\n' "${PHYSICAL_CPUS[@]}"

echo
echo "hog CPUs:"
printf '  %s\n' "${HOG_CPUS[@]}"

printf \
"hog_count\trun\tpasses\tlogical_MiBps\tlogical_GBps\timc_read_MiB\timc_write_MiB\timc_total_MiB\tphysical_MiBps\tphysical_GBps\n" \
> "$RESULT"


cleanup_pids()
{
    local pid

    for pid in "$@"; do
        kill -CONT "$pid" 2>/dev/null || true
        kill -TERM "$pid" 2>/dev/null || true
    done

    for pid in "$@"; do
        wait "$pid" 2>/dev/null || true
    done
}


for count in "${COUNTS[@]}"
do
    for run in $(seq 1 "$RUNS")
    do
        echo
        echo "=================================================="
        echo "hog_count=$count run=$run"
        echo "=================================================="

        PIDS=()
        LOGS=()

        # ------------------------------------------
        # Start each hog and stop it as soon as READY
        # ------------------------------------------

        for ((i = 0; i < count; i++))
        do
            cpu="${HOG_CPUS[$i]}"
            log="$RAW/hog${count}_run${run}_proc${i}.log"

            : > "$log"

            taskset -c "$cpu" \
                "$HOG" "$HOG_TOTAL_MIB" \
                >"$log" 2>&1 &

            pid="$!"

            PIDS+=("$pid")
            LOGS+=("$log")

            ready=0

            for _ in $(seq 1 500)
            do
                if ! kill -0 "$pid" 2>/dev/null; then
                    echo "error: hog pid=$pid terminated early"
                    cat "$log"
                    cleanup_pids "${PIDS[@]}"
                    exit 1
                fi

                if grep -q "REDY pid=" "$log"; then
                    ready=1
                    break
                fi

                sleep 0.01
            done

            if [ "$ready" -ne 1 ]; then
                echo "error: hog pid=$pid did not become ready"
                cleanup_pids "${PIDS[@]}"
                exit 1
            fi

            kill -STOP "$pid"

            echo "prepared: pid=$pid cpu=$cpu"
        done


        # ------------------------------------------
        # Verify all are stopped
        # ------------------------------------------

        for pid in "${PIDS[@]}"
        do
            state="$(ps -o stat= -p "$pid" | tr -d ' ')"

            if [[ "$state" != *T* ]]; then
                echo "error: pid=$pid is not stopped: $state"
                cleanup_pids "${PIDS[@]}"
                exit 1
            fi
        done


        # ------------------------------------------
        # perf measurement
        #
        # perf starts counters,
        # resumes all hogs,
        # runs exactly MEASURE_SECONDS,
        # then stops all hogs.
        # ------------------------------------------

        perf_log="$RAW/hog${count}_run${run}_perf.txt"

        pid_list="${PIDS[*]}"

        echo "measurement start"

        sudo "$PERF" stat \
            --no-big-num \
            -x ';' \
            -a \
            -e uncore_imc_free_running_0/data_read/ \
            -e uncore_imc_free_running_0/data_write/ \
            -e uncore_imc_free_running_1/data_read/ \
            -e uncore_imc_free_running_1/data_write/ \
            -- \
            bash -c "
                kill -CONT $pid_list
                sleep $MEASURE_SECONDS
                kill -STOP $pid_list
            " \
            2>"$perf_log"


        # ------------------------------------------
        # Terminate stopped hogs and collect passes
        # ------------------------------------------

        for pid in "${PIDS[@]}"
        do
            kill -TERM "$pid" 2>/dev/null || true
        done

        for pid in "${PIDS[@]}"
        do
            kill -CONT "$pid" 2>/dev/null || true
        done

        for pid in "${PIDS[@]}"
        do
            wait "$pid" 2>/dev/null || true
        done


        # ------------------------------------------
        # Sum passes
        # ------------------------------------------

        total_passes=0

        for log in "${LOGS[@]}"
        do
            passes="$(
                awk '
                    /STOP pid=/ {
                        for (i = 1; i <= NF; i++) {
                            if ($i ~ /^passes=/) {
                                split($i, a, "=")
                                print a[2]
                            }
                        }
                    }
                ' "$log"
            )"

            if [ -z "$passes" ]; then
                echo "error: could not parse passes from $log"
                cat "$log"
                exit 1
            fi

            total_passes=$((total_passes + passes))
        done


        # ------------------------------------------
        # Parse scaled MiB values from perf
        # ------------------------------------------

        imc_read_mib="$(
            awk -F';' '
                $3 ~ /data_read/ {
                    sum += $1
                }
                END {
                    printf "%.6f", sum
                }
            ' "$perf_log"
        )"

        imc_write_mib="$(
            awk -F';' '
                $3 ~ /data_write/ {
                    sum += $1
                }
                END {
                    printf "%.6f", sum
                }
            ' "$perf_log"
        )"


        # ------------------------------------------
        # Calculate throughput
        # ------------------------------------------

        read \
            logical_mibps \
            logical_gbps \
            imc_total_mib \
            physical_mibps \
            physical_gbps \
        < <(
            awk \
                -v passes="$total_passes" \
                -v mib="$HOG_TOTAL_MIB" \
                -v sec="$MEASURE_SECONDS" \
                -v rd="$imc_read_mib" \
                -v wr="$imc_write_mib" \
                'BEGIN {
                    logical_mibps = passes * mib / sec
                    logical_gbps  = logical_mibps * 1048576 / 1000000000

                    total = rd + wr

                    physical_mibps = total / sec
                    physical_gbps  = physical_mibps * 1048576 / 1000000000

                    printf "%.3f %.3f %.3f %.3f %.3f\n",
                        logical_mibps,
                        logical_gbps,
                        total,
                        physical_mibps,
                        physical_gbps
                }'
        )


        printf \
"%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
            "$count" \
            "$run" \
            "$total_passes" \
            "$logical_mibps" \
            "$logical_gbps" \
            "$imc_read_mib" \
            "$imc_write_mib" \
            "$imc_total_mib" \
            "$physical_mibps" \
            "$physical_gbps" \
            >> "$RESULT"


        echo
        echo "passes          : $total_passes"
        echo "logical BW      : $logical_gbps GB/s"
        echo "IMC read        : $imc_read_mib MiB"
        echo "IMC write       : $imc_write_mib MiB"
        echo "physical BW     : $physical_gbps GB/s"

        sleep 2
    done
done


echo
echo "=================================================="
echo "hog calibration complete"
echo "=================================================="

column -t -s $'\t' "$RESULT" || cat "$RESULT"
