"""Contract and architecture tests for the one-hop ESP-NOW interface."""

import os
import re
import struct
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROTOCOL = os.path.join(ROOT, "ESPNowProtocol.h")
INTERFACE = os.path.join(ROOT, "ESPNowInterface.h")
FIRMWARE = os.path.join(ROOT, "RNode_Firmware.ino")
PROVISIONING = os.path.join(ROOT, "Provisioning.cpp")
PLATFORMIO = os.path.join(ROOT, "platformio.ini")


def source(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def integer_defines():
    values = {}
    for name, raw in re.findall(r"^#define\s+(ESPNOW_\w+)\s+(.+?)\s*$",
                                source(PROTOCOL), re.M):
        raw = raw.split("//", 1)[0].strip()
        try:
            values[name] = int(raw, 0)
        except ValueError:
            pass
    return values


class ESPNowWireContractTests(unittest.TestCase):
    def setUp(self):
        self.d = integer_defines()

    def test_header_is_ten_bytes_and_network_order(self):
        self.assertEqual(10, self.d["ESPNOW_HEADER_SIZE"])
        encoded = struct.pack("!2sBBHBBH", b"RN", 1, 2, 0x1234, 1, 3, 564)
        self.assertEqual(10, len(encoded))
        self.assertEqual(b"RN\x01\x02\x12\x34\x01\x03\x02\x34", encoded)

    def test_esp_idf_44_payload_limit_and_fragment_budget(self):
        wire_mtu = self.d["ESPNOW_WIRE_MTU"]
        header = self.d["ESPNOW_HEADER_SIZE"]
        rns_mtu = self.d["ESPNOW_RNS_MTU"]
        self.assertEqual(250, wire_mtu)
        self.assertEqual(240, wire_mtu - header)
        self.assertEqual(564, rns_mtu)
        self.assertEqual(3, (rns_mtu + (wire_mtu - header) - 1) //
                         (wire_mtu - header))

    def test_all_supported_lengths_reassemble_exactly(self):
        payload = self.d["ESPNOW_WIRE_MTU"] - self.d["ESPNOW_HEADER_SIZE"]
        for length in (1, 239, 240, 241, 479, 480, 481, 564):
            original = bytes((i % 251 for i in range(length)))
            fragments = [original[i:i + payload]
                         for i in range(0, len(original), payload)]
            with self.subTest(length=length):
                self.assertLessEqual(len(fragments), 3)
                self.assertTrue(all(len(part) <= payload for part in fragments))
                self.assertEqual(original, b"".join(fragments))

    def test_discovery_payload_is_fixed_and_advisory(self):
        self.assertEqual(16, self.d["ESPNOW_DISCOVERY_SIZE"])
        protocol = source(PROTOCOL)
        self.assertIn("MUST NOT trigger an automatic PHY change", protocol)
        interface = source(INTERFACE)
        self.assertNotIn("radio_preset_apply", interface)
        self.assertNotIn("update_radio_lock", interface)


class ESPNowArchitectureTests(unittest.TestCase):
    def test_wifi_callback_only_enqueues_raw_bytes(self):
        text = source(INTERFACE)
        callback = text[text.rindex("inline void espnow_receive_trampoline"):]
        callback = callback[:callback.index("inline void espnow_send_trampoline")]
        self.assertIn("receive_from_callback", callback)
        self.assertNotIn("handle_incoming", callback)
        self.assertNotIn("RNS::Bytes", callback)

        enqueue = text[text.index("void receive_from_callback"):]
        enqueue = enqueue[:enqueue.index("void send_from_callback")]
        self.assertIn("memcpy", enqueue)
        self.assertNotIn("handle_incoming", enqueue)
        self.assertNotIn("new ", enqueue)

    def test_reticulum_receives_only_complete_reassemblies_on_loop(self):
        text = source(INTERFACE)
        self.assertIn("drain_inbound();", text)
        self.assertIn("slot->next_fragment == slot->fragment_count", text)
        self.assertIn("handle_incoming(packet);", text)

    def test_link_layer_does_not_forward_or_route(self):
        text = source(INTERFACE)
        self.assertNotIn("Transport::outbound", text)
        self.assertNotIn("register_interface", text)
        self.assertIn("It does not forward ESP-NOW frames or build routes", text)

    def test_state_is_statically_bounded(self):
        text = source(INTERFACE)
        for declaration in ("MAX_PEERS = 12", "REASSEMBLY_SLOTS = 6",
                            "RX_QUEUE_DEPTH = 9", "TX_QUEUE_DEPTH = 5"):
            self.assertIn(declaration, text)
        self.assertIn("Peer _peers[MAX_PEERS]", text)
        self.assertIn("Reassembly _reassembly[REASSEMBLY_SLOTS]", text)

    def test_both_rad_targets_enable_it_and_portable_explicitly_removes_it(self):
        text = source(PLATFORMIO)
        rev1 = text[text.index("[env:impr-rad01-rev1]"):
                    text.index("[env:impr-rad01-rev2]")]
        rev2 = text[text.index("[env:impr-rad01-rev2]"):
                    text.index("[env:impr-rad01-rev2-uart]")]
        portable = text[text.index("[env:impr-rad01-rev1-portable]"):
                        text.index("[env:impr-rad01-rev1]")]
        self.assertIn("-DESPNOW_TRANSPORT", rev1)
        self.assertIn("-DESPNOW_TRANSPORT", rev2)
        self.assertIn("-DESPNOW_TRANSPORT", portable.split("build_unflags", 1)[1])

    def test_interface_is_registered_as_gateway(self):
        text = source(FIRMWARE)
        self.assertIn("espnow_interface.mode(RNS::Type::Interface::MODE_GATEWAY)", text)
        self.assertIn("RNS::Transport::register_interface(espnow_interface)", text)

    def test_espnow_reuses_the_lora_backbone_ifac(self):
        text = source(PROVISIONING)
        block = text[text.index("apply_ifac_configuration(espnow_interface"):]
        block = block[:block.index("#endif")]
        self.assertIn("lora_ifac_enabled || secure_node_enabled", block)
        self.assertIn("lora_ifac_netname, lora_ifac_passphrase, 8", block)


if __name__ == "__main__":
    unittest.main()
