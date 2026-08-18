# adas_project_sub

Arty Z7-20(XC7Z020) + Jetson Nano 기반 ADAS 서브 프로젝트다.

KR260 본 프로젝트([kr260_adas_ps](https://github.com/clanadian/kr260_adas_ps))의
대체가 아니라 병행 경로다. KR260 복귀는 전제이나 확정은 아니므로, **여기서 만든
것은 KR260으로 되돌릴 수 있어야 하고 본 저장소는 그대로 살아 있어야 한다**는
제약을 지킨다.

## 구조

카메라 입력과 ROI 생성을 Jetson이 맡고, FPGA는 crop 단위 INT8 CNN 분류를
가속한다. 제한된 PL 자원에서 전체 프레임 탐지 대신 연산이 집중되는 분류
구간만 가속하도록 시스템을 나눈 형태다.

```text
Jetson Nano                     Arty Z7-20
  프레임 캡처                     PS(A9): crop 수신, PL 구동
  ROI 후보 탐색          →        PL: 소형 INT8 분류 CNN
  crop 리사이즈 + INT8   ←        class, confidence
  박스 결합, 위험도 판단
```

| 디렉터리 | 내용 |
| --- | --- |
| `common/` | 본 저장소에서 복사한 안전 판단 로직. `common/ORIGIN` 참조 |
| `ps/` | A9 Linux — crop 수신, PL 구동, 결과 반환 |
| `pl/` | 소형 분류 CNN HLS |
| `jetson/` | ROI 생성과 전송 |
| `docs/contracts/` | Jetson-PS 전송 포맷, PL 레지스터맵 |

## 본 저장소와 달라지는 것

- **RPU가 없다.** XC7Z020은 dual Cortex-A9뿐이라 Cortex-R5F가 없다. R5 펌웨어,
  `remoteproc`, `r5fss` 노드, `rpu_shared` 예약 메모리는 이식되지 않는다.
  판단 로직 자체(`common/`)는 그대로 살아남고 배치 위치만 바뀐다
- **탐지 구조가 다르다.** 전체 프레임 YOLO가 아니라 Jetson이 ROI를 만들고 PL이
  crop을 분류한다. 22-op descriptor, weight/bias, 골든 벡터는 재사용 불가이고
  crop 데이터셋 재학습과 calibration이 새로 필요하다
- **자원이 크게 다르다.** 기존 설계는 URAM을 conv partial sum에 쓰는데
  XC7Z020에는 URAM이 없다. BRAM 총 4.9Mb 안에 feature map과 partial sum을
  넣는 것이 모델 설계의 첫 제약이다

## 정해둔 것

- **전송은 이더넷을 정본으로 한다.** 대역폭 때문이 아니라 TCP가 프레이밍·
  재전송·백프레셔를 주고, 보드 없이 노트북 소켓만으로 Jetson 쪽을 먼저
  완성할 수 있기 때문이다. SPI는 이더넷이 막힐 때의 대안으로 남긴다
- **ROI는 프레임 단위로 묶어 한 번에 보낸다.** ROI마다 왕복하지 않는다
- **리사이즈와 INT8 양자화는 Jetson에서 끝낸다.** PS는 받아서 DDR에 복사만 한다
- **UART 3-byte 프레임 계약을 유지한다.** 송신 주체가 바뀌어도 출력
  인터페이스는 그대로 둔다
- **PL 구동은 `레지스터 프로그래밍 -> start -> done 폴링 -> timeout` 형태를
  따른다.** 본 저장소의 `HardwareAccelerator`와 같은 모양이라야 복귀가 쉽다

## 아직 안 정한 것

- 분류 CNN 입력 해상도 — 64x64로 시작할지 96x96으로 시작할지는 합성 결과를
  보고 정한다. 속도제한 표지판의 숫자 구분에는 64x64가 빠듯하다
- 안전 판단과 UART 송신 주체 — Jetson인지 PS인지. 팀 합의 필요
- Arty Z7-20의 USB 포트가 device(gadget) 가능한지 — 되면 USB CDC-NCM도 선택지다
