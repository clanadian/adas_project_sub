# INT8 양자화 산출물

PL 산술에 맞춰 생성한 DB 백엔드용 INT8 양자화 산출물이다.

| 디렉터리 | 대상 PL | 활성화 | requant 시프트 | 출력 범위 |
| --- | --- | --- | --- | --- |
| `roi_classifier_int8_db/` | `../pl_db/hls/HW/classifier_engine.*` | ReLU (clamp 하한 0에 접힘) | `scaled >> shift`, 라운딩 없음 | `[0, 127]` |

## 공통 사항

입력 규약(96×96×3 RGB UINT8 → 98×98×3 signed INT8 NHWC)과 PL 출력 형상
(12×12×64 signed INT8 NHWC), GAP/FC/argmax를 PS가 맡는 구조를 사용한다.
자세한 것은 [`../../docs/contracts/ROI_CLASSIFIER_CONTRACT.md`](../../docs/contracts/ROI_CLASSIFIER_CONTRACT.md).

`roi_classifier_int8_db/export/manifest.json`이 레이어별 scale·requant multiplier/shift·
클래스 순서의 정본이다.

종료된 EB 양자화 산출물은 Git 태그 `eb-comparison-final`에 보존되어 있다.
