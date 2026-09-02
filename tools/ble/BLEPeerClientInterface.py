# A Reticulum interface that joins a mesh through a node's BLE peer service.
#
# This is the deck-side counterpart of BLEPeerInterface.h, and it exists so the
# Columba path can be exercised without Columba. The phone is the real client,
# but it is also the one part of the chain that cannot be scripted, so every
# failure it reports is ambiguous: firmware, protocol or app. Standing in for it
# with a known-correct client removes two of those three.
#
# Protocol is Reticulum BLE peer v2.2, transcribed from BLEPeerProtocol.h:
#
#   service   37145b00-442d-4a94-917f-8f42c5da28e3
#   rx  ...e5 we write fragments here
#   tx  ...e4 we subscribe; the node notifies fragments to us
#   id  ...e6 read, 16 bytes, the node's transport identity
#
#   fragment  !BHH  type, seq, total  then payload
#   handshake a bare 16-byte write, no header, ours to the node
#   keepalive a bare 0x00 byte, both directions, every 15 s
#
# Install as an RNS external interface: drop this file in <configdir>/interfaces/
# and set `type = BLEPeerClientInterface`.
#
# Threading: bleak is asyncio and RNS is not. The event loop owns the link and
# runs in its own daemon thread; process_outgoing() is called from RNS threads
# and only ever hands work to that loop through run_coroutine_threadsafe.

import asyncio
import struct
import threading
import time

import RNS

from bleak import BleakClient, BleakScanner

SERVICE_UUID  = "37145b00-442d-4a94-917f-8f42c5da28e3"
TX_UUID       = "37145b00-442d-4a94-917f-8f42c5da28e4"
RX_UUID       = "37145b00-442d-4a94-917f-8f42c5da28e5"
IDENTITY_UUID = "37145b00-442d-4a94-917f-8f42c5da28e6"

HEADER_SIZE    = 5
ATT_HEADER     = 3
MIN_MTU        = 23
MAX_ATTR       = 512
IDENTITY_SIZE  = 16
KEEPALIVE_BYTE = 0x00
KEEPALIVE_MS   = 15000

TYPE_LONE, TYPE_START, TYPE_CONTINUE, TYPE_END = 0x00, 0x01, 0x02, 0x03


def usable_value_length(att_mtu):
    """Byte-for-byte the same arithmetic as ble_peer_usable_value_length()."""
    usable = int(att_mtu) - ATT_HEADER
    floor = MIN_MTU - ATT_HEADER
    if usable < floor:
        return floor
    if usable > MAX_ATTR:
        return MAX_ATTR
    return usable


def payload_size(usable):
    return usable - HEADER_SIZE if usable > HEADER_SIZE else 1


