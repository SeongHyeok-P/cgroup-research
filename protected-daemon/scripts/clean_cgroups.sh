#!/usr/bin/env bash
set -euo pipefail

BASE="/sys/fs/cgroup/cgroup_research"

if [[ $EUID -ne 0 ]]; then
    echo "[ERROR] Please run as root: sudo $0"
    exit 1
fi

if [[ ! -d "$BASE" ]]; then
    echo "[INFO] $BASE does not exist."
    exit 0
fi

echo "[INFO] Moving remaining processes to root cgroup..."

find "$BASE" -name cgroup.procs | while read procs; do
    while read -r pid; do
        [[ -z "$pid" ]] && continue
        echo "$pid" > /sys/fs/cgroup/cgroup.procs 2>/dev/null || true
    done < "$procs"
done

echo "[INFO] Removing child cgroups..."

# 깊은 디렉토리부터 삭제
find "$BASE" -depth -type d | while read dir; do
    if [[ "$dir" == "$BASE" ]]; then
        continue
    fi
    rmdir "$dir" 2>/dev/null || true
done

echo "[INFO] Done. Remaining:"
find "$BASE" -maxdepth 3 -type d 2>/dev/null || true
