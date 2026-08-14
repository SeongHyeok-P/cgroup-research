#!/usr/bin/env bash

set -euo pipefail


# ==================================================
# Paths
# ==================================================

ROOT="$HOME/cgroup_research"

EXP="$ROOT/experiments/stage6_5/6_5c_bank_overlap"

WORKER="$EXP/bin/oracle_bank_worker"

RUNTIME_MAP="/run/protected-daemon/dram-map.json"
SAVED_MAP="$EXP/config/dram-map.json"

RAW="$EXP/raw/oracle_overlap_pilot"

RESULT="$EXP/results/oracle_overlap_pilot.tsv"
SUMMARY="$EXP/results/oracle_overlap_pilot_summary.tsv"


# ==================================================
# Experiment configuration
# ==================================================

RUNS=3

PROTECTED_CLASS="0x00"
LOW_CLASS="0x7f"

CANDIDATE_MIB=4096
SELECTED_MIB=28

PROTECTED_SECONDS=10
AGGRESSOR_SECONDS=12

AGGRESSOR_WARMUP_SECONDS=1


mkdir -p "$RAW" "$EXP/results"


# ==================================================
# Sanity checks
# ==================================================

if [ ! -x "$WORKER" ]; then
    echo "error: worker not found: $WORKER" >&2
    exit 1
fi


if [ ! -f "$SAVED_MAP" ]; then
    echo "error: saved mapping not found: $SAVED_MAP" >&2
    exit 1
fi


if ! sudo test -f "$RUNTIME_MAP"; then
    echo "error: runtime mapping not found: $RUNTIME_MAP" >&2
    exit 1
fi


# The runtime mapping must be exactly the mapping
# saved for this Stage 6.5-C experiment.
if ! sudo cmp -s "$RUNTIME_MAP" "$SAVED_MAP"; then
    echo "error: runtime dram-map.json differs from saved 6.5-C mapping" >&2
    exit 1
fi


# ==================================================
# CPU topology
#
# One representative logical CPU per physical core.
#
# Expected:
#   0 2 4 6 8 10 12 14
#
# CPU 12 : aggressor
# CPU 14 : protected
#
# Both are distinct physical cores.
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


AGGRESSOR_CPU="${PHYSICAL_CPUS[6]}"
PROTECTED_CPU="${PHYSICAL_CPUS[7]}"


echo "protected CPU   : $PROTECTED_CPU"
echo "aggressor CPU   : $AGGRESSOR_CPU"
echo "protected class : $PROTECTED_CLASS"
echo "LOW class       : $LOW_CLASS"
echo


# ==================================================
# Result header
# ==================================================

printf \
'run\tcondition\tprotected_class\taggressor_class\tprotected_ns_per_read\tprotected_total_reads\tprotected_elapsed_sec\taggressor_ns_per_read\taggressor_total_reads\taggressor_elapsed_sec\taggressor_Mreads_per_sec\n' \
> "$RESULT"


# ==================================================
# Runtime PIDs
# ==================================================

PROTECTED_PID=""
AGGRESSOR_PID=""


# ==================================================
# Cleanup
# ==================================================

cleanup_current()
{
    if [ -n "$PROTECTED_PID" ]; then
        kill -CONT "$PROTECTED_PID" 2>/dev/null || true
        kill -TERM "$PROTECTED_PID" 2>/dev/null || true
        wait "$PROTECTED_PID" 2>/dev/null || true

        PROTECTED_PID=""
    fi


    if [ -n "$AGGRESSOR_PID" ]; then
        kill -CONT "$AGGRESSOR_PID" 2>/dev/null || true
        kill -TERM "$AGGRESSOR_PID" 2>/dev/null || true
        wait "$AGGRESSOR_PID" 2>/dev/null || true

        AGGRESSOR_PID=""
    fi
}


trap cleanup_current EXIT INT TERM


# ==================================================
# Wait until a worker reaches SIGSTOP
# ==================================================

