# Jetson Proposal Model — Deployment Spec

Class-agnostic YOLOv8n proposal model. Produces up to 10 candidate object
bboxes + objectness (`proposal_score`) from a 640×360 BGR camera frame, for
downstream 96×96 crop → Arty Z7-20 ROI classifier.

Status: FP32 ONNX exported and verified against PyTorch (2026-08-19). Ready
for Jetson-side TensorRT conversion + C++ integration + FPS measurement.

```
checkpoint:  runs/proposal_yolov8n_square_320/weights/best.pt
             (square mode, imgsz=320, stopped at epoch 18/50 — validation
             recall/mAP had plateaued for ~5 epochs, see §7)
onnx:        runs/proposal_yolov8n_square_320/export/proposal_yolov8n.onnx
golden:      runs/proposal_yolov8n_square_320/export/golden_sample.json
```

A second checkpoint trained at imgsz=480 (`square`, capped at 20 epochs) is
still running in the background to see whether more input resolution helps
small-object recall in general — see §6/§7 for why it's not blocking this
handoff. If it turns out meaningfully better, an updated ONNX will follow the
same export/verify process and get handed off separately.

## 1. Input tensor

```
name:   images
shape:  [1, 3, 320, 320]   (imgsz=320; see note on rect mode below)
dtype:  float32
layout: NCHW, channel order RGB, values in [0, 1]
```

## 2. Preprocessing (must match exactly — see `decode_reference.letterbox_preprocess`)

1. Camera frame: 640×360, BGR, uint8.
2. **Letterbox** to 320×320: `scale = min(320/640, 320/360)`, resize keeping
   aspect ratio, then pad to a 320×320 square.
   - **Pad value: (114, 114, 114) — Ultralytics' default gray, NOT black.**
     This is the single easiest thing to get wrong porting to C++; the model
     is trained against gray padding, and black padding will visibly hurt
     accuracy without crashing anything.
3. BGR → RGB.
4. `pixel / 255.0` → float32. No mean/std subtraction.
5. HWC → CHW, add batch dim.

Record the letterbox `scale`, `pad_x`, `pad_y` used for a given frame — step
3 of postprocessing needs them to invert back to original coordinates.

## 3. Output tensor

```
name:   output0
shape:  [1, 5, N]      (N = 2100 for imgsz=320: 40x40 + 20x20 + 10x10 anchor points)
layout: channel 0-3 = box (cx, cy, w, h), ALREADY DECODED to 0..320 pixel
        space (the model's input pixel space) — not normalized 0..1, and not
        raw regression offsets requiring further anchor-decoding.
        channel 4  = objectness/class score, ALREADY sigmoid-activated
                     (0..1) — do not apply sigmoid again.
```

Confirmed empirically against this project's actual nc=1 architecture (not
assumed from general YOLOv8 docs) — see `decode_reference.py`'s module
docstring for how.

## 4. Postprocessing → final proposals

See `decode_reference.decode_output()` for the literal reference
implementation. Steps:

1. Transpose `[1,5,N] -> [N,5]`.
2. Filter by `score >= conf_threshold`.
3. Convert center-xywh (320-space) → xyxy (320-space).
4. **Undo letterbox**: `x = (x_320 - pad_x) / scale`, same for y, using the
   SAME scale/pad recorded during preprocessing of this frame. Clip to
   `[0, 640] x [0, 360]`.
5. Sort by score descending.
6. NMS at `nms_iou` (candidate: see §6).
7. Keep top **10** by score.
8. **Convert center/xyxy to the deployment contract: top-left (x, y) +
   (width, height)** — `x = x1, y = y1, width = x2-x1, height = y2-y1`.
   Ultralytics' own `boxes.xywh` is CENTER-based; do not pass that through
   directly, it does not match this contract.

Final output per proposal: `{x, y, width, height, proposal_score}`, in
original 640×360 pixel coordinates, at most 10 entries, sorted by score
descending.

## 5. conf_threshold / nms_iou — **DECIDED: conf=0.10, iou=0.45**

