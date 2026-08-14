#!/usr/bin/env bash

set -euo pipefail


# ==================================================
# Paths
# ==================================================

ROOT="$HOME/cgroup_research"

MEMTEST="$ROOT/mem_interference_test"
EXP="$ROOT/experiments/stage6_5/6_5b_real_workloads"
GAPBS="$ROOT/benchmark/gapbs"

VICTIM="$MEMTEST/perf_mem_probe"
HOG="$MEMTEST/mem_hog"

GRAPH="$EXP/graphs/kron_g23.sg"

RAW="$EXP/raw/protected_corun"
RESULT="$EXP/results/protected_corun.tsv"
SUMMARY="$EXP/results/protected_corun_summary.tsv"


# ==================================================
# Experiment configuration
# ==================================================

RUNS=5

VICTIM_SIZE_MIB=64
VICTIM_SECONDS=10

HOG_TOTAL_MIB=384

HOG_WARMUP_SECONDS=1
GAPBS_WARMUP_SECONDS=1

COOLDOWN_SECONDS=2


# GAPBS background must remain alive during the
# entire 10-second victim measurement.
#
# B2 measurements:
#
# BFS:
#   ~0.044 sec / trial
#   320 trials ~= 14 sec
#
# PR:
#   ~1.157 sec / trial
#   12 trials ~= 13.9 sec
#
# This leaves enough margin after the 1-second
# background warm-up.
# ==================================================

BFS_TRIALS=320
PR_TRIALS=12


mkdir -p "$RAW" "$EXP/results"


# ==================================================
# Sanity checks
# ==================================================

for file in \
    "$VICTIM" \
    "$HOG" \
    "$GAPBS/bfs" \
    "$GAPBS/pr"
do
    if [ ! -x "$file" ]; then
        echo "error: executable not found: $file" >&2
        exit 1
    fi
done


if [ ! -f "$GRAPH" ]; then
    echo "error: graph not found: $GRAPH" >&2
    exit 1
fi


# ==================================================
# CPU topology
#
# One logical CPU per physical core.
#
# Current machine:
#
#   0 2 4 6 8 10 12 14
#
# CPU 0:
#   OS / miscellaneous
#
# CPU 2,4,6,8,10,12:
#   background workload
#
# CPU 14:
#   protected victim
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


OS_CPU="${PHYSICAL_CPUS[0]}"

BG_CPUS=(
    "${PHYSICAL_CPUS[1]}"
    "${PHYSICAL_CPUS[2]}"
    "${PHYSICAL_CPUS[3]}"
    "${PHYSICAL_CPUS[4]}"
    "${PHYSICAL_CPUS[5]}"
    "${PHYSICAL_CPUS[6]}"
)

VICTIM_CPU="${PHYSICAL_CPUS[7]}"


BG_CPU_LIST="$(IFS=,; echo "${BG_CPUS[*]}")"


echo "physical representative CPUs:"
printf '  %s\n' "${PHYSICAL_CPUS[@]}"

echo
echo "OS CPU        : $OS_CPU"
echo "background CPU: $BG_CPU_LIST"
echo "victim CPU    : $VICTIM_CPU"
echo


# ==================================================
# Initialize result
# ==================================================

printf \
'run\tcondition\tvictim_size_MiB\tvictim_seconds\telapsed_sec\ttotal_reads\tns_per_read\n' \
> "$RESULT"


# ==================================================
# Process state
# ==================================================

VICTIM_PID=""
BG_PID=""

HOG_PIDS=()


# ==================================================
# Cleanup
# ==================================================

cleanup_current()
{
    if [ -n "$VICTIM_PID" ]; then

        kill -CONT "$VICTIM_PID" \
            2>/dev/null || true

        kill -TERM "$VICTIM_PID" \
            2>/dev/null || true

        wait "$VICTIM_PID" \
            2>/dev/null || true

        VICTIM_PID=""
    fi


    if [ -n "$BG_PID" ]; then

        kill -TERM "$BG_PID" \
            2>/dev/null || true

        wait "$BG_PID" \
            2>/dev/null || true

        BG_PID=""
    fi


    if [ "${#HOG_PIDS[@]}" -gt 0 ]; then

        for pid in "${HOG_PIDS[@]}"
        do
            kill -TERM "$pid" \
                2>/dev/null || true
        done


        for pid in "${HOG_PIDS[@]}"
        do
            wait "$pid" \
                2>/dev/null || true
        done


        HOG_PIDS=()
    fi
}


