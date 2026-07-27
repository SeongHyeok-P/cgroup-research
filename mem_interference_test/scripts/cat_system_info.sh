echo "===== 1. kernel ====="
uname -r
uname -a

echo
echo "===== 2. filesystems ====="
grep resctrl /proc/filesystems || echo "resctrl not listed"

echo
echo "===== 3. mounted? ====="
mount | grep resctrl || echo "resctrl not mounted"

echo
echo "===== 4. findmnt ====="
findmnt -t resctrl || true

echo
echo "===== 5. directory ====="
ls -ld /sys/fs/resctrl
ls -la /sys/fs/resctrl

echo
echo "===== 6. cpuinfo related flags ====="
grep -m1 '^flags' /proc/cpuinfo | tr ' ' '\n' | grep -E 'rdt|cqm|mba|cat' || echo "no matching cpuinfo flags"

echo
echo "===== 7. kernel config ====="
grep -E 'CONFIG_(X86_CPU_RESCTRL|RESCTRL)' /boot/config-$(uname -r) || echo "no matching config line"
