echo "===== 1. boot cmdline ====="
cat /proc/cmdline

echo
echo "===== 2. coloring boot option ====="
cat /proc/cmdline \
| grep -Eio 'page[_-]?colou?r(ing)?|cache[_-]?colou?r(ing)?' \
|| echo "부팅 인자에 page/cache coloring 흔적 없음"

echo
echo "===== 3. kernel config ====="
grep -Ei \
'CONFIG_.*(PAGE.*COL(OR|OUR)|CACHE.*COL(OR|OUR)|COL(OR|OUR).*PAGE|COL(OR|OUR).*CACHE)' \
/boot/config-$(uname -r) \
|| echo "커널 설정에 page/cache coloring 관련 CONFIG 없음"

echo
echo "===== 4. dmesg ====="
sudo dmesg \
| grep -Ei \
'page[ _-]?colou?r(ing)?|cache[ _-]?colou?r(ing)?' \
|| echo "커널 로그에 page/cache coloring 흔적 없음"

echo
echo "===== 5. kernel symbols ====="
grep -Ei \
'page.*colou?r|colou?r.*page|cache.*colou?r|colou?r.*cache' \
/proc/kallsyms \
| head -n 50

echo
echo "===== 6. installed headers ====="
grep -RniE \
'page[ _-]?colou?r(ing)?|cache[ _-]?colou?r(ing)?' \
/usr/src/linux-headers-$(uname -r) \
2>/dev/null \
| head -n 50

echo
echo "===== 7. local custom software ====="
grep -RniE \
'page[ _-]?colou?r(ing)?|cache[ _-]?colou?r(ing)?' \
/etc/systemd/system \
/usr/local \
/opt \
2>/dev/null \
| head -n 50
