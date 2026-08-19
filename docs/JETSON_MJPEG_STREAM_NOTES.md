# Jetson MJPEG 스트림 — 구현 전 확인 사항

카메라 화면과 분류 결과(class_id)를 브라우저로 겹쳐 보기 위한 MJPEG HTTP 스트림
요청이다. 방향 자체는 맞다 — 지금 분류 파이프라인은 카메라 앞에 무엇이 있었는지
기록이 없어서, 오늘 Jetson 연동 테스트(`docs/DB_ARTY_BRINGUP_REPORT.md` §5)에서도
class_id가 실제로 맞았는지 판단하지 못했다. MJPEG로 바운딩박스·class를 겹쳐 보면
이 공백이 메워진다.

## 반드시 지킬 것 — 분류 루프와 별도 스레드/프로세스로 분리한다

`jetson/tools/jetson_roi_client.cpp:82-124`의 현재 메인 루프는 **완전 동기
블로킹**이다.

```cpp
while (!stop_requested.load()) {
    capture.captureFrame(frame, 1000);          // 캡처 대기
    candidates = proposer.propose(frame, ...);  // YOLO 추론
    for (const auto& candidate : candidates) {
        client.classify(*prepared, result);     // ROI 하나당 TCP 왕복
    }
    ++frame_id;                                 // 이게 다 끝나야 다음 프레임
}
```

프레임 버퍼링도 프레임 스킵도 없다. 실측으로 프레임당 처리 시간이 장면의 ROI
개수에 따라 400ms~수십 ms까지 크게 흔들린다 (20초에 50프레임이 나온 경우와
60초에 1130프레임이 나온 경우를 같은 코드로 관측했다).

**이 루프 안에 MJPEG 인코딩·전송을 순서대로 끼워 넣으면 안 된다.** 이유 두 가지:

1. 가뜩이나 느린 처리(프레임당 최대 400ms대)가 JPEG 인코딩만큼 더 느려진다.
2. 더 심각한 문제 — 브라우저 쪽 TCP 소켓에 `write()`가 블로킹된다. 브라우저 탭을
   닫거나 네트워크가 잠깐 느려지면 그 `write()`가 멈추고, **분류 루프 전체가
   같이 멈춘다.** MJPEG 서버 구현에서 가장 흔한 함정이다.

## 권장 구조

- 캡처는 한 곳에서만 하고, 최신 프레임을 분류 스레드와 MJPEG 서버 스레드가 각자
  읽어가는 구조로 분리한다 (예: mutex로 보호한 "최신 프레임 1장" 공유, 또는
  bounded queue).
- MJPEG 서버 쪽 소켓 쓰기는 넌블로킹으로 하거나 타임아웃을 짧게 건다. 느린
  클라이언트는 그 프레임을 건너뛰게 하고, 절대 프로듀서(캡처/분류 루프)를
  기다리게 하지 않는다.
- 분류 요청(TCP `classify()`) 자체도 이미 블로킹이라 여기 발목을 잡을 수 있다 —
  MJPEG 작업이 이 스레드와 절대 같은 스레드에 있으면 안 된다.

## 참고

- 기존 TCP 클라이언트 코드: `jetson/src/network/TcpRoiClient.{hpp,cpp}`
- 프로토콜 정의: `shared/include/roi_protocol.h`
- 오늘 관측한 파이프라인 처리율 편차: `docs/DB_ARTY_BRINGUP_REPORT.md` §5
