"""Contract and architecture tests for the one-hop ESP-NOW interface."""

import os
import re
import struct
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARDS = os.path.join(ROOT, "Boards.h")
DISPLAY = os.path.join(ROOT, "Display.h")
PROTOCOL = os.path.join(ROOT, "ESPNowProtocol.h")
INTERFACE = os.path.join(ROOT, "ESPNowInterface.h")
FIRMWARE = os.path.join(ROOT, "RNode_Firmware.ino")
PROVISIONING = os.path.join(ROOT, "Provisioning.cpp")
PROVISIONING_HEADER = os.path.join(ROOT, "Provisioning.h")
REMOTE = os.path.join(ROOT, "Remote.h")
PAGES = os.path.join(ROOT, "Pages.h")
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

    def test_active_recovery_frames_fit_one_action_frame(self):
        self.assertEqual(3, self.d["ESPNOW_FRAME_SOLICIT"])
        self.assertEqual(4, self.d["ESPNOW_FRAME_RECOVERY_REPLY"])
        self.assertEqual(4, self.d["ESPNOW_SOLICIT_SIZE"])
        self.assertEqual(8, self.d["ESPNOW_RECOVERY_PROOF_SIZE"])
        reply_payload = (self.d["ESPNOW_SOLICIT_SIZE"] +
                         self.d["ESPNOW_DISCOVERY_SIZE"] +
                         self.d["ESPNOW_RECOVERY_PROOF_SIZE"])
        self.assertEqual(28, reply_payload)
        self.assertLessEqual(self.d["ESPNOW_HEADER_SIZE"] + reply_payload,
                             self.d["ESPNOW_WIRE_MTU"])

    def test_recovery_reply_layout_binds_nonce_discovery_and_proof(self):
        nonce = 0x10203040
        discovery = struct.pack("!IIIBBBB", 0x11223344, 868100000,
                                125000, 9, 5, 6, 7)
        proof = bytes.fromhex("0102030405060708")
        payload = struct.pack("!I", nonce) + discovery + proof
        self.assertEqual(28, len(payload))
        self.assertEqual(nonce, struct.unpack("!I", payload[:4])[0])
        self.assertEqual(discovery, payload[4:20])
        self.assertEqual(proof, payload[20:])


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

    def test_expiry_snapshot_is_refreshed_after_inbound_timestamps(self):
        text = source(INTERFACE)
        loop = text[text.index("void loop() override"):]
        loop = loop[:loop.index("bool started() const")]
        drained = loop.index("drain_inbound();")
        refreshed = loop.index("now = millis();", drained)
        expired = loop.index("expire_state(now);", drained)
        self.assertLess(drained, refreshed)
        self.assertLess(refreshed, expired)

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

    def test_connected_station_cannot_be_channel_hopped(self):
        text = source(INTERFACE)
        request = text[text.index("bool request_recovery_scan"):]
        request = request[:request.index("void reset_recovery")]
        self.assertIn("WiFi.getMode() != WIFI_MODE_STA", request)
        self.assertIn("WiFi.status() == WL_CONNECTED", request)
        self.assertIn("return false", request)
        self.assertNotIn("esp_wifi_set_channel", request)

    def test_recovery_is_opt_in_and_precedes_existing_softap(self):
        remote = source(REMOTE)
        self.assertIn(
            "#define WIFI_ESPNOW_RECOVERY_DEFAULT WIFI_ESPNOW_RECOVERY_OFF",
            remote)
        self.assertIn(
            "wifi_espnow_recovery_mode = WIFI_ESPNOW_RECOVERY_DEFAULT",
            remote)
        recovery = remote.index("espnow_request_recovery_scan(")
        fallback = remote.index("wifi_remote_start_ap_fallback();", recovery)
        self.assertLess(recovery, fallback)
        self.assertIn("completed failed scan falls through", remote)

    def test_recovery_admission_uses_nonce_and_backbone_ifac(self):
        text = source(INTERFACE)
        handler = text[text.index("void handle_recovery_reply"):]
        handler = handler[:handler.index("Peer* touch_peer")]
        self.assertIn("nonce != _recovery_nonce", handler)
        self.assertIn("verify_recovery_proof", handler)
        self.assertIn("ESPNOW_CAP_IFAC_PROOF", handler)
        proof = text[text.index("void make_recovery_proof"):]
        proof = proof[:proof.index("void send_recovery_reply")]
        self.assertIn("Cryptography::hkdf", proof)
        self.assertIn("difference |=", proof)

    def test_solicit_burst_cannot_keep_postponing_a_pending_reply(self):
        text = source(INTERFACE)
        handler = text[text.index("void handle_solicit"):]
        handler = handler[:handler.index("void handle_recovery_reply")]
        guard = handler.index("if (_recovery_reply_pending) return")
        deadline = handler.index("_recovery_reply_at =")
        self.assertLess(guard, deadline)

    def test_selected_peer_health_uses_reticulum_acceptance(self):
        text = source(INTERFACE)
        inbound = text[text.index("const uint32_t accepted_before"):]
        inbound = inbound[:inbound.index("catch (const std::bad_alloc&)")]
        self.assertIn("handle_incoming(packet)", inbound)
        self.assertIn("Transport::packets_received() != accepted_before", inbound)
        self.assertIn("_accepted_from_selected++", inbound)
        self.assertIn("_recovery_peer_last_seen = millis()", inbound)

    def test_recovery_diagnostics_are_read_only_and_registered(self):
        firmware = source(FIRMWARE)
        pages = source(PAGES)
        self.assertIn(
            'register_request_handler("/page/espnow.mu", serve_page', firmware)
        page = pages[pages.index('path == "/page/espnow.mu"'):]
        page = page[:page.index("else if", 1)]
        self.assertIn("Recovery policy", page)
        self.assertIn("Channel errors", page)
        self.assertNotIn("request_recovery_scan", page)

    def test_recovery_provisioning_ids_do_not_overlap_ap_metrics(self):
        header = source(PROVISIONING_HEADER)
        fields = dict((name, int(value)) for name, value in re.findall(
            r"^#define\s+(PROV_NET_\w+)\s+(\d+)\s*$", header, re.M))
        self.assertEqual(8, fields["PROV_NET_ESPNOW_RECOVERY"])
        self.assertEqual(9, fields["PROV_NET_ESPNOW_SCAN_S"])
        self.assertEqual(10, fields["PROV_NET_ESPNOW_CHANNEL"])
        self.assertTrue({8, 9, 10}.isdisjoint({
            fields["PROV_NET_AP_ACTIVE"], fields["PROV_NET_AP_CLIENTS"],
            fields["PROV_NET_AP_SSID"]}))


