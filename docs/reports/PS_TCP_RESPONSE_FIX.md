# Arty PS — 응답 전송 40 ms 지연 수정 요청

작성 2026-08-20 · 대상 `arty/ps_db/src/network/tcp_roi_server.c`
· 저장소 `clanadian/adas_project_sub` @ `9d2e13c`

---

## 0. 한 줄 요약

**ROI 왕복 51.6 ms 중 실제 계산은 8 ms 뿐이고, 43 ms 는 PS 가 응답을 두 번에
나눠 `send()` 해서 생긴 TCP 대기다.** 젯슨 쪽에서 우회해 **9.7 ms** 로 줄여
놨지만(5.3배), 근본 수정은 PS 쪽이다.

| | 수정 전 | 수정 후 | |
|---|---:|---:|---|
| ROI 왕복 중앙값 | 51.608 ms | **9.706 ms** | 5.3× |
| ROI 처리량 | 7.15 /s | **40.85 /s** | 5.7× |
| ROI 1개 프레임 | 80.07 ms (12.5 FPS) | **38.66 ms (25.9 FPS)** | 2.1× |
| ROI 2개 프레임 | 132.20 ms (7.6 FPS) | **48.50 ms (20.6 FPS)** | 2.7× |
| ROI 3개 프레임 | 184.09 ms (5.4 FPS) | **58.26 ms (17.2 FPS)** | 3.2× |

표본: 수정 전 4,670 ROI / 653 s, 수정 후 4,118 ROI / 101 s. 같은 보드·같은
비트스트림·같은 카메라. **PL 은 한 줄도 안 건드렸다.**

---

## 1. 증상

젯슨이 재는 ROI 왕복이 항상 50~54 ms 로 고정이었다. 그런데 보드가 자체 측정한
가속기 실행 시간은 **6.6 ms** 다 (golden 테스트 `accelerator time=6613 us`,
report.md 의 `PL 실행 DB 6.604 ms` 와 일치).

44 ms 가 어디로 가는지가 문제였다. 배선은 직결 이더넷이라 27,676 바이트 전송
자체는 100 Mbit 기준 2.2 ms 밖에 안 된다.

**결정적 단서:** 요청이 뜸할 때는 같은 왕복이 10.5 ms 로도 나왔다.

```
TCP round-trip (per ROI)   n=60   median 10.536   mean 29.952   p95 54.109
```

같은 크기의 같은 요청인데 값이 **10 ms 와 54 ms 두 덩어리로 갈렸다.** 계산이
원인이면 이렇게 안 갈린다. 차이가 정확히 ~40 ms 인 것도 그냥 지나칠 수 없었다 —
리눅스 delayed-ACK 타이머 값이다.

---

## 2. 원인 — tcpdump 로 확인

RPi 가 젯슨(192.168.100.2) 과 아티(10.10.16.61) 사이를 라우팅하므로 RPi 에서
그대로 잡힌다.

```bash
sudo tcpdump -i enx00e04c680398 -nn -ttt 'tcp port 5000'
```

한 왕복이 이렇게 나온다 (`-ttt` = 직전 줄로부터의 경과):

```
 0.000000  젯슨 → 아티  length 20      요청 헤더
 0.000076  젯슨 → 아티  length 8688    이미지
 0.000147  젯슨 → 아티  length 13032
 0.000099  젯슨 → 아티  length 5956
 0.000337  아티 → 젯슨  ACK            즉시 받았다
 0.007805  아티 → 젯슨  length 20   ←  응답 헤더. 여기까지가 계산 전부 (8 ms)
 0.040777  젯슨 → 아티  ACK         ←  40 ms 공백
 0.000453  아티 → 젯슨  length 12   ←  응답 본문. ACK 받고 0.45 ms 만에 나왔다
```

**아티는 답을 8 ms 만에 다 만들어 놓고 40 ms 동안 못 보내고 있었다.**

이유는 `tcp_roi_server.c:258` `adas_tcp_roi_server_send_result()` 가 응답을
**두 번에 나눠 보내기** 때문이다:

