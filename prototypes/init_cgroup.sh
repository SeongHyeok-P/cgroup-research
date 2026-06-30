#!/bin/bash

set -e

BASE="/sys/fs/cgroup/cgroup_research"

echo "[INFO] initialize cgroup tree..."

# 1. Create base groups
sudo mkdir -p "$BASE/User"
sudo mkdir -p "$BASE/System"

# 2. Create User groups
sudo mkdir -p "$BASE/User/High"
sudo mkdir -p "$BASE/User/Important"
sudo mkdir -p "$BASE/User/Mid"
sudo mkdir -p "$BASE/User/Low"

# 3. Create System groups
sudo mkdir -p "$BASE/System/Protect"
sudo mkdir -p "$BASE/System/Critical"
sudo mkdir -p "$BASE/System/Service"
sudo mkdir -p "$BASE/System/Maintenance"

# 4. Enable available controllers for child cgroups
if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
    CTRLS=$(cat /sys/fs/cgroup/cgroup.controllers)

    for ctrl in $CTRLS; do
        echo "+$ctrl" | sudo tee /sys/fs/cgroup/cgroup.subtree_control >/dev/null 2>&1 || true
    done

    for ctrl in $CTRLS; do
        echo "+$ctrl" | sudo tee "$BASE/cgroup.subtree_control" >/dev/null 2>&1 || true
    done

    for ctrl in $CTRLS; do
        echo "+$ctrl" | sudo tee "$BASE/User/cgroup.subtree_control" >/dev/null 2>&1 || true
    done

    for ctrl in $CTRLS; do
        echo "+$ctrl" | sudo tee "$BASE/System/cgroup.subtree_control" >/dev/null 2>&1 || true
    done
fi

echo "[OK] cgroup tree initialized:"
find "$BASE" -maxdepth 3 -type d | sort