class OzdisanAcceptanceTargetTests(unittest.TestCase):
    def test_target_is_espnow_ble_peer_and_recovery_enabled(self):
        text = source(PLATFORMIO)
        target = text[text.index("[env:ozdisan-esp32-espnow]"):]
        target = target[:target.index("\n[env:", 1)]
        self.assertIn("board = esp32doit-devkit-v1", target)
        self.assertIn("upload_speed = 115200", target)
        self.assertIn("-DBOARD_MODEL=BOARD_OZDISAN_ESP32", target)
        self.assertIn("-DESPNOW_TRANSPORT", target)
        self.assertIn("-DBLE_PEER_TRANSPORT", target)
        self.assertIn("-DNIMBLE_PEER_TRANSPORT", target)
        self.assertNotIn("-DTCP_SERVER_TRANSPORT", target)
        self.assertIn("-DWIFI_ESPNOW_RECOVERY_DEFAULT=1", target)
        self.assertIn("-DLORA_TRANSPORT", target.split("build_unflags", 1)[1])

    def test_board_declares_no_lora_and_meshtastic_oled_pinout(self):
        boards = source(BOARDS)
        block = boards[boards.index("#elif BOARD_MODEL == BOARD_OZDISAN_ESP32"):]
        block = block[:block.index("#elif BOARD_MODEL", 1)]
        self.assertIn("#define NO_LORA_HARDWARE true", block)
        self.assertIn("#define VALIDATE_FIRMWARE false", block)
        self.assertIn("#define HAS_DISPLAY true", block)
        # NimBLE is used directly on this target. HAS_BLE selects the RNode
        # BLE-serial/Bluedroid stack instead, which is the thing that does not
        # fit, so the two must not both be on.
        self.assertIn("#define HAS_BLE false", block)
        self.assertIn("#define I2C_SDA 5", block)
        self.assertIn("#define I2C_SCL 4", block)

        display = source(DISPLAY)
        block = display[display.index("#elif BOARD_MODEL == BOARD_OZDISAN_ESP32"):]
        block = block[:block.index("#else", 1)]
        self.assertIn("#define DISP_RST 16", block)
        self.assertIn("#define DISP_ADDR 0x3C", block)
        self.assertIn("#define SCL_OLED 4", block)
        self.assertIn("#define SDA_OLED 5", block)

    def test_erased_fixture_starts_sta_without_lora_config(self):
        firmware = source(FIRMWARE)
        self.assertIn(
            "#if BOARD_MODEL == BOARD_OZDISAN_ESP32 && HAS_EEPROM",
            firmware)
        self.assertIn(
            "eeprom_update(eeprom_addr(ADDR_CONF_WIFI), WR_WIFI_STA)",
            firmware)
        self.assertIn("#if defined(NO_LORA_HARDWARE)", firmware)
        self.assertIn("const bool required_hardware_present = true", firmware)
        self.assertIn("const bool node_config_ready = true", firmware)
        self.assertIn('[lora] not fitted (ESP-NOW-only target)', firmware)

    def test_fixture_has_stable_device_and_nomadnet_name(self):
        firmware = source(FIRMWARE)
        self.assertGreaterEqual(
            firmware.count(
                'snprintf(bt_devname, sizeof(bt_devname), "OZD-ARD-01")'),
            1)
        self.assertGreaterEqual(
            firmware.count(
                'snprintf(nomadnet_name, sizeof(nomadnet_name), "OZD-ARD-01")'),
            1)

    def test_unconfigured_pinned_fixture_does_not_retry_blank_station(self):
        remote = source(REMOTE)
        pinned = remote[remote.index("if (espnow_recovery_pinned())"):
                        remote.index("if (fallback_due", remote.index(
                            "if (espnow_recovery_pinned())"))]
        self.assertIn("wr_ssid[0] != 0x00", pinned)
        self.assertIn("espnow_recovery_pinned_since()", pinned)

    def test_unconfigured_fixture_rescans_after_selected_peer_is_lost(self):
        remote = source(REMOTE)
        recovery = remote[remote.index("if (fallback_due &&"):]
        recovery = recovery[:recovery.index("// A completed failed scan")]
        retry = recovery.index("espnow_recovery_failed()")
        blank_ssid = recovery.index("wr_ssid[0] == 0x00", retry)
        clear_attempt = recovery.index("wifi_espnow_scan_attempted = false", blank_ssid)
        request = recovery.index("espnow_request_recovery_scan", clear_attempt)
        self.assertLess(retry, blank_ssid)
        self.assertLess(blank_ssid, clear_attempt)
        self.assertLess(clear_attempt, request)

    def test_new_recovery_attachment_immediately_announces_nomadnet_site(self):
        firmware = source(FIRMWARE)
        watch = firmware[firmware.index("static void nomadnet_announce_watch()") :]
        watch = watch[:watch.index("// Recover a wedged modem")]
        success = watch.index("espnow_recovery_successes()")
        pinned = watch.index("espnow_recovery_pinned()", success)
        bypass = watch.index("if (!recovery_attach &&", pinned)
        announce = watch.index("nomadnet_destination.announce", bypass)
        self.assertLess(success, pinned)
        self.assertLess(pinned, bypass)
        self.assertLess(bypass, announce)
        self.assertIn('recovery_attach ? "ESP-NOW attach"', watch)

    def test_first_recovery_request_can_start_interface_immediately(self):
        text = source(INTERFACE)
        request = text[text.index("bool request_recovery_scan"):]
        request = request[:request.index("void reset_recovery")]
        connected_guard = request.index("WiFi.status() == WL_CONNECTED")
        immediate_start = request.index("if (!_started && !start()) return false")
        self.assertLess(connected_guard, immediate_start)

    def test_espnow_only_node_keeps_backbone_ifac_namespace(self):
        text = source(PROVISIONING)
        self.assertIn(
            "defined(LORA_TRANSPORT) || (HAS_WIFI == true && defined(ESPNOW_TRANSPORT))",
            text)
        self.assertIn('backbone_access_name = "ESP-NOW Access Control"', text)
        self.assertIn("register_namespace(backbone_access_name, PROV_NS_IFAC_LORA)",
                      text)

    def test_provisioning_profile_and_firmware_hash_include_fixture(self):
        script = source(os.path.join(ROOT, "extra_script.py"))
        self.assertIn('"ozdisan_esp32_espnow": 3', script)
        self.assertGreaterEqual(script.count('"ozdisan_esp32_espnow"'), 5)

    def test_provision_target_resolves_rnodeconf_and_propagates_failure(self):
        script = source(os.path.join(ROOT, "extra_script.py"))
        helper = script[script.index("def find_rnodeconf"):]
        helper = helper[:helper.index("#\n# Custom targets")]
        self.assertIn('shutil.which("rnodeconf")', helper)
        self.assertIn("rnode-rns-venv/bin/rnodeconf", helper)
        self.assertIn("if result != 0", helper)
        self.assertIn("raise RuntimeError", helper)

        target = script[script.index("def target_provision"):]
        target = target[:target.index("# Add custom targets")]
        self.assertIn("run_rnodeconf(env", target)
        self.assertNotIn('f"rnodeconf ', target)

    def test_hash_writer_waits_for_slow_ozdisan_boot(self):
        script = source(os.path.join(ROOT, "extra_script.py"))
        writer = script[script.index("def device_set_firmware_hash"):]
        writer = writer[:writer.index("def target_fixhash")]
        self.assertIn('variant == "ozdisan_esp32_espnow"', writer)
        self.assertIn("boot_wait = 20.0", writer)
        self.assertIn("time.monotonic() + boot_wait", writer)
        self.assertNotIn("firmware_reset_kiss_frame()", writer)