```c
// tcp_roi_server.c:289
adas_tcp_roi_status_t status = send_all(
    server->client_fd,
    header_buffer,          // 20 바이트
    sizeof(header_buffer)
);
if (status != ADAS_TCP_ROI_OK) { return status; }

// tcp_roi_server.c:299
status = send_all(
    server->client_fd,
    result_buffer,          // 12 바이트  ← 이게 40 ms 붙들린다
    sizeof(result_buffer)
);
```

**Nagle 알고리즘**은 "아직 ACK 안 온 데이터가 있으면, 작은 조각은 모아서 보낸다"
이다. 첫 20 바이트가 아직 ACK 되지 않았으므로 두 번째 12 바이트가 커널 버퍼에
붙들린다. 받는 쪽(젯슨)은 **delayed-ACK** 로 최대 40 ms 까지 ACK 을 미룬다.
서로가 서로를 기다리다가 타이머가 터져야 풀린다.

> 요청 쪽(젯슨 → 아티)은 27 KB 라 세그먼트가 계속 나가고, 받는 쪽이 두 세그먼트마다
> 즉시 ACK 을 내주므로 이 교착이 안 생긴다. **작은 write 두 번**일 때만 걸린다.

---

## 3. 조치 A — 지금 당장 (코드 수정 0줄)

서버에 **이미 스위치가 있다.** 다만 **기본값이 꺼짐**이다.

```c
// ps_classifier_server.c:405
const int no_delay_requested = env_ulong("ADAS_TCP_NODELAY", 0ul) != 0ul;
```

```c
// ps_classifier_server.c:462
if (no_delay_requested
    && adas_tcp_roi_server_set_no_delay(&server, 1) != ADAS_TCP_ROI_OK) {
    perror("warning: failed to set TCP_NODELAY");
}
```

그러니 서버를 띄울 때 환경변수만 주면 된다:

```bash
ADAS_TCP_NODELAY=1 ADAS_UART_PORT=/dev/ttyPS1 ./ps_classifier_server ...
```

기동 로그에 `tcp_nodelay=on` 이 찍히면 적용된 것이다.

**주의 — 이건 완치가 아니라 진통제다.** Nagle 을 꺼서 12 바이트가 즉시 나가게 할
뿐, 응답은 여전히 **패킷 2개**로 쪼개져 나간다. 아래 조치 B 를 하면 이 환경변수는
있으나 없으나 상관없어진다.

---

## 4. 조치 B — 근본 수정 (권장)

응답 헤더와 본문은 **항상 붙어서 같이 나간다.** 애초에 나눠 보낼 이유가 없다.
합쳐서 한 번에 쓰면 Nagle 이 붙들 "두 번째 작은 write" 자체가 없어진다.

`arty/ps_db/src/network/tcp_roi_server.c` 의 `adas_tcp_roi_server_send_result()`
끝부분(현재 286~309행)을 이렇게 바꾼다.

**현재:**

```c
    uint8_t header_buffer[ADAS_ROI_HEADER_SIZE];
    uint8_t result_buffer[ADAS_ROI_RESULT_PAYLOAD_SIZE];

    adas_roi_encode_header(header_buffer, &response_header);
    adas_roi_encode_result(result_buffer, result);

    adas_tcp_roi_status_t status = send_all(
        server->client_fd,
        header_buffer,
        sizeof(header_buffer)
    );

    if (status != ADAS_TCP_ROI_OK) {
        return status;
    }

    status = send_all(
        server->client_fd,
        result_buffer,
        sizeof(result_buffer)
    );

    if (status != ADAS_TCP_ROI_OK) {
        return status;
    }

    return ADAS_TCP_ROI_OK;
```

**수정 후:**

