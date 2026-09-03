import struct

MAGIC = b"ML"
VERSION = 1

_HEADER = struct.Struct(">2sBH")
_RECORD = struct.Struct(">Q6sb")

Observation = tuple[int, int, int]


class ProtocolError(ValueError):
    pass


def decode_observations(payload: bytes) -> list[Observation]:
    if len(payload) < _HEADER.size:
        raise ProtocolError("payload is shorter than the header")

    magic, version, count = _HEADER.unpack_from(payload)
    if magic != MAGIC:
        raise ProtocolError("invalid payload magic")
    if version != VERSION:
        raise ProtocolError(f"unsupported payload version {version}")
    if count == 0:
        raise ProtocolError("payload contains no observations")

    expected_size = _HEADER.size + (count * _RECORD.size)
    if len(payload) != expected_size:
        raise ProtocolError(
            f"payload length is {len(payload)} bytes; expected {expected_size}"
        )

    observations: list[Observation] = []
    for offset in range(_HEADER.size, expected_size, _RECORD.size):
        observed_at_ms, mac, rssi = _RECORD.unpack_from(payload, offset)
        observations.append((observed_at_ms, int.from_bytes(mac, "big"), rssi))
    return observations