wait_for_stopped()
{
    local pid="$1"
    local state=""


    for _ in $(seq 1 600)
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
# Start protected worker and stop at READY
# ==================================================

start_protected()
{
    local run="$1"
    local condition="$2"

    local out_log="$RAW/run${run}_${condition}_protected.out"
    local err_log="$RAW/run${run}_${condition}_protected.err"


    sudo taskset -c "$PROTECTED_CPU" \
        "$WORKER" \
        "$PROTECTED_CLASS" \
        "$CANDIDATE_MIB" \
        "$SELECTED_MIB" \
        "$PROTECTED_SECONDS" \
        --wait \
        >"$out_log" \
        2>"$err_log" &


    PROTECTED_PID="$!"


    if ! wait_for_stopped "$PROTECTED_PID"; then
        echo "error: protected worker failed to reach SIGSTOP" >&2
        cat "$err_log" >&2 || true
        exit 1
    fi


    if ! grep -q '^READY ' "$err_log"; then
        echo "error: protected READY marker missing" >&2
        cat "$err_log" >&2 || true
        exit 1
    fi
}


# ==================================================
# Start aggressor worker and stop at READY
# ==================================================

start_aggressor()
{
    local run="$1"
    local condition="$2"
    local target_class="$3"

    local out_log="$RAW/run${run}_${condition}_aggressor.out"
    local err_log="$RAW/run${run}_${condition}_aggressor.err"


    sudo taskset -c "$AGGRESSOR_CPU" \
        "$WORKER" \
        "$target_class" \
        "$CANDIDATE_MIB" \
        "$SELECTED_MIB" \
        "$AGGRESSOR_SECONDS" \
        --wait \
        >"$out_log" \
        2>"$err_log" &


    AGGRESSOR_PID="$!"


    if ! wait_for_stopped "$AGGRESSOR_PID"; then
        echo "error: aggressor failed to reach SIGSTOP" >&2
        cat "$err_log" >&2 || true
        exit 1
    fi


    if ! grep -q '^READY ' "$err_log"; then
        echo "error: aggressor READY marker missing" >&2
        cat "$err_log" >&2 || true
        exit 1
    fi
}


# ==================================================
# Read one CSV key from worker output
# ==================================================

csv_value()
{
    local file="$1"
    local key="$2"


    awk -F',' \
        -v wanted="$key" '
            $1 == wanted {
                print $2
                exit
            }
        ' \
        "$file"
}


# ==================================================
# Run one condition
# ==================================================

run_condition()
{
    local run="$1"
    local condition="$2"

    local aggressor_class="NA"

    local prot_out="$RAW/run${run}_${condition}_protected.out"
    local aggr_out="$RAW/run${run}_${condition}_aggressor.out"

    local prot_ns
    local prot_reads
    local prot_elapsed

    local aggr_ns="NA"
    local aggr_reads="NA"
    local aggr_elapsed="NA"
    local aggr_rate="NA"


    echo "=================================================="
    echo "run=$run condition=$condition"
    echo "=================================================="


    cleanup_current


    # ----------------------------------------------
    # Protected memory construction + warm-up
    # happen before the experiment.
    #
    # It stops at READY.
    # ----------------------------------------------

    start_protected \
        "$run" \
        "$condition"


    # ----------------------------------------------
    # Prepare aggressor if needed.
    # ----------------------------------------------

    case "$condition" in

        baseline)
            ;;


        high)

            aggressor_class="$PROTECTED_CLASS"

            start_aggressor \
                "$run" \
                "$condition" \
                "$aggressor_class"

            ;;


        low)

            aggressor_class="$LOW_CLASS"

            start_aggressor \
                "$run" \
                "$condition" \
                "$aggressor_class"

            ;;


        *)

            echo "error: unknown condition: $condition" >&2
            exit 1

            ;;

    esac


    # ----------------------------------------------
    # Start aggressor first.
    #
    # Give it one second to enter steady
    # pointer-chasing execution before protected
    # measurement begins.
    # ----------------------------------------------

    if [ "$condition" != "baseline" ]; then

        kill -CONT "$AGGRESSOR_PID"


        sleep "$AGGRESSOR_WARMUP_SECONDS"


        if ! kill -0 "$AGGRESSOR_PID" 2>/dev/null; then
            echo "error: aggressor ended during warm-up" >&2
            exit 1
        fi

    fi


    # ----------------------------------------------
    # Start protected 10-second measurement.
    # ----------------------------------------------

    kill -CONT "$PROTECTED_PID"


    if ! wait "$PROTECTED_PID"; then
        echo "error: protected worker exited with failure" >&2
        exit 1
    fi


    PROTECTED_PID=""


    # ----------------------------------------------
    # For HIGH/LOW, aggressor must still be alive
    # when protected measurement finishes.
    # ----------------------------------------------

    if [ "$condition" != "baseline" ]; then

        if ! kill -0 "$AGGRESSOR_PID" 2>/dev/null; then
            echo "error: aggressor ended before protected measurement completed" >&2
            exit 1
        fi


        if ! wait "$AGGRESSOR_PID"; then
            echo "error: aggressor worker exited with failure" >&2
            exit 1
        fi


        AGGRESSOR_PID=""

    fi


    # ----------------------------------------------
    # Parse protected result
    # ----------------------------------------------

    prot_ns="$(
        csv_value \
            "$prot_out" \
            ns_per_read
    )"


    prot_reads="$(
        csv_value \
            "$prot_out" \
            total_reads
    )"


    prot_elapsed="$(
        csv_value \
            "$prot_out" \
            elapsed_sec
    )"


    if [ -z "$prot_ns" ] ||
       [ -z "$prot_reads" ] ||
       [ -z "$prot_elapsed" ]
    then
        echo "error: failed to parse protected result" >&2
        cat "$prot_out" >&2 || true
        exit 1
    fi


    # ----------------------------------------------
    # Parse aggressor result
    # ----------------------------------------------

    if [ "$condition" != "baseline" ]; then

        aggr_ns="$(
            csv_value \
                "$aggr_out" \
                ns_per_read
        )"


        aggr_reads="$(
            csv_value \
                "$aggr_out" \
                total_reads
        )"


        aggr_elapsed="$(
            csv_value \
                "$aggr_out" \
                elapsed_sec
        )"


        if [ -z "$aggr_ns" ] ||
           [ -z "$aggr_reads" ] ||
           [ -z "$aggr_elapsed" ]
        then
            echo "error: failed to parse aggressor result" >&2
            cat "$aggr_out" >&2 || true
            exit 1
        fi


        aggr_rate="$(
            awk \
                -v reads="$aggr_reads" \
                -v sec="$aggr_elapsed" \
                'BEGIN {
                    printf "%.3f",
                        (reads / sec) / 1000000.0
                }'
        )"

    fi


    # ----------------------------------------------
    # Store result
    # ----------------------------------------------

    printf \
