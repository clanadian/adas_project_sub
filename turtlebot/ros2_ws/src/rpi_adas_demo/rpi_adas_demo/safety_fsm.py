from __future__ import annotations

import time
from dataclasses import dataclass

from .config import FSMState, MISS_FRAME_THRESHOLD, STOP_DURATION_SEC, SafetyState


@dataclass
class FSMResult:
    safety_state: SafetyState
    fsm_state: FSMState
    stop_timer: float
    miss_count: int
    obstacle_miss_count: int
    reason: str


class SafetyFSM:
    """NORMAL → STOPPING (hold) → HANDLED (drain) → NORMAL debounce machine."""

    def __init__(
        self,
        stop_duration: float = STOP_DURATION_SEC,
        miss_frame_threshold: int = MISS_FRAME_THRESHOLD,
    ) -> None:
        self.stop_duration = stop_duration
        self.miss_frame_threshold = miss_frame_threshold
        self._state = FSMState.NORMAL
        self._stop_start: float | None = None
        self._miss_count = 0
        self._obstacle_miss = 0
        self._latched_stop_classes: frozenset[str] = frozenset()

    def update(self, judge_result) -> FSMResult:
        now = time.monotonic()
        raw = judge_result.safety_state
        stop_classes = judge_result.stop_classes

        if self._state == FSMState.NORMAL:
            if raw == SafetyState.STOP:
                self._state = FSMState.STOPPING
                self._stop_start = now
                self._miss_count = 0
                self._obstacle_miss = 0
                self._latched_stop_classes = stop_classes
                reason, out = "STOP detected", SafetyState.STOP
            else:
                reason, out = "normal", raw

        elif self._state == FSMState.STOPPING:
            elapsed = now - self._stop_start
            out = SafetyState.STOP
            if elapsed >= self.stop_duration:
                self._state = FSMState.HANDLED
                self._miss_count = 0
                self._obstacle_miss = 0
                reason = f"hold {elapsed:.1f}s done"
            else:
                reason = f"stopping {elapsed:.1f}/{self.stop_duration:.0f}s"

        elif self._state == FSMState.HANDLED:
            # A class that caused this stop is deliberately not re-triggered.
            # A newly dangerous stop class, however, immediately begins a new
            # hold event (e.g. person hold followed by prohibition sign).
            new_stop_classes = stop_classes - self._latched_stop_classes
            if new_stop_classes:
                self._state = FSMState.STOPPING
                self._stop_start = now
                self._latched_stop_classes = stop_classes
                self._miss_count = 0
                self._obstacle_miss = 0
                reason, out = "new STOP class detected", SafetyState.STOP
            elif raw == SafetyState.STOP:
                self._obstacle_miss = 0
                self._miss_count = 0
                reason, out = "latched obstacle visible", SafetyState.SLOW
            else:
                self._obstacle_miss += 1
                self._miss_count += 1 if raw == SafetyState.CLEAR else 0
                reason = f"clearing {self._miss_count}/{self.miss_frame_threshold}"

            if self._state == FSMState.STOPPING:
                pass
            elif self._miss_count >= self.miss_frame_threshold:
                self._state = FSMState.NORMAL
                self._stop_start = None
                self._latched_stop_classes = frozenset()
                reason, out = "HANDLED → NORMAL", SafetyState.CLEAR
            elif raw != SafetyState.STOP:
                out = SafetyState.SLOW if raw == SafetyState.STOP else raw

        else:
            reason, out = "unknown", SafetyState.STOP

        return FSMResult(
            safety_state=out,
            fsm_state=self._state,
            stop_timer=(now - self._stop_start) if self._stop_start is not None else 0.0,
            miss_count=self._miss_count,
            obstacle_miss_count=self._obstacle_miss,
            reason=reason,
        )
