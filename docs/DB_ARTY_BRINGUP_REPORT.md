# Arty Z7-20 DB 브링업 보고서 — 2026-08-19

## 1. 결론

DB 보드가 부팅 실패 상태에서 시작해 같은 날 **Jetson 카메라 실사용 조건까지 연동을 확인**했다.
클럭·DDR 설정 오류 두 건을 진단해 PL 팀에 전달했고, 수정된 XSA로 재빌드한 뒤
PL 가속기·PS 서버·Jetson 클라이언트를 실제 하드웨어에서 순서대로 검증했다.

EB 보드는 오전에 이미 부팅·로그인·네트워크까지 확인된 상태였다. EB의 PS 가속기 제어
코드는 아직 DB용 주소맵을 그대로 쓰고 있어 EB 실보드 연동은 별도로 남아 있다 (§6).

## 2. 문제 진단

### 2.1 UART 무음 — 최초 XSA에 UART 자체가 없었음

최초 `classifier_linux` 프로젝트의 XSA는 `PCW_UART_PERIPHERAL_VALID=0`으로 PS UART가
합성 단계에서 아예 빠져 있었다. FSBL이 출력할 UART가 물리적으로 없으므로 SD 카드·
점프퍼 설정과 무관하게 시리얼 콘솔이 항상 무음이었다.

### 2.2 PS 입력 클럭 불일치 — CPU가 정격의 3배로 설정됨

두 번째로 받은 XSA(UART는 켜져 있었음)를 검사한 결과:

```text
PCW_CRYSTAL_PERIPHERAL_FREQMHZ = 33.333333   (Arty Z7-20 실제 크리스털: 50 MHz)
ARMPLL_CTRL_FBDIV = 40  →  33.333 × 40 = 1333 MHz 의도
```

이 분주비가 실제 50 MHz 입력에 적용되면 `50 × 40 = 2000 MHz`가 되어 XC7Z020-1
정격(667 MHz)의 3배를 초과한다. FSBL이 PLL 설정 단계에서 멈추므로 UART가 무음이고
DONE LED도 켜지지 않는다. 같은 보드에서 EB XSA(50 MHz 설정)는 정상 부팅하는 것으로
교차 확인했다.

### 2.3 DDR 물리 구성 불일치

클럭을 수정한 다음 XSA에서 DDR 설정이 Zybo Z7-20 값(32비트 버스)으로 바뀌어 있었다.
EB 팀이 남긴 `arty/pl_eb/system/arty_ps7_preset_z7_20.tcl` 머리말에 Arty Z7-20의
실제 구성이 명시되어 있다.

```text
              받은 XSA              Arty Z7-20 실제
DDR_PARTNO    MT41J128M8 JP-125     MT41J256M16 RE-125
BUS_WIDTH     32 Bit                16 Bit
DRAM_WIDTH    8 Bits                16 Bits
CAPACITY      1024 MBits            4096 MBits
ROW_ADDR      14                    15
```

두 XSA 모두 총 용량은 512 MB로 계산되지만 물리 배선(버스 폭·칩 폭)이 보드와 맞지
않으면 DDR 트레이닝이 실패한다.

## 3. 재발 방지 조치

- [`arty/tools/check_xsa.sh`](../arty/tools/check_xsa.sh) — XSA를 열어 보드 필수
  파라미터 10개(크리스털·DDR 5종·UART·SD MIO)를 자동 대조한다. 하나라도 어긋나면
  실패로 종료한다.
- [`docs/contracts/PL_HANDOFF_CHECKLIST.md`](contracts/PL_HANDOFF_CHECKLIST.md) —
  PL 인계본이 만족해야 할 기준. §1 보드 설정, §3 실가중치 검증 결과 포함 여부를
  명시하고, 각 항목에 오늘 발생한 사고를 근거로 남겼다.
- `arty/classifier_linux_db/project-spec/hw-description/`에 XSA를 먼저 채우고
  그 경로를 `--get-hw-description` 인자로 주면 `mrproper`가 자기 자신을 지우는
  함정이 있다 — mrproper를 먼저 실행하고 그 다음 XSA를 채우도록 순서를 바로잡았다
  (`petalinux` 환경 메모리에 반영).

## 4. 검증 체인

수정된 XSA(`PCW_CRYSTAL_PERIPHERAL_FREQMHZ=50`, DDR `MT41J256M16 RE-125` 16비트)로
`petalinux-build -x mrproper` 후 재빌드해 아래 순서로 확인했다.

