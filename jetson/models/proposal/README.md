# Jetson Nano ROI Proposal 모델

640×360 카메라 프레임에서 **객체 후보 bbox 최대 10개 + objectness**를 생성하는
class-agnostic 모델입니다. 클래스 판단은 하지 않으며, 여기서 나온 후보를 96×96으로
crop해서 Arty Z7-20 FPGA의 ROI 분류기로 넘기는 구조입니다.

`roi_classifier_fp32/`와는 **코드 의존성이 전혀 없습니다** (import 없음). 같은 원본
이미지를 읽기 전용으로 참조하고, leakage 방지 split 로직만 의도적으로 복제해서 씁니다.

## 전달 산출물 (`export/`)

| 파일 | 내용 |
|---|---|
| `proposal_yolov8n.onnx` | FP32 ONNX (12MB). TensorRT engine은 Jetson 현지에서 직접 생성 |
| `golden_sample.json` | 대표 이미지의 letterbox 파라미터, raw 출력 shape, PyTorch·ONNX Runtime 양쪽 디코딩 결과 |
| `eval_val.json` | 전체 val 검증 결과 (conf/NMS 스윕 포함) |
| `eval_val_v2_corrected_focus_my_first_project_v1_my_first_project_v2.json` | 데모 관련 서브셋만의 검증 결과 |

`DEPLOY_SPEC.md`에 입출력 텐서 규격, 전처리·후처리 규칙, 좌표 변환, threshold가
전부 실측값으로 정리되어 있습니다. **C++ 포팅 시 이 문서와 `decode_reference.py`를
같이 보세요.**

## 핵심 계약 (자세한 건 DEPLOY_SPEC.md)

```
입력:  images [1,3,320,320] float32, NCHW, RGB, [0,1]
출력:  output0 [1,5,2100]
       채널 0-3 = box(cx,cy,w,h), 이미 320-pixel space로 디코딩됨
       채널 4   = objectness, 이미 sigmoid 적용됨 (다시 적용하지 말 것)
threshold: conf=0.10, iou=0.45
최종 출력: 좌상단 기준 (x, y, width, height) + proposal_score, 최대 10개
```

**C++ 포팅 시 실수하기 쉬운 3가지**:
1. letterbox padding은 **114 회색**입니다 (검은색 아님). 검은색으로 하면 크래시 없이 정확도만 떨어집니다.
2. 출력 score에 sigmoid를 **다시 적용하면 안 됩니다** (이미 적용됨).
3. Ultralytics의 `boxes.xywh`는 **중심 기준**입니다. 계약은 **좌상단 기준**이라 변환이 필요합니다.

## 검증 결과

```
                          전체 val (8,827장)   데모 서브셋 (4,249장)
recall_top10 (전체)              53.2%                96.3%
  car                            46.4%                92.6%
  person                         92.4%                98.7%
  sign_warning/prohibition/mandatory  65.8/51.5/68.6%   전부 100%
작은 객체 recall                 27.8%          (작은 객체 0개)
```

전체 수치가 낮은 건 BDD100K의 차량 10대 넘는 복잡한 장면(top-10 제한에 걸림)과
TT100K의 멀리 있는 작은 표지판 때문입니다. **실제 데모 환경과 가까운 서브셋에서는
96.3%**이고 작은 객체가 아예 없어서, 데모 관점에서는 충분한 성능입니다.

PyTorch vs ONNX Runtime 출력 비교: 8/8 proposal 일치, 최대 오차 6.1e-05 (float32 반올림 수준).

## 파일 구성

```
build_proposal_dataset.py  5개 소스의 모든 foreground box를 단일 "object" 클래스로
                           통합, leakage-safe split, Ultralytics 레이아웃 생성
                           (원본 세부 클래스는 평가용으로 original_labels.csv에 별도 보존)
split.py                   원본 이미지/augmentation 계열 단위 split (roi_classifier_fp32와
                           동일 로직의 의도적 복제 — 이 디렉터리의 독립성 유지 목적)
classes.py                 단일 object 클래스 + 평가용 원본 클래스 목록
train_proposal.py          YOLOv8n nc=1 학습 (--mode square|rect로 전처리 방식 분리)
eval_proposal.py           recall 중심 커스텀 평가 (top-10 recall, 클래스별, 작은 객체,
                           conf/NMS 스윕, --source-prefix로 소스별 필터링)
decode_reference.py        전처리·후처리 순수 numpy 레퍼런스 (C++ 포팅 기준)
export_onnx.py             ONNX export + PyTorch↔ONNX 대조 검증 + golden sample 생성
DEPLOY_SPEC.md             배포 명세서
```

## 재현 방법

```bash
# 데이터셋 생성 (5개 소스 스캔, leakage-safe split)
yolo_env/bin/python roi_proposal_jetson/build_proposal_dataset.py

# 학습
yolo_env/bin/python roi_proposal_jetson/train_proposal.py --mode square --imgsz 320 --epochs 20

# 평가 (전체 / 데모 서브셋만)
yolo_env/bin/python roi_proposal_jetson/eval_proposal.py --weights <best.pt> --split val
yolo_env/bin/python roi_proposal_jetson/eval_proposal.py --weights <best.pt> --split val \
    --source-prefix v2_corrected_focus my_first_project_v1 my_first_project_v2

# ONNX export + 검증
yolo_env/bin/python roi_proposal_jetson/export_onnx.py --weights <best.pt> --conf 0.10 --iou 0.45
```

## 알려진 사항

- 학습은 50 epoch 기본값으로 시작됐다가 epoch 18에서 중단했습니다. 최근 5 epoch 동안
  recall/mAP가 정체 상태였고(0.543 고정), 남은 32 epoch에 13시간 이상이 필요했습니다.
- imgsz=480 학습을 별도로 돌려 작은 객체 recall 개선 여지를 확인 중입니다. 데모 서브셋은
  이미 96.3%라 배포를 막지 않는 보조 작업이며, 유의미하게 좋으면 같은 export/검증 절차로
  갱신본을 전달할 예정입니다.
