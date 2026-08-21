#!/bin/sh
echo "=== 종료 전 세션 요약 (클라이언트 끊길 때 기록됨) ==="
sudo -n sed -n '/=== session summary ===/,/=====/p' /home/petalinux/server.log | tail -14

PID=$(sudo -n sh -c "ps | grep -a ps_classifier_server | grep -av grep" | awk '{print $1}' | head -1)
echo "=== 서버 PID: ${PID:-(없음)} ==="
if [ -n "$PID" ]; then
    sudo -n kill -TERM "$PID"
    i=0
    while [ $i -lt 10 ]; do
        sudo -n sh -c "ps | grep -a ps_classifier_server | grep -av grep" >/dev/null 2>&1 || break
        sleep 1
        i=$((i+1))
    done
    if sudo -n sh -c "ps | grep -a ps_classifier_server | grep -av grep" >/dev/null 2>&1; then
        echo "TERM 으로 안 죽음 -> KILL"
        sudo -n kill -KILL "$PID"
        sleep 2
    fi
fi

echo "=== 확인 ==="
sudo -n sh -c "ps | grep -a ps_classifier_server | grep -av grep" && echo "!! 아직 프로세스 남음" || echo "프로세스: 없음 OK"
grep -a ':1388' /proc/net/tcp >/dev/null 2>&1 && echo "!! 포트 5000 아직 점유" || echo "포트 5000: 해제됨 OK"