trap cleanup_current EXIT INT TERM


# ==================================================
# Wait until victim enters SIGSTOP
# ==================================================

wait_for_stopped()
{
    local pid="$1"

    local state=""

    for _ in $(seq 1 200)
    do

        if ! kill -0 "$pid" 2>/dev/null; then
            return 1
        fi


        state="$(
            ps -o stat= -p "$pid" \
                2>/dev/null |
            tr -d ' '
        )"


        if [[ "$state" == *T* ]]; then
            return 0
        fi


        sleep 0.05
    done


    return 1
}


# ==================================================
# Wait for mem_hog REDY marker
#
# mem_hog source currently prints REDY, not READY.
# ==================================================

wait_for_hog_ready()
{
    local pid="$1"
    local log="$2"


    for _ in $(seq 1 200)
    do

        if ! kill -0 "$pid" 2>/dev/null; then
            return 1
        fi


        if grep -q '^REDY ' "$log" \
            2>/dev/null
        then
            return 0
        fi


        sleep 0.05
    done


    return 1
}


# ==================================================
# Prepare victim
#
# Important:
#
# perf_mem_probe allocates its memory,
# creates its random linked list,
# performs its warm-up,
# prints READY,
# and then SIGSTOPs itself.
#
# Therefore background interference begins only
# AFTER victim initialization/warm-up is complete.
# ==================================================

start_victim()
{
    local out_log="$1"
    local err_log="$2"


    taskset -c "$VICTIM_CPU" \
        "$VICTIM" \
        "$VICTIM_SIZE_MIB" \
        "$VICTIM_SECONDS" \
        >"$out_log" \
        2>"$err_log" &


    VICTIM_PID="$!"


    if ! wait_for_stopped "$VICTIM_PID"; then

        echo \
            "error: victim did not enter SIGSTOP state" \
            >&2

        echo \
            "----- victim stdout -----" \
            >&2

        cat "$out_log" >&2 || true


        echo \
            "----- victim stderr -----" \
            >&2

        cat "$err_log" >&2 || true


        exit 1
    fi


    if ! grep -q '^READY ' "$err_log"; then

        echo \
            "error: victim stopped but READY marker was not found" \
            >&2

        cat "$err_log" >&2 || true

        exit 1
    fi
}


# ==================================================
# Start mem_hog background
# ==================================================

start_hogs()
{
    local count="$1"
    local run="$2"
    local condition="$3"

    HOG_PIDS=()


    for ((i = 0; i < count; i++))
    do

        local cpu="${BG_CPUS[$i]}"

        local log="$RAW/run${run}_${condition}_hog${i}_cpu${cpu}.log"


        taskset -c "$cpu" \
            "$HOG" \
            "$HOG_TOTAL_MIB" \
            >/dev/null \
            2>"$log" &


        local pid="$!"

        HOG_PIDS+=("$pid")


        if ! wait_for_hog_ready \
            "$pid" \
            "$log"
        then
            echo \
                "error: mem_hog did not become ready" \
                >&2

            cat "$log" >&2 || true

            exit 1
        fi

    done


    sleep "$HOG_WARMUP_SECONDS"
}


# ==================================================
# Start GAPBS background
# ==================================================

