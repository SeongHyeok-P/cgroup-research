#!/usr/bin/env bash

set -euo pipefail


# ==================================================
# Paths
# ==================================================

ROOT="$HOME/cgroup_research"

GAPBS="$ROOT/benchmark/gapbs"
EXP="$ROOT/experiments/stage6_5/6_5b_real_workloads"

GRAPH="$EXP/graphs/kron_g23.sg"

PERF="$HOME/kernel_workspace/linux-hwe-7.0-7.0.0/tools/perf/perf"

RAW="$EXP/raw/imc"
RESULT="$EXP/results/gapbs_imc.tsv"
SUMMARY="$EXP/results/gapbs_imc_summary.tsv"


# ==================================================
# Experiment configuration
# ==================================================

RUNS=3

BFS_TRIALS=230
PR_TRIALS=9

PERF_DELAY_MS=500

# Stage 6.5-A result
HOG6_PHYSICAL_GBPS=32.586


mkdir -p "$RAW" "$EXP/results"


# ==================================================
# Sanity checks
# ==================================================

if [ ! -f "$GRAPH" ]; then
    echo "error: graph not found: $GRAPH" >&2
    exit 1
fi

if [ ! -x "$PERF" ]; then
    echo "error: perf not found: $PERF" >&2
    exit 1
fi

if [ ! -x "$GAPBS/bfs" ]; then
    echo "error: bfs binary not found" >&2
    exit 1
fi

if [ ! -x "$GAPBS/pr" ]; then
    echo "error: pr binary not found" >&2
    exit 1
fi


# ==================================================
# CPU selection
#
# One logical CPU per physical core.
#
# Current machine is expected to produce:
#
#   0 2 4 6 8 10 12 14
#
# Core 0:
#   leave for OS / perf
#
# Next six physical cores:
#   GAPBS workload
#
# Final physical core:
#   reserved for later protected workload
# ==================================================

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


if [ "${#PHYSICAL_CPUS[@]}" -lt 8 ]; then
    echo "error: expected at least 8 physical cores" >&2
    exit 1
fi


APP_CPUS=(
    "${PHYSICAL_CPUS[1]}"
    "${PHYSICAL_CPUS[2]}"
    "${PHYSICAL_CPUS[3]}"
    "${PHYSICAL_CPUS[4]}"
    "${PHYSICAL_CPUS[5]}"
    "${PHYSICAL_CPUS[6]}"
)


CPU_LIST="$(IFS=,; echo "${APP_CPUS[*]}")"


echo "physical representative CPUs:"
printf '  %s\n' "${PHYSICAL_CPUS[@]}"

echo
echo "GAPBS CPUs : $CPU_LIST"
echo "OMP threads: 6"
echo


# ==================================================
# Initialize result file
# ==================================================

printf \
'kernel\trun\ttrials\tavg_trial_sec\tread_sec\timc_read_MiB\timc_write_MiB\timc_total_MiB\tcounter_window_sec\tphysical_GBps\n' \
> "$RESULT"


# ==================================================
# One experiment run
# ==================================================

run_one()
{
    local kernel="$1"
    local run="$2"

    local trials
    local app_log
    local perf_log

    local read_sec
    local avg_trial

    local parsed

    local imc_read_mib
    local imc_write_mib
    local imc_total_mib
    local counter_window_sec
    local physical_gbps

    local -a cmd


    app_log="$RAW/${kernel}_run${run}.out"
    perf_log="$RAW/${kernel}_run${run}.perf"


    # ----------------------------------------------
    # Build benchmark command
    # ----------------------------------------------

    case "$kernel" in

        bfs)
            trials="$BFS_TRIALS"

            cmd=(
                "$GAPBS/bfs"
                -f "$GRAPH"
                -r 0
                -n "$trials"
            )
            ;;


        pr)
            trials="$PR_TRIALS"

            cmd=(
                "$GAPBS/pr"
                -f "$GRAPH"
                -n "$trials"
                -i 20
            )
            ;;


        *)
            echo "error: unknown kernel: $kernel" >&2
            exit 1
            ;;

    esac


    echo "=================================================="
    echo "$kernel run=$run"
    echo "=================================================="


    # ----------------------------------------------
    # Run workload with IMC measurement
    #
    # --delay 500:
    # graph Read Time is about 0.31 sec.
    # Starting counters after 500 ms removes most
    # graph-loading traffic and measures mainly the
    # steady BFS/PageRank execution phase.
    # ----------------------------------------------

    sudo "$PERF" stat \
        --no-big-num \
        -x ';' \
        -a \
        --delay "$PERF_DELAY_MS" \
        -e uncore_imc_free_running_0/data_read/ \
        -e uncore_imc_free_running_0/data_write/ \
        -e uncore_imc_free_running_1/data_read/ \
        -e uncore_imc_free_running_1/data_write/ \
        -- \
        env \
            OMP_NUM_THREADS=6 \
            OMP_DYNAMIC=FALSE \
            OMP_PROC_BIND=close \
            OMP_PLACES=threads \
        taskset -c "$CPU_LIST" \
        "${cmd[@]}" \
        >"$app_log" \
        2>"$perf_log"


    # ----------------------------------------------
    # Parse GAPBS output
    # ----------------------------------------------

    read_sec="$(
        awk '
            /^Read Time:/ {
                print $3
                exit
            }
        ' "$app_log"
    )"


    avg_trial="$(
        awk '
            /^Average Time:/ {
                print $3
                exit
            }
        ' "$app_log"
    )"


    if [ -z "$read_sec" ] || [ -z "$avg_trial" ]; then
        echo "error: could not parse GAPBS output" >&2
        echo "----- app log -----" >&2
        cat "$app_log" >&2
        exit 1
    fi


    # ----------------------------------------------
    # Parse perf CSV
    #
    # Actual format observed:
    #
    # 255868.69;MiB;uncore_imc_free_running_0/data_read/;10325978443;100.00;;
    #
    # Field 1 = scaled MiB
    # Field 3 = event name
    # Field 4 = counter runtime in ns
    #
    # Each event may differ slightly in runtime.
    # Therefore:
    #
    #   event_rate = event_MiB / event_runtime
    #
    # and then all four event rates are summed.
    # ----------------------------------------------

    parsed="$(
        awk -F';' '
            $3 ~ /uncore_imc_free_running_[01]\/data_(read|write)\// &&
            $1 ~ /^[0-9]+([.][0-9]+)?$/ &&
            $4 ~ /^[0-9]+$/ {

                value = $1 + 0.0
                sec = ($4 + 0.0) / 1000000000.0

                if (sec <= 0)
                    next

                if ($3 ~ /data_read/)
                    read_mib += value
                else if ($3 ~ /data_write/)
                    write_mib += value

                total_mib += value
                total_mibps += value / sec

                runtime_sum += sec
                runtime_count++
            }

            END {
                if (runtime_count == 0)
                    exit 1

                mean_runtime = runtime_sum / runtime_count
                gbps = total_mibps * 1048576.0 / 1000000000.0

                printf "%.6f %.6f %.6f %.6f %.6f\n",
                    read_mib,
                    write_mib,
                    total_mib,
                    mean_runtime,
                    gbps
            }
        ' "$perf_log"
    )"


    if [ -z "$parsed" ]; then
        echo "error: could not parse perf counters" >&2
        echo "----- perf log -----" >&2
        cat "$perf_log" >&2
        exit 1
    fi


    read -r \
        imc_read_mib \
        imc_write_mib \
        imc_total_mib \
        counter_window_sec \
        physical_gbps \
        <<< "$parsed"


    # ----------------------------------------------
    # Save one run
    # ----------------------------------------------

    printf \
