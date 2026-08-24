"""Shared safety-state values used by the UART receiver and velocity arbiter."""
from enum import IntEnum


class SafetyState(IntEnum):
    CLEAR = 0
    SLOW = 1
    STOP = 2
