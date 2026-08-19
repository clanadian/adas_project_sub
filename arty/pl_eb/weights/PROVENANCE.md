# weights_int8 — 학습팀 INT8 익스포트 (정본)

**받은 날:** 2026-08-19 · **원본:** `roi_classifier_int8_export_arty96.tar.gz`
**아카이브 sha256:** a2d2166453ac75248d4c126d... (앞 24자)

## 이것이 무엇인가

`forps_golden/` 의 가중치는 **여기서 나온다.** 예전에는 `verif_host/gen_golden.c` 가
seed=42 로 난수를 만들었고, 그래서 인계본이 "정확도 0, 배선만 검증됨" 이었다.

## 이 익스포트가 정본 엔진 기준임을 확인한 근거 (2026-08-19)

우리 numpy 참조 모델에 **이쪽 가중치·입력**을 넣고 **이쪽 golden** 과 대조:

| 단계 | 일치율 |
|---|---|
| conv0_out / conv0_pool | 100.00% |
| conv1_out / conv1_pool | 100.00% |
| conv2_out / conv2_pool | 100.00% |
| GAP→FC→argmax | logits 6개 정수 동일, `class_id=2 'person'` |

⚠️ **이 확인이 왜 필요했나:** 2026-08-19 이전에 받은 v2 익스포트는 형제 프로토타입
(`z7_classifier_64_hls`: 순수 ReLU + 절삭 시프트 + clamp[0,127]) 기준이었고,
정본(leaky 13/128 + round_shift + saturate[-128,127])에 넣으니 pool2 일치율이
**20~23%** 였다. **파일 크기·dtype·argmax 는 그때도 통과했다** - 형식 검사로는
못 잡는다. 학습팀 manifest 도 이 점을 스스로 경고하고 있다
(`sibling_variant_note`).

## 레이어별 재양자화 (스탠드인이 아니라 실측)

| 레이어 | multiplier | shift | 활성화 |
|---|---|---|---|
| conv0 | 1545298110 | 37 | leaky 13/128 |
| conv1 | 1525725976 | 36 | leaky 13/128 |
| conv2 | 1924470265 | 39 | **LINEAR** |

## 정확도 (학습팀 보고)

- 전이학습 후 val **94.83%** / test **94.25%**
- INT8 양자화 후: FP32 대비 일치율 **92.67%**, 실측 정확도 **89.67%** (FP32 94.33%)
- CLE 3회로 conv0 채널 범위 편차 24배 → 4배, 가중치양자화만으로 75.5% → 93.5%

## 클래스 (6종, 순서 고정)

`background, car, person, sign_warning, sign_prohibition, sign_mandatory`