start_gapbs()
{
    local kernel="$1"
    local run="$2"
    local condition="$3"

    local out_log="$RAW/run${run}_${condition}_bg.out"
    local err_log="$RAW/run${run}_${condition}_bg.err"


    case "$kernel" in

        bfs)

            env \
                OMP_NUM_THREADS=6 \
                OMP_DYNAMIC=FALSE \
                OMP_PROC_BIND=close \
                OMP_PLACES=threads \
                taskset -c "$BG_CPU_LIST" \
                "$GAPBS/bfs" \
                -f "$GRAPH" \
                -r 0 \
                -n "$BFS_TRIALS" \
                >"$out_log" \
                2>"$err_log" &

            ;;


        pr)

            env \
                OMP_NUM_THREADS=6 \
                OMP_DYNAMIC=FALSE \
                OMP_PROC_BIND=close \
                OMP_PLACES=threads \
                taskset -c "$BG_CPU_LIST" \
                "$GAPBS/pr" \
                -f "$GRAPH" \
                -n "$PR_TRIALS" \
                -i 20 \
                >"$out_log" \
                2>"$err_log" &

            ;;


        *)

            echo \
                "error: unknown GAPBS kernel: $kernel" \
                >&2

            exit 1

            ;;

    esac


    BG_PID="$!"


    # g23 Read Time was about 0.31 sec.
    # One second gives enough time for graph loading
    # and allows the application kernel to enter its
    # steady execution phase.

    sleep "$GAPBS_WARMUP_SECONDS"


    if ! kill -0 "$BG_PID" 2>/dev/null; then

        echo \
            "error: GAPBS background ended before victim measurement started" \
            >&2


        echo \
            "----- background stdout -----" \
            >&2

        cat "$out_log" >&2 || true


        echo \
            "----- background stderr -----" \
            >&2

        cat "$err_log" >&2 || true


        exit 1
    fi
}


# ==================================================
# Start selected background condition
# ==================================================

start_background()
{
    local condition="$1"
    local run="$2"


    case "$condition" in

        none)
            ;;


        hog1)
            start_hogs \
                1 \
                "$run" \
                "$condition"
            ;;


        hog4)
            start_hogs \
                4 \
                "$run" \
                "$condition"
            ;;


        bfs)
            start_gapbs \
                bfs \
                "$run" \
                "$condition"
            ;;


        pr)
            start_gapbs \
                pr \
                "$run" \
                "$condition"
            ;;


        *)
            echo \
                "error: unknown condition: $condition" \
                >&2

            exit 1
            ;;

    esac
}


# ==================================================
# Confirm background stayed alive
# ==================================================

verify_background_alive()
{
    local condition="$1"


    case "$condition" in

        none)

            return 0

            ;;


        hog1|hog4)

            for pid in "${HOG_PIDS[@]}"
            do

                if ! kill -0 "$pid" \
                    2>/dev/null
                then
                    return 1
                fi

            done

            ;;


        bfs|pr)

            if ! kill -0 "$BG_PID" \
                2>/dev/null
            then
                return 1
            fi

            ;;

    esac


    return 0
}


# ==================================================
# Run one condition
# ==================================================

run_condition()
{
    local run="$1"
    local condition="$2"


    local victim_out="$RAW/run${run}_${condition}_victim.out"
    local victim_err="$RAW/run${run}_${condition}_victim.err"


    local elapsed
    local total_reads
    local ns_per_read


    echo "=================================================="
    echo "run=$run condition=$condition"
    echo "=================================================="


    cleanup_current


    # ----------------------------------------------
    # 1. Prepare protected workload first.
    #    It stops itself after warm-up.
    # ----------------------------------------------

    start_victim \
        "$victim_out" \
        "$victim_err"


    echo \
        "victim ready: pid=$VICTIM_PID cpu=$VICTIM_CPU"


    # ----------------------------------------------
    # 2. Start and stabilize background.
    # ----------------------------------------------

    start_background \
        "$condition" \
        "$run"


    if ! verify_background_alive "$condition"; then

        echo \
            "error: background is not alive before victim start" \
            >&2

        exit 1
    fi


    # ----------------------------------------------
    # 3. Begin protected measurement.
    # ----------------------------------------------

    kill -CONT "$VICTIM_PID"


    if ! wait "$VICTIM_PID"; then

        echo \
            "error: victim exited with failure" \
            >&2

        cat "$victim_out" >&2 || true
        cat "$victim_err" >&2 || true


        VICTIM_PID=""

        exit 1
    fi


    VICTIM_PID=""


    # ----------------------------------------------
    # 4. Background must still be alive when
    #    victim completes. Otherwise the run did
    #    not contain full-duration interference.
    # ----------------------------------------------

    if ! verify_background_alive "$condition"; then

        echo \
            "error: background ended before victim completed; run is invalid" \
            >&2

        exit 1
    fi


    # ----------------------------------------------
    # 5. Parse victim result.
    # ----------------------------------------------

    elapsed="$(
        awk -F',' '
            $1 == "elapsed_sec" {
                print $2
                exit
            }
        ' "$victim_out"
    )"


    total_reads="$(
        awk -F',' '
            $1 == "total_reads" {
                print $2
                exit
            }
        ' "$victim_out"
    )"


    ns_per_read="$(
        awk -F',' '
            $1 == "ns_per_read" {
                print $2
                exit
            }
        ' "$victim_out"
    )"


    if [ -z "$elapsed" ] ||
       [ -z "$total_reads" ] ||
       [ -z "$ns_per_read" ]
    then

        echo \
            "error: failed to parse victim result" \
            >&2

        cat "$victim_out" >&2 || true

        exit 1
    fi


    # ----------------------------------------------
    # 6. Save result.
    # ----------------------------------------------

    printf \