| 단계 | 방법 | 결과 |
| --- | --- | --- |
| XSA 보드 설정 | `check_xsa.sh` | 10/10 통과 |
| device tree | `dtc`로 최종 dtb 추출 | `stdout-path=serial0`, DDR `0x20000000`(512MB), UART0/`classifier@40000000` 모두 `status="okay"` |
| 부팅 | UART 콘솔 | FSBL부터 로그인 프롬프트까지 정상 |
| 커널 드라이버 | `dmesg` | `adas_classifier 40000000.classifier: DMA buffer at 0x1f060000, size 0x13000` — probe 성공 |
| **PL 가속기 (실가중치)** | `ps_db_golden_test` | `PASS: 9216 bytes bit-exact, accelerator time=6570 us` — `mismatch_count=0` |
| **PS 서버 (TCP 전체 경로)** | 직접 작성한 ROI1 프로토콜 클라이언트로 golden 벡터 재전송 | `status=0 class_id=2` — golden 기대값(`person`, class_id=2)과 일치 |
| **Jetson 실카메라 연동** | `jetson_roi_client`로 실시간 20초 구동 | 아래 §5 |

`ps_db_golden_test`는 학습팀이 준 실가중치(`arty/models/roi_classifier_int8_db/export/`)를
`/dev/adas_classifier`로 실제 PL에 넣어 `golden_conv2_pool.npy`와 바이트 단위로
대조하는 온보드 테스트다. PL cosim이 아니라 **실리콘에서** 검증했다는 점이 다르다.

## 5. Jetson 실카메라 연동 결과

Jetson(10.10.16.160)에서 `jetson_roi_client`를 카메라(`/dev/video0`)와 TensorRT
YOLOv8n 엔진(`proposal_yolov8n_fp16.engine`)으로 실행해 Arty DB 보드
(10.10.16.61:5000)로 20초간 스트리밍했다.

```text
캡처:  V4L2Capture /dev/video0  YUYV 640x360 @30fps
프레임 범위:  frame=0 ~ frame=49  (검출 있었던 프레임 기준)
ROI 분류 요청: 236건
status=0 (정상): 236 / 236  — 프로토콜·가속기·후처리 오류 0건
```

class_id는 1(car)·2(person)이 주로 나왔고 4(sign_prohibition)도 간헐적으로
나왔다. **카메라 앞 실제 피사체를 확인하지 않았으므로 분류 정확도 자체는 판단하지
않는다** — 여기서 확인한 것은 파이프라인이 실시간 조건에서 오류 없이 지속적으로
동작한다는 사실까지다.

`confidence_ppm`은 항상 0으로 응답됐다. `ps_classifier_server.c`에 `0u`로
하드코딩된 스텁이며 버그가 아니다 — `manifest.json`에도 명시된 대로 분류(argmax) 자체에는
불필요하고, 실수값 confidence가 필요해지면 별도 구현이 필요하다.

## 6. 남은 일

- **EB PS 가속기 코드가 아직 DB 주소맵이다.** `arty/ps_eb/include/accelerator/classifier_registers.h`가
  `design_1.hwh`(DB) 기준 2-region 주소맵을 그대로 쓰고 있다. 실제 EB는 IP 3개
  (conv/conv0/maxpool, 6-op 실행 순서)이므로 EB 실보드에서 그대로 돌리면 존재하지
  않는 레지스터를 건드린다. 참고 자료(`arty_cls_address_map.h`,
  `conv0_engine_hw_driver.h` 등)는 `arty/handoffs/eb/arty96_pl_handoff/sw/`에 있다.
- **`ps_classifier_server`가 아직 rootfs 레시피에 없다.** 오늘은 1회성 검증을 위해
  `arty/classifier_linux_db/build/.../ps-db-golden-test`가 생성한 툴체인 파일을
  재사용해 수동 크로스컴파일 후 `scp`로 올렸다. 정식 배포 전에
  `ps-db-golden-test.bb`처럼 bitbake 레시피로 편입하는 것을 권한다.
- **DB rootfs는 initramfs(`root=/dev/ram0`)라 재부팅하면 IP·가중치·서버 바이너리가
  모두 사라진다.** 매 세션 재설정하거나, rootfs 방식을 바꾸는 결정이 필요하다.
- confidence_ppm 실계산 미구현 (위 §5).
- DB 쪽 PL 가중치 검증(§4의 `ps_db_golden_test`)은 PS 담당이 온보드로 수행한
  것이다. EB처럼 PL 팀이 자체 cosim으로도 검증한 기록(`PROVENANCE.md` 상당)은
  아직 받지 않았다 — `PL_HANDOFF_CHECKLIST.md` §3 기준으로 요청 필요.

## 7. 참고 — 오늘 만든 도구

- [`arty/deploy/burn_sd.sh`](../arty/deploy/burn_sd.sh) — 변종(db/eb) FAT UUID로
  카드를 찾아가 반대쪽 카드에 덮어쓰는 사고를 방지. 포맷은 하지 않고 파일만 갱신한다.
- [`arty/deploy/inspect_sd.sh`](../arty/deploy/inspect_sd.sh) — SD 카드 파티션·FAT
  구조를 덤프해 두 카드를 대조.
- [`arty/tools/check_xsa.sh`](../arty/tools/check_xsa.sh) — §3 참조.