`eval_proposal.py` swept conf ∈ {0.10, 0.20, 0.30} × iou ∈ {0.45, 0.60}. conf=0.10
consistently gave the best recall_top10 across every split (both the global
val set and the demo-relevant subset, §7) without a materially worse
`frac_images_over_10_proposals` than the stricter thresholds — lower conf
just means more candidates survive to the top-10 cut, which is exactly what
we want per the recall-over-precision instruction (the FPGA classifier's
`background` class absorbs the extra false positives). iou=0.45 vs 0.60 made
almost no difference at this conf level, so we kept 0.45 (Ultralytics' own
default).

## 6. Resolution note (imgsz=320 square vs rect) — turned out to be low-priority

640×360 is 16:9. A 320×320 square letterbox only gives ~320×180 of real
image content (padded top/bottom by 70px each side), so this was flagged as
a risk for small distant signs before training. In practice (§7),
`recall_top10_small_objects` on the global set is weak (27.8%), but the
demo-relevant subset has **zero** objects small enough to trigger this
concern at all — the actual demo photos are close-range. So imgsz=320 square
is being shipped as-is for this handoff; the imgsz=480 run in progress is a
robustness improvement for the general/BDD/TT100K case, not a blocker.
`train_proposal.py --mode rect` remains available if a rectangular input is
ever worth revisiting.

## 7. Validation results (val split, conf=0.10, iou=0.45)

Two very different numbers depending on which images are in scope — see
`runs/proposal_yolov8n_square_320/eval_val.json` (global) and
`eval_val_v2_corrected_focus_my_first_project_v1_my_first_project_v2.json`
(demo-relevant subset only):

```
                          global (8,827 img)   demo subset (4,249 img)
standard mAP50                    0.605                 —
standard mAP50-95                 0.354                 —
recall_top10 (overall)            53.2%                 96.3%
  car                             46.4%                 92.6%
  person                          92.4%                 98.7%
  sign_warning                    65.8%                100%
  sign_prohibition                51.5%                100%
  sign_mandatory                  68.6%                100%
recall_top10_small_objects        27.8%           (0 small objects present)
avg proposals/image               10.6                  5.7
frac images >10 proposals         41.7%                 5.5%
```

The global number is dragged down almost entirely by BDD100K's dense
multi-car street scenes (often >10 real cars in frame, so the top-10 cap
itself removes true positives) and TT100K's small distant signs — neither
scenario the actual demo will encounter. The **demo-relevant subset (My First
Project + its corrected-focus derivatives) is what should be trusted for
go/no-go judgment**, and it's at 96.3% recall with zero small objects. The
480px run in progress is aimed at the global/BDD/TT100K weaknesses, not at
the demo scenario itself, which is already in good shape.

Training was stopped at epoch 18/50 (not the full 50) because recall/mAP had
been flat for the preceding ~5 epochs — see the dashboard history for the
per-epoch numbers that justified this call.

## 8. Golden sample (verified, PyTorch vs ONNX Runtime match)

`export/golden_sample.json` — sample image
`my_first_project_v1__-2026-08-06-111720_png.rf.bd954102cd96539fe90033d48e9241ab.jpg`
(demo-relevant source), letterbox params (`scale=0.625, pad_x=0, pad_y=70`),
raw ONNX output shape `[1, 5, 2100]`, and the final decoded proposals from
BOTH PyTorch and ONNX Runtime run on the identical preprocessed tensor:

```
PyTorch vs ONNX Runtime: 8 vs 8 proposals, max_abs_diff = 6.1e-05
```

That's float32 rounding noise, not a real discrepancy — the ONNX export is
numerically faithful to the trained PyTorch model. 8 proposals in this
sample, scores 0.30–0.89. Use this file to cross-check a C++ decode port:
if your C++ output doesn't match `proposals_xywh_score_top_left_onnxruntime`
within a similar tolerance on this exact image, the bug is in the C++ port,
not the model.

## 9. Leakage-safe split

Same principle as `roi_classifier_fp32`: split by original image / Roboflow
augmentation-family (`.rf.<hash>` in filename) *before* anything else, so
derived/augmented copies of one source photo can't land in different
train/val/test splits. Implemented independently in this directory's
`split.py` (intentional duplicate, not an import — see that file's
docstring) so `roi_proposal_jetson/` has zero code dependency on
`roi_classifier_fp32/`.