'%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$run" \
        "$condition" \
        "$PROTECTED_CLASS" \
        "$aggressor_class" \
        "$prot_ns" \
        "$prot_reads" \
        "$prot_elapsed" \
        "$aggr_ns" \
        "$aggr_reads" \
        "$aggr_elapsed" \
        "$aggr_rate" \
        >> "$RESULT"


    echo "protected ns/read : $prot_ns"


    if [ "$condition" != "baseline" ]; then
        echo "aggressor class   : $aggressor_class"
        echo "aggressor ns/read : $aggr_ns"
        echo "aggressor Mread/s : $aggr_rate"
    fi


    echo
}


# ==================================================
# Balanced pilot order
#
# Each condition occupies each ordinal position once.
# ==================================================

ORDERS=(
    "baseline high low"
    "high low baseline"
    "low baseline high"
)


if [ "$RUNS" -ne 3 ]; then
    echo "error: pilot order table requires RUNS=3" >&2
    exit 1
fi


# ==================================================
# Execute
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
# 1  run
# 2  condition
# 3  protected_class
# 4  aggressor_class
# 5  protected_ns_per_read
# 6  protected_total_reads
# 7  protected_elapsed_sec
# 8  aggressor_ns_per_read
# 9  aggressor_total_reads
# 10 aggressor_elapsed_sec
# 11 aggressor_Mreads_per_sec
# ==================================================

awk -F '\t' '
    NR == 1 {
        next
    }

    {
        k = $2

        n[k]++

        prot_sum[k] += $5
        prot_sq[k] += $5 * $5

        if ($11 != "NA") {
            aggr_n[k]++
            aggr_rate_sum[k] += $11
            aggr_rate_sq[k] += $11 * $11
        }
    }

    END {
        if (n["baseline"] == 0) {
            print "error: baseline missing" > "/dev/stderr"
            exit 1
        }

        baseline =
            prot_sum["baseline"] /
            n["baseline"]

        print "condition\truns\tmean_protected_ns\tsd_protected_ns\tslowdown_x\tdegradation_pct\tmean_aggressor_Mreads_s\tsd_aggressor_Mreads_s"

        order[1] = "baseline"
        order[2] = "high"
        order[3] = "low"

        for (i = 1; i <= 3; i++) {
            k = order[i]

            if (n[k] == 0)
                continue

            pm = prot_sum[k] / n[k]

            psd = 0.0

            if (n[k] > 1) {
                pv =
                    (prot_sq[k] -
                     n[k] * pm * pm) /
                    (n[k] - 1)

                if (pv < 0.0)
                    pv = 0.0

                psd = sqrt(pv)
            }

            slowdown = pm / baseline

            degradation =
                (slowdown - 1.0) *
                100.0


            if (aggr_n[k] > 0) {
                am =
                    aggr_rate_sum[k] /
                    aggr_n[k]

                asd = 0.0

                if (aggr_n[k] > 1) {
                    av =
                        (aggr_rate_sq[k] -
                         aggr_n[k] * am * am) /
                        (aggr_n[k] - 1)

                    if (av < 0.0)
                        av = 0.0

                    asd = sqrt(av)
                }

                printf \
                    "%s\t%d\t%.3f\t%.3f\t%.3f\t%.1f\t%.3f\t%.3f\n",
                    k,
                    n[k],
                    pm,
                    psd,
                    slowdown,
                    degradation,
                    am,
                    asd
            }
            else {
                printf \
                    "%s\t%d\t%.3f\t%.3f\t%.3f\t%.1f\tNA\tNA\n",
                    k,
                    n[k],
                    pm,
                    psd,
                    slowdown,
                    degradation
            }
        }
    }
' \
"$RESULT" \
> "$SUMMARY"


# ==================================================
# Finish
# ==================================================

cleanup_current

trap - EXIT INT TERM


echo "=================================================="
echo "oracle overlap pilot complete"
echo "=================================================="

echo


if command -v column >/dev/null 2>&1; then

    echo "================ per-run ========================="

    column \
        -t \
        -s $'\t' \
        "$RESULT"

    echo

    echo "================ summary ========================="

    column \
        -t \
        -s $'\t' \
        "$SUMMARY"

else

    cat "$RESULT"

    echo

    cat "$SUMMARY"

fi