'%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$kernel" \
        "$run" \
        "$trials" \
        "$avg_trial" \
        "$read_sec" \
        "$imc_read_mib" \
        "$imc_write_mib" \
        "$imc_total_mib" \
        "$counter_window_sec" \
        "$physical_gbps" \
        >> "$RESULT"


    echo "Read Time       : $read_sec s"
    echo "Average Trial   : $avg_trial s"
    echo "Counter window  : $counter_window_sec s"
    echo "IMC read        : $imc_read_mib MiB"
    echo "IMC write       : $imc_write_mib MiB"
    echo "IMC total       : $imc_total_mib MiB"
    echo "Physical BW     : $physical_gbps GB/s"

    echo
}


# ==================================================
# BFS: 3 runs
# ==================================================

for run in $(seq 1 "$RUNS")
do
    run_one bfs "$run"
done


# ==================================================
# PageRank: 3 runs
# ==================================================

for run in $(seq 1 "$RUNS")
do
    run_one pr "$run"
done


# ==================================================
# Summary
#
# Columns in RESULT:
#
# 1  kernel
# 2  run
# 3  trials
# 4  avg_trial_sec
# 5  read_sec
# 6  imc_read_MiB
# 7  imc_write_MiB
# 8  imc_total_MiB
# 9  counter_window_sec
# 10 physical_GBps
#
# Sample standard deviation:
#
# sqrt(
#   (sum(x^2) - n * mean^2)
#   / (n - 1)
# )
# ==================================================

awk -F '\t' \
    -v hog6="$HOG6_PHYSICAL_GBPS" '
        NR == 1 {
            next
        }

        {
            k = $1

            n[k]++

            trial_sum[k] += $4
            trial_sq[k] += $4 * $4

            bw_sum[k] += $10
            bw_sq[k] += $10 * $10
        }

        END {
            print "kernel\truns\tmean_trial_sec\tsd_trial_sec\tmean_physical_GBps\tsd_physical_GBps\tpct_of_hog6_physical"

            order[1] = "bfs"
            order[2] = "pr"

            for (i = 1; i <= 2; i++) {
                k = order[i]

                if (n[k] == 0)
                    continue

                mt = trial_sum[k] / n[k]
                mb = bw_sum[k] / n[k]

                st = 0.0
                sb = 0.0

                if (n[k] > 1) {
                    vt = (trial_sq[k] - n[k] * mt * mt) / (n[k] - 1)
                    vb = (bw_sq[k] - n[k] * mb * mb) / (n[k] - 1)

                    if (vt < 0)
                        vt = 0

                    if (vb < 0)
                        vb = 0

                    st = sqrt(vt)
                    sb = sqrt(vb)
                }

                if (hog6 > 0)
                    pct = 100.0 * mb / hog6
                else
                    pct = 0.0

                printf "%s\t%d\t%.6f\t%.6f\t%.3f\t%.3f\t%.1f\n",
                    k,
                    n[k],
                    mt,
                    st,
                    mb,
                    sb,
                    pct
            }
        }
    ' \
    "$RESULT" \
    > "$SUMMARY"


# ==================================================
# Print results
# ==================================================

echo "=================================================="
echo "GAPBS IMC measurement complete"
echo "=================================================="

echo
echo "================ per-run results ================="

if command -v column >/dev/null 2>&1; then
    column -t -s $'\t' "$RESULT"
else
    cat "$RESULT"
fi


echo
echo "================ summary ========================="

if command -v column >/dev/null 2>&1; then
    column -t -s $'\t' "$SUMMARY"
else
    cat "$SUMMARY"
fi
