import unittest

from tenet_control import decode_line, encode_command


class ProtocolTests(unittest.TestCase):
    def test_command_is_compact_json_with_newline(self):
        self.assertEqual(
            encode_command("start", seconds=10),
            b'{"cmd":"start","seconds":10}\n',
        )

    def test_start_command_carries_difficulty(self):
        self.assertEqual(
            encode_command("start", seconds=60, difficulty="hard"),
            b'{"cmd":"start","seconds":60,"difficulty":"hard"}\n',
        )

    def test_decodes_gateway_packet(self):
        packet = decode_line(b'{"src":4,"dst":255,"type":3,"id":17,"dir":1,"payload":"IR_INVERT","rssi":-48}\r\n')
        self.assertEqual(packet["src"], 4)
        self.assertEqual(packet["payload"], "IR_INVERT")

    def test_decodes_gateway_test_event(self):
        event = decode_line('{"event":"mesh_test_summary","status":"PASS","online":4}')
        self.assertEqual(event["event"], "mesh_test_summary")
        self.assertEqual(event["online"], 4)

    def test_decodes_ir_input_state(self):
        event = decode_line('{"event":"ir_state","level":"HIGH","raw":1,"rssi":-51}')
        self.assertEqual(event["level"], "HIGH")
        self.assertEqual(event["raw"], 1)

    def test_decodes_game_result(self):
        event = decode_line('{"event":"game_state","state":"VICTORY","paradox_mask":0}')
        self.assertEqual(event["state"], "VICTORY")
        self.assertEqual(event["paradox_mask"], 0)

    def test_ignores_invalid_or_non_object_json(self):
        self.assertIsNone(decode_line(b"not json\n"))
        self.assertIsNone(decode_line(b"[1, 2]"))


if __name__ == "__main__":
    unittest.main()