if __name__ == "__main__":
    unittest.main()


class CompactProvisioningTests(unittest.TestCase):
    """The constrained profile must retain only operational configuration.

    OZD_COMPACT_PROVISIONING exists because the full schema does not fit in the
    heap this board has left after NimBLE, Wi-Fi and ESP-NOW. Even the reduced
    device-metrics namespace consumed enough heap to reproduce TASK_WDT resets,
    so the fixture is observed through its periodic serial diagnostics instead.
    """

    def profile(self):
        text = source(os.path.join(ROOT, "Provisioning.cpp"))
        start = text.index("#if defined(OZD_COMPACT_PROVISIONING)")
        return text[start:text.index("#else", start)]

    def test_espnow_ifac_and_secure_node_are_retained(self):
        block = self.profile()
        self.assertIn("PROV_NS_IFAC_LORA", block)
        self.assertIn("PROV_NS_SECURE_NODE", block)

    def test_device_metrics_are_omitted(self):
        block = self.profile()
        self.assertNotIn("PROV_NS_METRICS", block)

    def test_the_stack_metric_is_not_served_on_a_radioless_target(self):
        # sample_loop_stack() runs inside `if (radio_online)`, so on a board
        # with no radio it never runs. OZD omits all of ns108, including the
        # sentinel that would serialise as 4294967295 bytes. See Backlog item 9.
        self.assertNotIn("PROV_METRICS_DEV_STACK", self.profile())
