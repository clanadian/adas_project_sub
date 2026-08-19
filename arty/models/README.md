# INT8 양자화 산출물

PL 구현이 담당자별로 둘이고 **산술이 서로 다르다.** 그래서 가중치도 둘이다.

| 디렉터리 | 대상 PL | 활성화 | requant 시프트 | 출력 범위 |
| --- | --- | --- | --- | --- |
| `roi_classifier_int8_db/` | `../pl_db/hls/HW/classifier_engine.*` | ReLU (clamp 하한 0에 접힘) | `scaled >> shift`, 라운딩 없음 | `[0, 127]` |
| `roi_classifier_int8_eb/` | `../pl_eb/conv_engine_tr8/HW/conv_engine.*` | LeakyReLU 13/128, requant **이전** 적용 | `round_shift` (반올림 항 있음) | `[-128, 127]` |

## ⚠️ 두 가중치는 교환할 수 없다

가중치가 각자의 산술에 맞춰 양자화돼 있다. 반대쪽에 넣어도
**에러 없이 동작하고 분류 결과만 조용히 틀린다.** 실행 중에는 드러나지 않으므로
어느 쪽을 올렸는지 배포 시점에 확인한다.

공통 파일 21개(가중치·바이어스·골든 벡터·`fixed_point.py`·`golden_int8.py`·
`quantize_export.py`)가 전부 다르다. EB 쪽에는 leaky 파인튜닝용으로
`cle.py`, `finetune_leaky.py`, `export/finetune_result.json`이 추가로 있다.

## 공통 사항

입력 규약(96×96×3 RGB UINT8 → 98×98×3 signed INT8 NHWC)과 PL 출력 형상
(12×12×64 signed INT8 NHWC), GAP/FC/argmax를 PS가 맡는 구조는 두 변종이 같다.
자세한 것은 [`../../docs/contracts/ROI_CLASSIFIER_CONTRACT.md`](../../docs/contracts/ROI_CLASSIFIER_CONTRACT.md).

각 디렉터리의 `export/manifest.json`이 레이어별 scale·requant multiplier/shift·
클래스 순서의 정본이다.