'%d\t%s\t%d\t%d\t%s\t%s\t%s\n' \
        "$run" \
        "$condition" \
        "$VICTIM_SIZE_MIB" \
        "$VICTIM_SECONDS" \
        "$elapsed" \
        "$total_reads" \
        "$ns_per_read" \
        >> "$RESULT"


    echo "elapsed_sec : $elapsed"
    echo "total_reads : $total_reads"
    echo "ns_per_read : $ns_per_read"

    echo


    # ----------------------------------------------
    # 7. Remove background and cool down.
    # ----------------------------------------------

    cleanup_current

    sleep "$COOLDOWN_SECONDS"
}


# ==================================================
# Balanced run order
#
# Each condition appears once in each ordinal
# position across the five runs.
# ==================================================

ORDERS=(
    "none hog1 hog4 bfs pr"
    "hog1 hog4 bfs pr none"
    "hog4 bfs pr none hog1"
    "bfs pr none hog1 hog4"
    "pr none hog1 hog4 bfs"
)


if [ "$RUNS" -ne 5 ]; then

    echo \
        "error: balanced order table is defined for RUNS=5" \
        >&2

    exit 1
fi


# ==================================================
# Execute all runs
# ==================================================

for run in $(seq 1 "$RUNS")
do

    read -r -a CONDITIONS \
        <<< "${ORDERS[$((run - 1))]}"


    for condition in "${CONDITIONS[@]}"
    do

        run_condition \
            "$run" \
            "$condition"

    done

done


# ==================================================
# Summary
#
# RESULT columns:
#
# 1 run
# 2 condition
# 3 victim_size_MiB
# 4 victim_seconds
# 5 elapsed_sec
# 6 total_reads
# 7 ns_per_read
#
#
# slowdown_x:
#
#   condition_mean_ns
#   -----------------
#    baseline_mean_ns
#
#
# degradation_pct:
#
#   (slowdown_x - 1) * 100
#
#
# SD is sample standard deviation.
# ==================================================

awk -F '\t' '
    NR == 1 {
        next
    }


    {
        k = $2

        n[k]++

        sum[k] += $7
        sq[k] += $7 * $7
    }


    END {

        if (n["none"] > 0)
            baseline = sum["none"] / n["none"]
        else
            baseline = 0.0


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

            sd = 0.0


            if (n[k] > 1) {

                variance =
                    (sq[k] - n[k] * mean * mean) /
                    (n[k] - 1)


                if (variance < 0)
                    variance = 0


                sd = sqrt(variance)
            }


            if (baseline > 0) {

                slowdown =
                    mean / baseline


                degradation =
                    (slowdown - 1.0) * 100.0

            } else {

                slowdown = 0.0
                degradation = 0.0
            }


            printf \
                "%s\t%d\t%.3f\t%.3f\t%.3f\t%.1f\n", \
                k, \
                n[k], \
                mean, \
                sd, \
                slowdown, \
                degradation
        }
    }
' "$RESULT" \
> "$SUMMARY"


# ==================================================
# Final cleanup
# ==================================================

cleanup_current

trap - EXIT INT TERM


# ==================================================
# Print results
# ==================================================

echo "=================================================="
echo "protected co-run experiment complete"
echo "=================================================="

echo
echo "================ per-run results ================="

if command -v column >/dev/null 2>&1; then

    column \
        -t \
        -s $'\t' \
        "$RESULT"

else

    cat "$RESULT"

fi


echo
echo "================ summary ========================="

if command -v column >/dev/null 2>&1; then

    column \
        -t \
        -s $'\t' \
        "$SUMMARY"

else

    cat "$SUMMARY"

fi