```c
    /*
     * 헤더(20B)와 결과(12B)를 한 버퍼에 담아 write 한 번으로 보낸다.
     *
     * 두 번에 나눠 보내면 Nagle 이 두 번째 작은 write 를 "첫 20B 의 ACK 이
     * 올 때까지" 붙들고, 받는 쪽 delayed-ACK 가 최대 40ms 뒤에 오므로 그
     * 시간이 통째로 왕복에 얹힌다. 2026-08-20 실측에서 왕복 51.6ms 중
     * 43ms 가 이 대기였다 (PS_TCP_RESPONSE_FIX.md).
     *
     * 32 바이트라 스택에 두어도 되고, 한 번의 write 이므로 부분 전송
     * 처리도 send_all 이 그대로 해 준다.
     */
    uint8_t response_buffer[
        ADAS_ROI_HEADER_SIZE + ADAS_ROI_RESULT_PAYLOAD_SIZE];

    adas_roi_encode_header(response_buffer, &response_header);
    adas_roi_encode_result(
        response_buffer + ADAS_ROI_HEADER_SIZE,
        result
    );

    return send_all(
        server->client_fd,
        response_buffer,
        sizeof(response_buffer)
    );
```

- 프로토콜은 **하나도 안 바뀐다.** 와이어에 나가는 바이트 순서·값이 동일하다.
  클라이언트는 어차피 헤더 20B 를 읽고 본문 12B 를 읽으므로 수정 불필요.
- `send_all` 이 부분 전송 루프를 이미 갖고 있어 그대로 쓰면 된다.
- `ADAS_ROI_RESULT_PAYLOAD_SIZE` 는 `shared/include/roi_protocol.h` 에서
  12 로 고정이므로 버퍼 크기는 컴파일 타임에 결정된다.

### EB 비교 구현

EB 백엔드는 폐기했고 원본 소스는 Git 태그
`eb-comparison-final`에 보존했다. 현재 트리의 응답 최적화·테스트
대상은 DB 백엔드 하나다.

---

## 5. 왜 A 가 아니라 B 인가

| | A: `ADAS_TCP_NODELAY=1` | B: write 합치기 |
|---|---|---|
| 지연 제거 | O | O |
| 패킷 수 | 2개 (헤더/본문 따로) | **1개** |
| 켜는 걸 잊으면 | **다시 40 ms** | 해당 없음 |
| 다른 클라이언트가 붙으면 | 그쪽도 영향 | 해당 없음 |
| 되돌릴 위험 | 실행 스크립트 하나 고치면 재발 | 코드에 박힘 |

A 는 **"운영자가 매번 기억해야 하는 설정"** 이다. 이번에도 그 스위치가 있는 줄
모르고 40 ms 를 그대로 먹고 있었다. B 로 고치면 잊을 것 자체가 없어진다.

**둘 다 하는 것을 권한다** — B 로 고치고, A 는 그대로 둔다(요청 수신 쪽에도
도움이 되고, 해가 없다).

---

## 6. 지금 젯슨에 들어가 있는 임시 우회

아티에 SSH 를 못 붙는 상태여서(재플래시 후 authorized_keys 소실) 받는 쪽에서
막았다. `jetson/src/network/TcpRoiClient.cpp` 에 `armQuickAck()` 를 추가하고
응답 수신 직전에 호출한다 — `TCP_QUICKACK` 로 delayed-ACK 를 건너뛰고 즉시
ACK 을 보낸다. 리눅스가 quickack 을 몇 번 쓰면 스스로 되돌리므로 **요청마다 다시
켠다.**

**PS 를 B 로 고친 뒤에도 이 코드는 그대로 둬도 된다.** 해가 없고, 앞으로 비슷한
지연이 생겨도 클라이언트가 알아서 막아 준다. 굳이 지우고 싶으면 PS 수정이
보드에 올라간 것을 확인한 다음에 지운다 — 순서를 바꾸면 40 ms 가 조용히
돌아온다.

`jetson/tools/jetson_roi_client.cpp` 는 요청을 **헤더 20B / bbox 28B / 이미지
27,648B 세 번에 나눠** 보낸다. 지금은 `ADAS_TCP_NODELAY=1` 로 가려져 있지만,
앞 두 개(48B)를 한 버퍼에 합치는 게 맞다. 급하지 않아 손대지 않았다.

---

## 7. 검증 절차

