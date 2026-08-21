#!/bin/bash
cd /home/jetson/adas_project_sub/jetson
# ADAS_TCP_NODELAY: 요청을 헤더/bbox/이미지 3번에 나눠 보내므로 Nagle 을 끈다.
# 응답 쪽 40ms 지연은 TcpRoiClient 의 armQuickAck 이 처리한다 (그쪽 주석 참고).
export ADAS_TCP_NODELAY=1
exec ./build/jetson_roi_client /dev/video0 10.10.16.61 5000 \
     models/proposal/export/proposal_yolov8n_fp16.engine 8080
