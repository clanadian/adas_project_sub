# Identical to repo-root config.py; kept here so the ROS2 package is self-contained.
from enum import IntEnum


class SafetyState(IntEnum):
    CLEAR = 0
    SLOW = 1
    STOP = 2


class FSMState(IntEnum):
    NORMAL = 0
    STOPPING = 1
    HANDLED = 2


CONF_THRESHOLD = 0.35
NMS_THRESHOLD = 0.4
DISPLAY_CONF_THRESHOLD = 0.35

USE_ROI = False
ROI_X_MIN = 0.3
ROI_X_MAX = 0.7

STOP_DURATION_SEC = 3.0
MISS_FRAME_THRESHOLD = 10

# Per-class confidence and bounding-box distance policy lives in safety_judge.py.