### 7.1 패킷으로 (가장 확실함)

RPi 에서:

```bash
sudo tcpdump -i enx00e04c680398 -nn -ttt 'tcp port 5000' -c 120
```

**고쳐졌다면** 응답 헤더와 본문 사이에 40 ms 공백이 없어야 한다. 조치 B 로
고쳤다면 아예 **`length 32` 패킷 하나**로 나온다.

```
 0.007804  아티 → 젯슨  length 32   ← 헤더+본문 한 번에
```

조치 A 만 했다면 패킷은 둘이지만 간격이 1 ms 미만이어야 한다.

### 7.2 앱 수치로

젯슨 클라이언트를 잠깐 돌리고 종료(Ctrl-C)하면 요약이 나온다.

```
  TCP round-trip (per ROI)   median  9.7 ms   ← 50 ms 대가 나오면 안 고쳐진 것
```

`median` 이 50 ms 대면 아직 걸려 있는 것이다. 9~11 ms 면 정상이다.

### 7.3 단위 테스트

`arty/ps_db/tests/test_tcp_roi_server.c:242` 가 `send_result` 를 부르고 있다.
**수정 후에도 그대로 통과해야 한다** — 와이어 바이트가 안 바뀌기 때문이다.
통과하지 않으면 인코딩 오프셋을 잘못 짚은 것이니 그 자리에서 잡을 수 있다.

여유가 있으면 이런 검사를 추가하면 회귀를 막을 수 있다:

- `send_result` 호출 한 번에 소켓 write 가 **1회**인지 (`send` 를 감싼 카운터)
- 받은 32 바이트가 `헤더 20 + 결과 12` 로 정확히 파싱되는지

---

## 8. 이걸 고치면 무엇이 달라지나

수정 전 병목은 **가속기가 아니라 TCP 였다.** ROI 1개 늘 때마다 프레임이 51 ms 씩
길어졌는데 그중 43 ms 가 대기였다.

수정 후 병목은 **젯슨의 YOLO proposal 28.2 ms** 로 옮겨간다. 이제 ROI 하나
추가 비용이 9.7 ms 라, ROI 가 여러 개인 장면에서 특히 크게 차이가 난다
(ROI 3개 프레임: 184 ms → 58 ms).

즉 **PL 최적화(DSP packing, conv/pool 융합)로 벌어 놓은 이득이 그동안 TCP 대기에
묻혀 있었다.** report.md 의 "end-to-end 는 DB·EB 둘 다 14.6~14.8 ROI/s 로 동일"
이라는 결론도 이 지연을 포함한 값이므로, 고친 뒤 다시 재 볼 필요가 있다.

---

## 9. 체크리스트

- [x] `arty/ps_db/src/network/tcp_roi_server.c` — `send_result` 를 1회 write 로
- [x] EB 백엔드 폐기 및 `eb-comparison-final` 태그로 보존
- [x] 단위 테스트 통과 확인 (`test_tcp_roi_server`)
- [ ] 보드에 재배포 후 tcpdump 로 40 ms 공백 소멸 확인 (§7.1)
- [ ] 젯슨 요약에서 RTT median 10 ms 대 확인 (§7.2)
- [ ] 실행 스크립트에 `ADAS_TCP_NODELAY=1` 유지 (해가 없고 요청 쪽에 도움)
- [ ] report.md 성능 수치 재측정 — 기존 값은 40 ms 대기를 포함한 것
- [ ] (선택) 젯슨 요청 측 헤더+bbox 를 한 버퍼로

---

## 10. 참고

- 젯슨 측정 요약 원본: 젯슨 `/tmp/jetson_qa.log`, `/tmp/jetson.log`
- 가속기 단독 시간 근거: golden 테스트 `accelerator time=6613 us`
- 젯슨 접속: `ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2`
  (유저명은 `jetson`이며 RPi를 반드시 경유한다.)
- 아티는 재플래시로 SSH 키가 날아가 접속 불가 상태다. 이 문서의 수정은
  보드에 직접 붙을 수 있는 사람이 해야 한다.
