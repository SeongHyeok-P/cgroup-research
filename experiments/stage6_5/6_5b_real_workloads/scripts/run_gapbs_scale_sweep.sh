#!/usr/bin/env bash

set -euo pipefail

ROOT="$HOME/cgroup_research"
GAPBS="$ROOT/benchmark/gapbs"
EXP="$ROOT/experiments/stage6_5/6_5b_real_workloads"

RAW="$EXP/raw/scale_sweep"
RESULT="$EXP/results/gapbs_scale_sweep.tsv"

SCALES=(20 21 22 23)

# Scale 결정용이므로 3 trials만 수행
TRIALS=3

# PageRank iteration cap
PR_ITERS=20

mkdir -p "$RAW" "$EXP/results"


# --------------------------------------------------
# Pick one logical CPU from each physical core.
# Reserve first physical core for OS/perf.
# Use the next six physical cores for GAPBS.
# --------------------------------------------------

mapfile -t PHYSICAL_CPUS < <(
    lscpu -p=CPU,CORE,SOCKET |
    awk -F, '
        !/^#/ {
            key=$2 ":" $3
            if (!seen[key]++)
                print $1
        }
    '
)

if [ "${#PHYSICAL_CPUS[@]}" -lt 7 ]; then
    echo "error: need at least 7 physical cores" >&2
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

export OMP_NUM_THREADS=6
export OMP_DYNAMIC=FALSE
export OMP_PROC_BIND=TRUE


echo "GAPBS CPUs : $CPU_LIST"
echo "OMP threads: $OMP_NUM_THREADS"

printf \
"kernel\tscale\tnodes\tedges\tgenerate_sec\tbuild_sec\tavg_trial_sec\tmax_rss_kb\twall_sec\n" \
> "$RESULT"


run_one()
{
    kernel="$1"
    scale="$2"

    stdout="$RAW/${kernel}_g${scale}.out"
    stderr="$RAW/${kernel}_g${scale}.time"

    echo
    echo "=============================================="
    echo "$kernel scale=$scale"
    echo "=============================================="

    if [ "$kernel" = "bfs" ]; then

        /usr/bin/time -v \
            taskset -c "$CPU_LIST" \
            "$GAPBS/bfs" \
            -g "$scale" \
            -n "$TRIALS" \
            >"$stdout" \
            2>"$stderr"

    elif [ "$kernel" = "pr" ]; then

        /usr/bin/time -v \
            taskset -c "$CPU_LIST" \
            "$GAPBS/pr" \
            -g "$scale" \
            -n "$TRIALS" \
            -i "$PR_ITERS" \
            >"$stdout" \
            2>"$stderr"

    else
        echo "unknown kernel: $kernel" >&2
        exit 1
    fi


    nodes="$(
        awk '
            /Graph has/ {
                print $3
                exit
            }
        ' "$stdout"
    )"

    edges="$(
        awk '
            /Graph has/ {
                print $6
                exit
            }
        ' "$stdout"
    )"

    generate="$(
        awk '
            /^Generate Time:/ {
                print $3
                exit
            }
        ' "$stdout"
    )"

    build="$(
        awk '
            /^Build Time:/ {
                print $3
                exit
            }
        ' "$stdout"
    )"

    avg="$(
        awk '
            /^Average Time:/ {
                print $3
                exit
            }
        ' "$stdout"
    )"

    rss="$(
        awk -F: '
            /Maximum resident set size/ {
                gsub(/^[ \t]+/, "", $2)
                print $2
                exit
            }
        ' "$stderr"
    )"

    wall="$(
        awk -F: '
            /Elapsed \(wall clock\) time/ {
                value=$2

                # time -v may print m:ss.xx
                n=split(value, a, ":")

                if (n == 2) {
                    gsub(/^[ \t]+/, "", a[1])
                    print (a[1] * 60) + a[2]
                } else {
                    gsub(/^[ \t]+/, "", value)
                    print value
                }

                exit
            }
        ' "$stderr"
    )"


    printf \
"%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$kernel" \
        "$scale" \
        "$nodes" \
        "$edges" \
        "$generate" \
        "$build" \
        "$avg" \
        "$rss" \
        "$wall" \
        >> "$RESULT"


    echo "--- GAPBS ---"
    grep -E \
        'Generate Time|Build Time|Graph has|Average Time' \
        "$stdout"

    echo "--- resource ---"
    grep -E \
        'Maximum resident set size|Elapsed.*wall clock' \
        "$stderr"
}


for scale in "${SCALES[@]}"
do
    run_one bfs "$scale"
done

for scale in "${SCALES[@]}"
do
    run_one pr "$scale"
done


echo
echo "=============================================="
echo "scale sweep complete"
echo "=============================================="

column -t -s $'\t' "$RESULT" || cat "$RESULT"
