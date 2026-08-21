#!/bin/sh
sudo -n sh -c 'ADAS_UART_PORT=/dev/ttyPS1 ADAS_TCP_NODELAY=1 nohup ps_classifier_server "*" 5000 /home/petalinux/arty_deploy_v2/model 6 1 1467099144 38 1160501223 35 1422046702 38 8.540366656652573e-06 > /home/petalinux/server.log 2>&1 &'
sleep 4
echo "=== 리스닝 ==="
grep -a ':1388' /proc/net/tcp >/dev/null && echo "5000 LISTEN OK" || echo "5000 리스닝 실패"
echo "=== 프로세스 ==="
ps | grep -a classifier_server | grep -av grep
