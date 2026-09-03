import struct
import unittest

from telemetry import ProtocolError, decode_observations


class DecodeObservationsTests(unittest.TestCase):
    def test_decodes_binary_batch(self) -> None:
        payload = b"ML" + bytes([1]) + struct.pack(">H", 2)
        payload += struct.pack(
            ">Q6sb", 1_700_000_000_123, bytes.fromhex("aabbccddeeff"), -72
        )
        payload += struct.pack(
            ">Q6sb", 1_700_000_000_456, bytes.fromhex("001122334455"), -31
        )

        observations = decode_observations(payload)

        self.assertEqual(len(observations), 2)
        self.assertEqual(observations[0], (1_700_000_000_123, 0xAABBCCDDEEFF, -72))
        self.assertEqual(observations[1][0], 1_700_000_000_456)

    def test_rejects_wrong_length(self) -> None:
        with self.assertRaises(ProtocolError):
            decode_observations(b"ML\x01\x00\x01")

    def test_rejects_empty_batch(self) -> None:
        with self.assertRaises(ProtocolError):
            decode_observations(b"ML\x01\x00\x00")


if __name__ == "__main__":
    unittest.main()
