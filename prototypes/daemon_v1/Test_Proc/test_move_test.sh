#!/bin/bash

BASE=/home/shp1026/cgroup_research/Test_Proc
CG=/sys/fs/cgroup/cgroup_research

echo "=== before test ==="
echo "[High cpu.max]"
cat $CG/High/cpu.max
echo "[Middle cpu.max]"
cat $CG/Middle/cpu.max
echo "[Low cpu.max]"
cat $CG/Low/cpu.max

echo
echo "=== High test ==="
time $BASE/High/high_exe/t1_high

echo
echo "=== Middle test ==="
time $BASE/Middle/middle_exe/t1_middle

echo
echo "=== Low test ==="
time $BASE/Low/low_exe/t1_low

echo
echo "=== after test cpu.stat ==="
echo "[High]"
cat $CG/High/cpu.stat

echo
echo "[Middle]"
cat $CG/Middle/cpu.stat

echo
echo "[Low]"
cat $CG/Low/cpu.stat
