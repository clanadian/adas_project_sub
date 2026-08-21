from __future__ import annotations

from dataclasses import dataclass, field
from typing import List

from .config import SafetyState


@dataclass
class JudgeResult:
    safety_state: SafetyState
    filtered_detections: List = field(default_factory=list)
    stop_classes: frozenset[str] = field(default_factory=frozenset)


@dataclass(frozen=True)
class _Policy:
    confidence: float
    slow_area: float
    slow_height: int
    stop_area: float = 2.0  # >1 means this class never requests STOP
    stop_height: int = 10**6


# Box area is a fraction of the whole camera image.  The height condition
# prevents a thin false-positive box from changing the robot state.
_POLICIES = {
    # Far person: ignore; approaching person: SLOW; close person: STOP.
    'person': _Policy(.70, .015, 70, .040, 120),
    # Cars never STOP in this demo; a sufficiently large car requests SLOW.
    'car': _Policy(.50, .015, 40),
    # Warning/mandatory signs reduce speed only.
    'sign_warning': _Policy(.70, .006, 35),
    'sign_mandatory': _Policy(.70, .006, 35),
    # A prohibition sign becomes STOP only when close enough to read clearly.
    'sign_prohibition': _Policy(.70, .006, 35, .020, 70),
}


class SafetyJudge:
    """Per-frame hazard classification: detections → CLEAR / SLOW / STOP."""

    def __init__(
        self,
        conf_threshold: float = 0.35,
        use_roi: bool = False,
        roi_x_min: float = 0.3,
        roi_x_max: float = 0.7,
    ) -> None:
        self.conf_threshold = conf_threshold
        self.use_roi = use_roi
        self.roi_x_min = roi_x_min
        self.roi_x_max = roi_x_max

    def judge(self, detections, frame_width: int, frame_height: int) -> JudgeResult:
        worst = SafetyState.CLEAR
        filtered = []
        stop_classes = set()
        for det in detections:
            policy = _POLICIES.get(det.class_name)
            if policy is None:
                continue
            if det.confidence < max(self.conf_threshold, policy.confidence):
                continue
            if self.use_roi:
                cx = (det.x1 + det.x2) / 2.0
                if not (self.roi_x_min * frame_width <= cx <= self.roi_x_max * frame_width):
                    continue
            box_width = max(0, det.x2 - det.x1)
            box_height = max(0, det.y2 - det.y1)
            area_fraction = (box_width * box_height) / float(frame_width * frame_height)
            if area_fraction < policy.slow_area or box_height < policy.slow_height:
                continue  # detected, but too far/small to affect driving
            filtered.append(det)
            if area_fraction >= policy.stop_area and box_height >= policy.stop_height:
                state = SafetyState.STOP
                stop_classes.add(det.class_name)
            else:
                state = SafetyState.SLOW
            if state > worst:
                worst = state
        return JudgeResult(safety_state=worst, filtered_detections=filtered,
                           stop_classes=frozenset(stop_classes))
