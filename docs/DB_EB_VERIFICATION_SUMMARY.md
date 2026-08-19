# DB / EB 검증 결과 비교

DB 클럭·DDR 진단 과정은 [`DB_ARTY_BRINGUP_REPORT.md`](DB_ARTY_BRINGUP_REPORT.md),
사용법은 [`ARTY_SD_BOOT_USAGE.md`](ARTY_SD_BOOT_USAGE.md) 참고.

## 결과

| 항목 | DB | EB |
| --- | --- | --- |
| 드라이버 probe | `DMA buffer at 0x1f060000, size 0x13000` | `DMA buffer at 0x1f080000, size 0x5b000, 6 ops` |
| PL 실측 | `PASS: 9216 bytes bit-exact, 6570 us` | op0~op5 전부 `PASS bit-exact`, 합계 `12336 us` |
| TCP golden 대조 | `class_id=2, confidence_ppm=992666` (기대값과 일치) | `class_id=2, confidence_ppm=776093` (기대값과 일치) |
| Jetson 실카메라 — 요청/오류 | 236 요청 중 `status=0` 236/236 (confidence 구현 전) + 이후 재연동 시 다수 프레임 오류 0 | 연속 다수 프레임 `status=0`, 오류 0 (정확한 총 요청 수는 재지 않음) |
| Jetson 실카메라 — confidence_ppm | 1차(구현 전): **항상 0** · 2차(구현 후): 실측 **62.9%~99.8%** (아래 예시) | 실측 52.0%~82.6% (아래 예시) |
| INT8 실측 정확도 | 90.33% | 89.67% |
| FP32 정확도 | 94.33% | 94.83% / 94.25% (val/test) |

## 두 테스트의 환경 차이

| 항목 | DB | EB |
| --- | --- | --- |
| 커널 드라이버 | 기존 존재 | 오늘 신규 작성 (레시피·dtsi 포함) |
| PL 검증 단위 | 최종 출력 하나로 대조 | op 6개(엔진 3개)를 개별 대조 |
| confidence_ppm 구현 여부 (라이브 테스트 시점 기준) | 1차는 미구현, 2차는 구현 후 (재연동해서 둘 다 있음) | 구현 후에만 테스트 |
| 양자화 파이프라인 | 표준 | leaky 파인튜닝 + CLE + percentile calibration 추가 |
| 테스트 시각 | 먼저 | 나중 (같은 날, 수 시간 차) |
| 카메라 위치·조명·피사체·배경 | 동일 (변경 없음) | 동일 (변경 없음) |

**DB 1차 라이브(초기 236건 테스트)의 confidence는 EB와 비교할 수 없다**
— 그 시점엔 confidence 계산 자체가 없어서 전부 0이었다. 이후 confidence를
구현하고 나서 DB를 다시 붙여 2차로 잰 값은 EB와 조건이 같아 비교 가능하다.

DB 2차 라이브 로그 실제 예시 (confidence 구현 후, frame 53~63, 연속 12건):

```text
frame=53 roi=2 status=0 class=4 confidence_ppm=676607
frame=54 roi=0 status=0 class=1 confidence_ppm=956741
frame=55 roi=0 status=0 class=1 confidence_ppm=966210
frame=56 roi=0 status=0 class=1 confidence_ppm=964158
frame=57 roi=0 status=0 class=1 confidence_ppm=962728
frame=57 roi=1 status=0 class=3 confidence_ppm=921105
frame=58 roi=0 status=0 class=1 confidence_ppm=997661
frame=59 roi=0 status=0 class=1 confidence_ppm=679741
frame=60 roi=0 status=0 class=1 confidence_ppm=830129
frame=61 roi=0 status=0 class=1 confidence_ppm=948401
frame=62 roi=0 status=0 class=1 confidence_ppm=922854
frame=63 roi=0 status=0 class=4 confidence_ppm=628624
```

EB 라이브 로그 실제 예시 (frame 62~67, 연속 12건):

```text
frame=62 roi=1 status=0 class=2 confidence_ppm=658523
frame=63 roi=0 status=0 class=1 confidence_ppm=750687
frame=63 roi=1 status=0 class=2 confidence_ppm=768035
frame=64 roi=0 status=0 class=1 confidence_ppm=731198
frame=64 roi=1 status=0 class=2 confidence_ppm=795333
frame=65 roi=0 status=0 class=1 confidence_ppm=825541
frame=65 roi=1 status=0 class=2 confidence_ppm=638000
frame=66 roi=0 status=0 class=1 confidence_ppm=717607
frame=66 roi=1 status=0 class=1 confidence_ppm=780641
frame=67 roi=0 status=0 class=1 confidence_ppm=741507
frame=67 roi=1 status=0 class=2 confidence_ppm=520086
frame=67 roi=2 status=0 class=2 confidence_ppm=(생략)
```

## 미해결

INT8 실측 정확도 0.66%p 차이가 실제 열화인지 판단하려면 더 큰 검증셋
재측정이 필요하다 — **요청만 보낸 상태고, 재측정 자체는 아직 안 됐다.**
