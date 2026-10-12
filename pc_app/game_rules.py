from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

Difficulty = Literal["easy", "normal", "hard"]
IrCategory = Literal["SHORT", "MEDIUM", "LONG", "INVALID"]


@dataclass(frozen=True)
class MissionConfig:
    difficulty: Difficulty
    duration_seconds: int
    fault_interval_seconds: int
    required_packets: int
    enabled_faults: tuple[str, ...] = ("INVERSE", "PARADOX", "LOSS", "DELAY")
    fault_intensity: int = 1

    @classmethod
    def for_difficulty(cls, difficulty: str) -> MissionConfig:
        normalized = difficulty.lower()
        if normalized not in {"easy", "normal", "hard"}:
            raise ValueError("Difficulty must be easy, normal, or hard")
        defaults = {
            "easy": (300, 20, 5),
            "normal": (240, 12, 8),
            "hard": (180, 6, 12),
        }
        duration, interval, packets = defaults[normalized]
        return cls(normalized, duration, interval, packets)  # type: ignore[arg-type]

    def validate(self) -> None:
        if not 30 <= self.duration_seconds <= 3600:
            raise ValueError("Mission duration must be between 30 and 3600 seconds")
        if not 2 <= self.fault_interval_seconds <= 300:
            raise ValueError("Fault interval must be between 2 and 300 seconds")
        if not 1 <= self.required_packets <= 100:
            raise ValueError("Required packet count must be between 1 and 100")
        if not 1 <= self.fault_intensity <= 5:
            raise ValueError("Fault intensity must be between 1 and 5")
        allowed = {"INVERSE", "PARADOX", "LOSS", "DELAY", "DUPLICATE", "INVALID_PAYLOAD"}
        if not self.enabled_faults or any(fault not in allowed for fault in self.enabled_faults):
            raise ValueError("Select at least one supported fault type")


@dataclass(frozen=True)
class IrConfig:
    active_high: bool = True
    group_gap_ms: int = 80
    short_min_ms: int = 100
    short_max_ms: int = 400
    medium_min_ms: int = 500
    medium_max_ms: int = 1200
    long_min_ms: int = 1300
    long_max_ms: int = 2500
    minimum_active_ms: int = 50
    maximum_group_ms: int = 3000
    cooldown_ms: int = 800

    def validate(self) -> None:
        if not 10 <= self.group_gap_ms <= 1000:
            raise ValueError("Pulse group gap must be between 10 and 1000 ms")
        if not 1 <= self.minimum_active_ms < self.short_min_ms:
            raise ValueError("Minimum active duration must be positive and below short range")
        if not (self.short_min_ms <= self.short_max_ms < self.medium_min_ms <= self.medium_max_ms < self.long_min_ms <= self.long_max_ms):
            raise ValueError("IR command ranges must be ordered and non-overlapping")
        if self.maximum_group_ms <= self.long_max_ms or self.maximum_group_ms > 10000:
            raise ValueError("Maximum group duration must exceed long range and be <= 10000 ms")
        if not 0 <= self.cooldown_ms <= 10000:
            raise ValueError("Cooldown must be between 0 and 10000 ms")

    def classify(self, active_ms: int) -> IrCategory:
        self.validate()
        if active_ms < self.minimum_active_ms or active_ms > self.maximum_group_ms:
            return "INVALID"
        if self.short_min_ms <= active_ms <= self.short_max_ms:
            return "SHORT"
        if self.medium_min_ms <= active_ms <= self.medium_max_ms:
            return "MEDIUM"
        if self.long_min_ms <= active_ms <= self.long_max_ms:
            return "LONG"
        return "INVALID"


@dataclass(frozen=True)
class MissionScore:
    total: int
    delivery: int
    stability: int
    recovery: int
    time: int


def calculate_score(
    *,
    delivered_packets: int,
    required_packets: int,
    online_nodes: int,
    required_nodes: int,
    repaired_faults: int,
    total_faults: int,
    seconds_remaining: int,
    mission_duration: int,
) -> MissionScore:
    if required_packets <= 0 or required_nodes <= 0 or mission_duration <= 0:
        raise ValueError("Mission denominators must be positive")
    delivery = round(40 * min(max(delivered_packets, 0), required_packets) / required_packets)
    stability = round(30 * min(max(online_nodes, 0), required_nodes) / required_nodes)
    if total_faults <= 0:
        recovery = 20
    else:
        recovery = round(20 * min(max(repaired_faults, 0), total_faults) / total_faults)
    time = round(10 * min(max(seconds_remaining, 0), mission_duration) / mission_duration)
    return MissionScore(delivery + stability + recovery + time, delivery, stability, recovery, time)