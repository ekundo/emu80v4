#!/bin/sh
# Взять код ESP платы ComboLite в эмулятор: Wi-Fi модем и канал настроек
# работают здесь те же, что на плате (VectorModem в src/Vector.cpp).
#
#   src/combolite/sync.sh ~/src/v06c-combolite/fw
#
# Файлы копируются, а не ссылаются: эмулятор должен собираться и без
# репозитория платы рядом. Править -- там, сюда -- только этим скриптом.
set -e
FW=${1:?путь к fw/ репозитория ComboLite}
HERE=$(cd "$(dirname "$0")" && pwd)
for f in vchan.c vchan.h modem.c modem.h bbslist.c bbslist.h \
         inflate.c inflate.h fetch.h fetch_curl.c vchan_sim.c board.h combo.h; do
    cp "$FW/$f" "$HERE/$f"
done
git -C "$FW" log -1 --format='%H' > "$HERE/FW_VERSION" 2>/dev/null || true
echo "взято из $FW"
