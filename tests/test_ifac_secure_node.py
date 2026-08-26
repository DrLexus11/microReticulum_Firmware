"""Regression tests for TCP/UDP IFAC and the secure-node host workflow."""

import importlib.util
import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

try:
    import serial  # noqa: F401
    from RNS.vendor import umsgpack  # noqa: F401
    HAVE_TOOL_DEPS = True
except Exception:
    HAVE_TOOL_DEPS = False


def load_tool():
    path = os.path.join(ROOT, "tools", "ifac", "provision.py")
    spec = importlib.util.spec_from_file_location("ifac_provision", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FirmwareContractTests(unittest.TestCase):
    def test_udp_capacity_covers_maximum_ifac_without_changing_radio_mtu(self):
        with open(os.path.join(ROOT, "UDPInterface.h"), encoding="utf-8") as handle:
            udp_header = handle.read()
        with open(os.path.join(ROOT, "Config.h"), encoding="utf-8") as handle:
            config = handle.read()
        with open(os.path.join(ROOT, "Remote.h"), encoding="utf-8") as handle:
            remote = handle.read()

        capacity = int(re.search(r"#define\s+UDP_RX_CAPACITY\s+(\d+)", udp_header).group(1))
        radio_mtu = int(re.search(r"#define\s+MTU\s+(\d+)", config).group(1))
        self.assertEqual(capacity, 500 + 64)
        self.assertEqual(radio_mtu, 508)
        self.assertIn("packet_len > UDP_RX_CAPACITY", remote)
        self.assertIn("udp_buffer.writable(UDP_RX_CAPACITY)", remote)
        self.assertNotIn("udp_buffer.writable(MTU)", remote)

    def test_access_control_namespace_ids_are_permanent_and_distinct(self):
        with open(os.path.join(ROOT, "Provisioning.h"), encoding="utf-8") as handle:
            header = handle.read()
        found = dict(re.findall(
            r"#define\s+(PROV_NS_(?:IFAC_LORA|IFAC_TCP|IFAC_UDP|SECURE_NODE))\s+(\d+)",
            header,
        ))
        self.assertEqual(found, {
            "PROV_NS_IFAC_LORA": "109",
            "PROV_NS_IFAC_TCP": "110",
            "PROV_NS_IFAC_UDP": "111",
            "PROV_NS_SECURE_NODE": "112",
        })

    def test_wireless_kiss_starts_fail_closed(self):
        with open(os.path.join(ROOT, "Config.h"), encoding="utf-8") as handle:
            config = handle.read()
        with open(os.path.join(ROOT, "Bluetooth.h"), encoding="utf-8") as handle:
            bluetooth = handle.read()
        with open(os.path.join(ROOT, "Remote.h"), encoding="utf-8") as handle:
            remote = handle.read()
        with open(os.path.join(ROOT, "BLESerial.cpp"), encoding="utf-8") as handle:
            ble_serial = handle.read()
        self.assertIn("#if defined(HAS_PROVISIONING)", config)
        self.assertIn("bool wireless_kiss_policy_ready = false;", config)
        self.assertIn("bool wireless_kiss_allowed = false;", config)
        self.assertIn("!wireless_kiss_policy_ready || !wireless_kiss_allowed", bluetooth)
        self.assertIn("wireless_kiss_policy_ready && wireless_kiss_allowed", remote)
        self.assertIn("wr_device_ip != kiss_bound_ip", remote)
        self.assertIn("KISS TCP bound on %s:7633", remote)
        self.assertIn("wireless_kiss_policy_ready && wireless_kiss_allowed", ble_serial)


@unittest.skipUnless(HAVE_TOOL_DEPS, "IFAC tool dependencies unavailable")
class ProvisioningToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = load_tool()

    def test_administrator_must_be_exactly_sixteen_bytes(self):
        self.assertEqual(self.tool.parse_administrator("ab" * 16), bytes.fromhex("ab" * 16))
        with self.assertRaisesRegex(RuntimeError, "16-byte"):
            self.tool.parse_administrator("ab" * 15)
        with self.assertRaisesRegex(RuntimeError, "hexadecimal"):
            self.tool.parse_administrator("not-hex")

    def test_secure_operation_is_one_staged_transition_and_hides_secrets(self):
        client = FakeClient(self.tool)
        networks = {name: "%s-net" % name for name in self.tool.IFAC_NAMESPACES}
        passphrases = {name: "%s-secret" % name for name in self.tool.IFAC_NAMESPACES}
        administrator = bytes.fromhex("41" * 16)

        result = self.tool.provision_secure(client, networks, passphrases, administrator)

        self.assertTrue(result[2])
        set_requests = [entry for entry in client.calls if entry[0] == self.tool.OP_SET_STATE]
        self.assertEqual(len(set_requests), 1)
        commit_requests = [entry for entry in client.calls if entry[0] == self.tool.OP_COMMIT]
        self.assertEqual(commit_requests[0][1][1][0], self.tool.NS_SECURE_NODE)
        changes = set_requests[0][1][3]
        self.assertTrue(changes[self.tool.NS_SECURE_NODE][self.tool.FIELD_SECURE_NODE_ENABLED])
        self.assertEqual(
            changes[self.tool.NS_GENERAL][self.tool.FIELD_REMOTE_MANAGEMENT_ALLOWED],
            [administrator],
        )
        for interface, (namespace_id, _) in self.tool.IFAC_NAMESPACES.items():
            self.assertEqual(changes[namespace_id][self.tool.FIELD_PASSPHRASE],
                             passphrases[interface])
            self.assertNotIn(self.tool.FIELD_PASSPHRASE, client.state[namespace_id])

    def test_open_operation_commits_secure_flag_last(self):
        client = FakeClient(self.tool)

        result = self.tool.provision_open(client, clear=True)

        self.assertTrue(result[2])
        commit_requests = [entry for entry in client.calls if entry[0] == self.tool.OP_COMMIT]
        self.assertEqual(commit_requests[0][1][1][-1], self.tool.NS_SECURE_NODE)


class FakeClient:
    """Minimal Provisioning wire model used to inspect the tool's transaction."""

    def __init__(self, tool):
        self.tool = tool
        self.calls = []
        self.pending = {}
        self.state = {
            tool.NS_GENERAL: {
                tool.FIELD_REMOTE_MANAGEMENT_ENABLED: True,
                tool.FIELD_REMOTE_MANAGEMENT_ALLOWED: [],
            },
            tool.NS_SECURE_NODE: {tool.FIELD_SECURE_NODE_ENABLED: False},
        }
        for namespace_id, _ in tool.IFAC_NAMESPACES.values():
            self.state[namespace_id] = {
                tool.FIELD_ENABLED: False,
                tool.FIELD_NETNAME: "",
            }

    def _schema(self, namespace_id):
        if namespace_id == self.tool.NS_GENERAL:
            fields = [
                {1: self.tool.FIELD_REMOTE_MANAGEMENT_ENABLED, 4: 0},
                {1: self.tool.FIELD_REMOTE_MANAGEMENT_ALLOWED, 4: 0},
            ]
        elif namespace_id == self.tool.NS_SECURE_NODE:
            fields = [{1: self.tool.FIELD_SECURE_NODE_ENABLED, 4: 0}]
        else:
            fields = [
                {1: self.tool.FIELD_ENABLED, 4: 0},
                {1: self.tool.FIELD_NETNAME, 4: 0},
                {1: self.tool.FIELD_PASSPHRASE, 4: self.tool.FF_SECRET},
            ]
        return [namespace_id, "namespace-%d" % namespace_id, 0, fields]

    def request(self, operation, payload=None):
        self.calls.append((operation, payload))
        if operation == self.tool.OP_GET_SCHEMA:
            return [self._schema(namespace_id) for namespace_id in payload[1]]
        if operation == self.tool.OP_SET_STATE:
            self.pending = payload[3]
            visible = {}
            for namespace_id, fields in self.pending.items():
                visible[namespace_id] = {
                    field: value for field, value in fields.items()
                    if not (namespace_id in (
                        self.tool.NS_IFAC_LORA,
                        self.tool.NS_IFAC_TCP,
                        self.tool.NS_IFAC_UDP,
                    ) and field == self.tool.FIELD_PASSPHRASE)
                }
            return {3: [], 5: visible}
        if operation == self.tool.OP_COMMIT:
            for namespace_id, fields in self.pending.items():
                for field, value in fields.items():
                    if field != self.tool.FIELD_PASSPHRASE:
                        self.state.setdefault(namespace_id, {})[field] = value
            self.pending = {}
            return {2: True}
        if operation == self.tool.OP_GET_STATE:
            return {1: {namespace_id: self.state[namespace_id] for namespace_id in payload[1]}}
        if operation == self.tool.OP_DISCARD:
            self.pending = {}
            return {}
        raise AssertionError("unexpected operation %r" % operation)


if __name__ == "__main__":
    unittest.main()
