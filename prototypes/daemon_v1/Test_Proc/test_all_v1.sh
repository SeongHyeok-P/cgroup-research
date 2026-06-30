#!/bin/bash

CG=/sys/fs/cgroup/cgroup_research

echo "=== High ==="
/home/shp1026/cgroup_research/Test_Proc/High/high_exe/t1_high &
PID=$!
echo $PID | sudo tee $CG/High/cgroup.procs > /dev/null
time wait $PID

echo "=== Middle ==="
/home/shp1026/cgroup_research/Test_Proc/Middle/middle_exe/t1_middle &
PID=$!
echo $PID | sudo tee $CG/Middle/cgroup.procs > /dev/null
time wait $PID

echo "=== Low ==="
/home/shp1026/cgroup_research/Test_Proc/Low/low_exe/t1_low &
PID=$!
echo $PID | sudo tee $CG/Low/cgroup.procs > /dev/null
time wait $PID
