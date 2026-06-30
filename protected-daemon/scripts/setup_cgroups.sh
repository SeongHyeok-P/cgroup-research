#!/usr/bin/env bash
set -euo pipefail

BASE="/sys/fs/cgroup/cgroup_research"

PROTECTED="$BASE/protected"
BACKGROUND="$BASE/background"
THROTTLED="$BASE/throttled"

echo "[INFO] Setting up cgroup v2 hierarchy..."

# 1. root 권한 확인
if [[ $EUID -ne 0 ]]; then
    echo "[ERROR] Please run as root: sudo $0"
    exit 1
fi

# 2. cgroup v2 확인
# cgroup v2가 제대로 mount되어 있으면 이 파일이 존재함
if [[ ! -f /sys/fs/cgroup/cgroup.controllers ]]; then
    echo "[ERROR] cgroup v2 does not seem to be mounted at /sys/fs/cgroup."
    echo "        Check: mount | grep cgroup2"
    exit 1
fi

echo "[INFO] cgroup v2 detected."

# 3. base cgroup 생성
mkdir -p "$BASE"

# 4. root cgroup에서 cgroup_research 하위에 controller 넘기기
# 실패할 수 있는 controller는 무시
for ctrl in cpu memory io cpuset; do
    if grep -qw "$ctrl" /sys/fs/cgroup/cgroup.controllers; then
        echo "+$ctrl" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true
    fi
done

# 5. 새 연구용 cgroup 생성
mkdir -p "$PROTECTED"
mkdir -p "$BACKGROUND"
mkdir -p "$THROTTLED"

echo "[INFO] Created cgroups:"
echo "       $PROTECTED"
echo "       $BACKGROUND"
echo "       $THROTTLED"

# 6. cgroup_research 아래에서도 하위 cgroup 제어를 위해 controller 활성화
for ctrl in cpu memory io cpuset; do
    if grep -qw "$ctrl" "$BASE/cgroup.controllers"; then
        echo "+$ctrl" > "$BASE/cgroup.subtree_control" 2>/dev/null || true
    fi
done

echo "[INFO] Enabled subtree controllers under $BASE:"
cat "$BASE/cgroup.subtree_control" 2>/dev/null || true
echo

# 7. 기본 제한값 초기화
# protected: 제한 없음
echo "max 100000" > "$PROTECTED/cpu.max" 2>/dev/null || true
echo "max" > "$PROTECTED/memory.high" 2>/dev/null || true
echo "max" > "$PROTECTED/memory.max" 2>/dev/null || true

# background: 제한 없음
echo "max 100000" > "$BACKGROUND/cpu.max" 2>/dev/null || true
echo "max" > "$BACKGROUND/memory.high" 2>/dev/null || true
echo "max" > "$BACKGROUND/memory.max" 2>/dev/null || true

# throttled: 처음에는 제한 없음
echo "max 100000" > "$THROTTLED/cpu.max" 2>/dev/null || true
echo "max" > "$THROTTLED/memory.high" 2>/dev/null || true
echo "max" > "$THROTTLED/memory.max" 2>/dev/null || true

# 8. 결과 출력
echo "[INFO] Current layout:"
find "$BASE" -maxdepth 2 -type d

echo
echo "[INFO] Current cgroup settings:"
for cg in "$PROTECTED" "$BACKGROUND" "$THROTTLED"; do
    echo
    echo "== $cg =="

    echo -n "cpu.max: "
    cat "$cg/cpu.max" 2>/dev/null || echo "N/A"

    echo -n "memory.current: "
    cat "$cg/memory.current" 2>/dev/null || echo "N/A"

    echo -n "memory.high: "
    cat "$cg/memory.high" 2>/dev/null || echo "N/A"

    echo -n "memory.max: "
    cat "$cg/memory.max" 2>/dev/null || echo "N/A"
done

echo
echo "[INFO] cgroup setup complete."
