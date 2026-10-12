from __future__ import annotations

import json
from enum import IntEnum
from typing import Any


PROTOCOL_VERSION = 1
MAX_SERIAL_LINE_BYTES = 1024


class MessageType(IntEnum):
    HEARTBEAT = 0x01
    PACKET_DATA = 0x02
    COMMAND = 0x03
    INVERT_TIME = 0x03
    GAME_EVENT = 0x04
    PARADOX = 0x04
    COMMAND_RESULT = 0x05
    REPAIR = 0x05
    CONFIG = 0x06
    BOMB_STATE = 0x06
    HELLO = 0x07
    STATUS = 0x08
    ERROR = 0x0B
    ACK = 0x0A
    DIAGNOSTIC = 0x0E
    PING = 0x0F
    PONG = 0x10


class GatewayEvent:
    HELLO = "hello"
    NODE_STATUS = "node_status"
    PACKET = "packet"
    PACKET_RESULT = "packet_result"
    MISSION_STATE = "mission_state"
    MISSION_RESULT = "mission_result"
    IR_SAMPLE = "ir_sample"
    IR_CONFIG_RESULT = "ir_config_result"
    DIAGNOSTIC = "diagnostic"


def encode_command(command: str, **values: Any) -> bytes:
    payload = {"cmd": command, **values}
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")


def decode_line(line: bytes | str) -> dict[str, Any] | None:
    try:
        text = line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line
        encoded = text.encode("utf-8")
        if len(encoded) > MAX_SERIAL_LINE_BYTES:
            return None
        value = json.loads(text.strip())
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None
    return value if isinstance(value, dict) else None


def packet_status(packet: dict[str, Any]) -> str:
    """Return delivery state from an explicit application result, never from send alone."""
    value = packet.get("delivery_status")
    return value if value in {"SENT", "ACK", "LOST", "RETRY", "INVALID", "DUPLICATE", "UNKNOWN"} else "UNKNOWN"