class BLEPeerClientInterface(RNS.Interfaces.Interface.Interface):
    BITRATE_GUESS = 100000
    DEFAULT_IFAC_SIZE = 16

    def __init__(self, owner, configuration):
        super().__init__()
        c = RNS.Interfaces.Interface.Interface.get_config_obj(configuration)

        self.owner = owner
        self.name = c["name"]
        # Optional: pin one node by BLE address. Left unset we take the first
        # advertiser of the service UUID, which is right in a one-node lab and
        # wrong the moment a second RAD is powered on.
        self.target_address = c["target_address"] if "target_address" in c else None
        self.scan_timeout = float(c["scan_timeout"]) if "scan_timeout" in c else 20.0

        self.IN = True
        self.OUT = True
        self.HW_MTU = 500
        self.bitrate = BLEPeerClientInterface.BITRATE_GUESS
        self.online = False

        self.peer_identity = None
        self.negotiated_mtu = MIN_MTU
        self.frames_in = 0
        self.frames_out = 0
        self.packets_in = 0
        self.packets_out = 0
        self.reassembly_drops = 0

        self._client = None
        self._inbound = bytearray()
        self._expected = 0
        self._handshake_sent = False
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()

    # --- event loop ---------------------------------------------------------

    def _run_loop(self):
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._maintain())

    async def _maintain(self):
        while True:
            try:
                await self._session()
            except Exception as e:
                RNS.log(f"{self} session ended: {e}", RNS.LOG_WARNING)
            self.online = False
            self._handshake_sent = False
            self._inbound.clear()
            self._expected = 0
            await asyncio.sleep(5)

    async def _session(self):
        if self.target_address:
            device = await BleakScanner.find_device_by_address(
                self.target_address, timeout=self.scan_timeout)
        else:
            device = await BleakScanner.find_device_by_filter(
                lambda d, a: SERVICE_UUID in [u.lower() for u in (a.service_uuids or [])],
                timeout=self.scan_timeout)
        if device is None:
            RNS.log(f"{self} no peer service advertising", RNS.LOG_DEBUG)
            return

        async with BleakClient(device, timeout=25) as client:
            self._client = client
            # BlueZ only exchanges MTU lazily. Without this the link stays at
            # the 23-byte default and every RNS packet is shredded into 15-byte
            # fragments, which works but is slow enough to look like a fault.
            try:
                await client._acquire_mtu()
            except Exception:
                pass
            self.negotiated_mtu = client.mtu_size or MIN_MTU

            identity = await client.read_gatt_char(IDENTITY_UUID)
            self.peer_identity = bytes(identity)
            RNS.log(f"{self} peer identity {self.peer_identity.hex()} "
                    f"mtu={self.negotiated_mtu}", RNS.LOG_NOTICE)

            await client.start_notify(TX_UUID, self._on_notify)
            self.online = True

            last_beat = 0
            while client.is_connected:
                # The handshake needs our transport identity, which does not
                # exist until Transport has started. Interfaces are constructed
                # before that, so send it on the first pass where it is there
                # rather than at connect time.
                if not self._handshake_sent:
                    await self._send_handshake(client)
                now = time.time() * 1000
                if now - last_beat >= KEEPALIVE_MS:
                    last_beat = now
                    await self._write(client, bytes([KEEPALIVE_BYTE]))
                await asyncio.sleep(0.2)

        self._client = None

    async def _send_handshake(self, client):
        try:
            identity = RNS.Transport.identity
        except Exception:
            identity = None
        if identity is None:
            return
        await self._write(client, identity.hash)
        self._handshake_sent = True
        RNS.log(f"{self} sent identity {identity.hash.hex()}", RNS.LOG_NOTICE)

    async def _write(self, client, value):
        # write-with-response. The node accepts both, and on BlueZ the
        # acknowledged form is what keeps a long fragment train in order.
        await client.write_gatt_char(RX_UUID, value, response=True)
        self.frames_out += 1

    # --- inbound ------------------------------------------------------------

    def _on_notify(self, _characteristic, data):
        self.frames_in += 1
        frame = bytes(data)
        if len(frame) == 1 and frame[0] == KEEPALIVE_BYTE:
            return
        if len(frame) < HEADER_SIZE + 1:
            self.reassembly_drops += 1
            return

        ftype, seq, total = struct.unpack("!BHH", frame[:HEADER_SIZE])
        payload = frame[HEADER_SIZE:]

        if ftype == TYPE_LONE or (ftype == TYPE_START and total == 1):
            self._deliver(payload)
            return
        if ftype == TYPE_START:
            self._inbound = bytearray(payload)
            self._expected = total
            return
        if ftype in (TYPE_CONTINUE, TYPE_END):
            if self._expected == 0:
                # A fragment train we joined halfway. Dropping it is correct;
                # counting it is what tells us that is happening.
                self.reassembly_drops += 1
                return
            self._inbound.extend(payload)
            if ftype == TYPE_END or seq == self._expected - 1:
                self._deliver(bytes(self._inbound))
                self._inbound = bytearray()
                self._expected = 0
            return
        self.reassembly_drops += 1

    def _deliver(self, packet):
        self.packets_in += 1
        self.process_incoming(packet)

    def process_incoming(self, data):
        self.rxb += len(data)
        self.owner.inbound(data, self)

    # --- outbound -----------------------------------------------------------

    def process_outgoing(self, data):
        if not self.online or self._client is None:
            return
        try:
            future = asyncio.run_coroutine_threadsafe(
                self._send_packet(bytes(data)), self._loop)
            future.result(timeout=30)
            self.txb += len(data)
            self.packets_out += 1
        except Exception as e:
            RNS.log(f"Could not transmit on {self}: {e}", RNS.LOG_ERROR)

    async def _send_packet(self, data):
        client = self._client
        if client is None or not client.is_connected:
            return
        usable = usable_value_length(self.negotiated_mtu)
        chunk = payload_size(usable)
        total = max(1, (len(data) + chunk - 1) // chunk)

        for i in range(total):
            if i == 0:
                # START even when it is also the last fragment. Columba's
                # reassembler drops unknown types and never emits LONE, so a
                # LONE here makes the entire outbound direction vanish while
                # inbound keeps working. See BLEPeerProtocol.h.
                ftype = TYPE_START
            elif i == total - 1:
                ftype = TYPE_END
            else:
                ftype = TYPE_CONTINUE
            body = data[i * chunk:(i + 1) * chunk]
            await self._write(client, struct.pack("!BHH", ftype, i, total) + body)

    def detach(self):
        self.online = False

    def __str__(self):
        peer = self.peer_identity.hex() if self.peer_identity else "unconnected"
        return f"BLEPeerClientInterface[{self.name}/{peer}]"


interface_class = BLEPeerClientInterface
