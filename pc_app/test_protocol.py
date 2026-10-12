import unittest

from protocol import PROTOCOL_VERSION, decode_line, encode_command, packet_status
from game_rules import IrConfig, MissionConfig, calculate_score


class ProtocolTests(unittest.TestCase):
    def test_protocol_version_is_explicit(self):
        self.assertEqual(PROTOCOL_VERSION, 1)

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

    def test_sent_is_not_treated_as_delivery_ack(self):
        self.assertEqual(packet_status({"send_status": "SENT"}), "UNKNOWN")
        self.assertEqual(packet_status({"delivery_status": "ACK"}), "ACK")


class GameRuleTests(unittest.TestCase):
    def test_mission_defaults_match_spec(self):
        self.assertEqual((MissionConfig.for_difficulty("easy").duration_seconds,
                          MissionConfig.for_difficulty("easy").fault_interval_seconds,
                          MissionConfig.for_difficulty("easy").required_packets), (300, 20, 5))
        self.assertEqual((MissionConfig.for_difficulty("normal").duration_seconds,
                          MissionConfig.for_difficulty("normal").fault_interval_seconds,
                          MissionConfig.for_difficulty("normal").required_packets), (240, 12, 8))
        self.assertEqual((MissionConfig.for_difficulty("hard").duration_seconds,
                          MissionConfig.for_difficulty("hard").fault_interval_seconds,
                          MissionConfig.for_difficulty("hard").required_packets), (180, 6, 12))

    def test_invalid_mission_config_is_rejected(self):
        with self.assertRaises(ValueError):
            MissionConfig("normal", 10, 12, 8).validate()

    def test_ir_ranges_and_gaps(self):
        config = IrConfig()
        self.assertEqual(config.classify(100), "SHORT")
        self.assertEqual(config.classify(450), "INVALID")
        self.assertEqual(config.classify(700), "MEDIUM")
        self.assertEqual(config.classify(1800), "LONG")
        self.assertEqual(config.classify(4000), "INVALID")

    def test_overlapping_ir_ranges_are_rejected(self):
        with self.assertRaises(ValueError):
            IrConfig(short_max_ms=600).validate()

    def test_score_is_deterministic_and_bounded(self):
        score = calculate_score(delivered_packets=4, required_packets=8,
                                online_nodes=5, required_nodes=5,
                                repaired_faults=1, total_faults=2,
                                seconds_remaining=120, mission_duration=240)
        self.assertEqual((score.delivery, score.stability, score.recovery, score.time), (20, 30, 10, 5))
        self.assertEqual(score.total, 65)

    def test_ignores_invalid_or_non_object_json(self):
        self.assertIsNone(decode_line(b"not json\n"))
        self.assertIsNone(decode_line(b"[1, 2]"))


if __name__ == "__main__":
    unittest.main()