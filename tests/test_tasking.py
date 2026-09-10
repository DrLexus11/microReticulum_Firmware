"""Task authorization, signed wire interoperability, and durable status semantics."""

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import task_codec as codec
import tak_tasking as gateway

try:
    import RNS
except ImportError:
    RNS = None


@unittest.skipUnless(RNS, "requires RNS virtualenv")
class TaskTests(unittest.TestCase):
    def setUp(self):
        self.authority = RNS.Identity.from_bytes(bytes(range(64)))
        self.phone = RNS.Identity.from_bytes(bytes(range(64, 128)))
        self.now = 1788700000
        self.task = codec.Message(codec.GOTO, self.authority.hash,
                                  gateway.destination_hash(self.phone), bytes(range(16)),
                                  self.now, self.now + 900, 411234567, 291234567, "Go to café")
        self.wire = codec.encode(self.task, self.authority)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = os.path.join(self.directory.name, "tasks.db")

    def verify(self, wire=None, **kw):
        return codec.verify(self.wire if wire is None else wire, kw.get("identity", self.authority),
                            kw.get("recipient", self.task.recipient), kw.get("now", self.now))

    def test_golden_vector_shared_with_kotlin(self):
        fixture = json.loads((Path(__file__).parent / "fixtures/task_v1.json").read_text())
        self.assertEqual(self.wire.hex(), fixture["task"])
        self.assertEqual(self.verify().text, "Go to café")

    def test_every_byte_is_authenticated(self):
        for index in range(len(self.wire)):
            tampered = bytearray(self.wire)
            tampered[index] ^= 1
            with self.assertRaises(ValueError, msg=str(index)):
                self.verify(bytes(tampered))

    def test_wrong_recipient_and_authority(self):
        with self.assertRaises(ValueError):
            self.verify(recipient=bytes(16))
        with self.assertRaises(ValueError):
            self.verify(identity=self.phone)

    def test_expiry_future_and_bad_lengths(self):
        for now in (self.now - 31, self.task.expires):
            with self.assertRaises(ValueError):
                self.verify(now=now)
        for wire in (self.wire[:-1], self.wire + b"x", bytes(10000)):
            with self.assertRaises(ValueError):
                self.verify(wire)

    def test_signed_malformed_body_rejected(self):
        payload = bytearray(self.wire[:-64])
        for index, value in ((0, 2), (1, 9), (66, 64), (67, 0xff)):
            malformed = payload.copy()
            malformed[index] = value
            with self.assertRaises(ValueError):
                self.verify(bytes(malformed) + self.authority.sign(codec.DOMAIN + malformed))

    def test_text_bounds_are_utf8_bytes(self):
        from dataclasses import replace
        self.assertEqual(len(codec.encode(replace(self.task, text="é" * 32), self.authority)), codec.MAX_PACKET)
        for text in ("é" * 33, "", "line\nbreak", "\x7f"):
            with self.assertRaises(ValueError):
                codec.encode(replace(self.task, text=text), self.authority)

    def test_coordinates_and_lifetime(self):
        from dataclasses import replace
        for change in ({"lat_e7": 900000001}, {"lon_e7": -1800000001},
                       {"expires": self.now}, {"expires": self.now + 3601}):
            with self.assertRaises(ValueError):
                codec.encode(replace(self.task, **change), self.authority)

    def test_firehose_never_creates_signed_tasks(self):
        envelope = json.dumps({"uid": "claimed-command", "cot":
            '<event uid="task" type="t-x-test"><point lat="41.2" lon="29.1"/>'
            '<detail><remarks>Move here</remarks><marti><dest uid="someone"/></marti></detail></event>'}).encode()
        with gateway.connect(self.path) as db:
            key = gateway.stage(db, envelope, self.now)
            self.assertIsNotNone(key)
            self.assertEqual(db.execute("SELECT count(*) FROM tasks").fetchone()[0], 0)
            self.assertEqual(gateway.stage(db, envelope, self.now), key)
            self.assertEqual(db.execute("SELECT count(*) FROM candidates").fetchone()[0], 1)

    def test_atak_marker_stages_once_however_often_it_is_rebroadcast(self):
        """Real ATAK traffic, captured 2026-09-10 from ATAK-CIV 5.6.0.12.

        Dropping a marker emits an atom, not a `t-` tasking type, and with
        auto-send on ATAK repeats it about every ten seconds with fresh
        timestamps. Keying candidates on the envelope made a new one per repeat
        and filled the inbox from a single stationary marker.
        """
        def marker(when, lat="40.9545566", lon="29.0945023"):
            return json.dumps({"uid": "ANDROID-7819dadfcf858641", "cot":
                '<event access="Undefined" how="h-g-i-g-o" type="a-h-G" version="2.0"'
                ' uid="66d6de40-bd62-4a5e-92ef-d4ee14b0194a"'
                ' time="%s" start="%s" stale="%s">'
                '<point ce="9999999.0" hae="48.019" lat="%s" le="9999999.0" lon="%s"/>'
                '<detail><status readiness="true"/><contact callsign="R.10.144053"/>'
                '<creator callsign="LEXUS" type="a-f-G-U-C" uid="ANDROID-7819dadfcf858641"/>'
                '<remarks/></detail></event>' % (when, when, when, lat, lon)}).encode()

        with gateway.connect(self.path) as db:
            key = gateway.stage(db, marker("2026-09-10T11:41:57.829Z"), self.now)
            self.assertIsNotNone(key, "an ATAK marker must be offered as a candidate")

            # Five auto-send repeats, each with different timestamps.
            for second in (7, 17, 27, 34, 44):
                repeat = marker("2026-09-10T11:42:%02d.000Z" % second)
                self.assertEqual(gateway.stage(db, repeat, self.now + second), key)
            self.assertEqual(db.execute("SELECT count(*) FROM candidates").fetchone()[0], 1)

            # Moving it is a different proposition and earns its own candidate.
            moved = gateway.stage(db, marker("2026-09-10T11:43:00.000Z", lat="40.9600000"), self.now)
            self.assertNotEqual(moved, key)
            self.assertEqual(db.execute("SELECT count(*) FROM candidates").fetchone()[0], 2)

            # A PLI is the same EUD saying where it is, not a place to send
            # anyone, and it moves constantly. Same envelope UID as event UID.
            pli = json.dumps({"uid": "ANDROID-7819dadfcf858641", "cot":
                '<event type="a-f-G-U-C" version="2.0" uid="ANDROID-7819dadfcf858641"'
                ' time="2026-09-10T11:42:03Z" start="2026-09-10T11:42:03Z"'
                ' stale="2026-09-10T11:42:51Z">'
                '<point ce="44.0" hae="33.593" lat="40.954935" le="9999999.0" lon="29.093375"/>'
                '<detail><contact callsign="LEXUS"/></detail></event>'}).encode()
            self.assertIsNone(gateway.stage(db, pli, self.now),
                              "a self-report must never become a candidate")
            self.assertEqual(db.execute("SELECT count(*) FROM candidates").fetchone()[0], 2)

            # Staging still never signs anything, whatever the type.
            self.assertEqual(db.execute("SELECT count(*) FROM tasks").fetchone()[0], 0)
            row = db.execute("SELECT cot_type, target_hint FROM candidates WHERE id=?", (key,)).fetchone()
            self.assertEqual(row["cot_type"], "a-h-G")
            self.assertEqual(json.loads(row["target_hint"])["claimed_origin"], "LEXUS")

    def test_rejects_xml_entities_and_invalid_points(self):
        for xml in ('<!DOCTYPE event [<!ENTITY x "boom">]><event/>',
                    '<event type="t-test"><point lat="nan" lon="0"/></event>'):
            with gateway.connect(self.path) as db, self.assertRaises(ValueError):
                gateway.stage(db, json.dumps({"cot": xml}).encode(), self.now)

    def make_task(self):
        with gateway.connect(self.path) as db:
            with db:
                db.execute("INSERT INTO peers VALUES(?,?)", ("phone", self.phone.get_public_key()))
            return gateway.enqueue(db, self.authority, "phone", 1, 2, "Go here", 900, self.now)

    def ack(self, task_id, status):
        return codec.encode(codec.Message(codec.STATUS, self.phone.hash,
                            gateway.destination_hash(self.authority), bytes.fromhex(task_id),
                            self.now + 10, self.now + 900, status=status), self.phone)

    def test_ack_persists_and_cannot_downgrade_decision(self):
        task_id = self.make_task()
        for status in (codec.ACCEPTED, codec.RECEIVED, codec.DECLINED):
            with gateway.connect(self.path) as db:
                _, result = gateway.accept_status(db, self.ack(task_id, status),
                                                   gateway.destination_hash(self.authority), self.now + 10)
                self.assertEqual(result, "accepted")

    def test_unknown_and_forged_ack_rejected(self):
        task_id = self.make_task()
        ack = self.ack(task_id, codec.RECEIVED)
        for wire in (self.ack("ab" * 16, codec.ACCEPTED), ack[:-1] + bytes([ack[-1] ^ 1])):
            with gateway.connect(self.path) as db, self.assertRaises(ValueError):
                gateway.accept_status(db, wire, gateway.destination_hash(self.authority), self.now + 10)

    def test_candidate_cannot_be_approved_twice(self):
        self.make_task()
        with gateway.connect(self.path) as db:
            with self.assertRaises(ValueError):
                gateway.enqueue(db, self.authority, "phone", 1, 2, "Go here", 900, self.now, "missing")
            self.assertEqual(db.execute("SELECT count(*) FROM tasks").fetchone()[0], 1)

    def test_approved_import_does_not_reappear_on_broker_replay(self):
        self.make_task()
        envelope = json.dumps({"cot": '<event type="t-test"><point lat="1" lon="2"/></event>'}).encode()
        with gateway.connect(self.path) as db:
            key = gateway.stage(db, envelope, self.now)
            gateway.enqueue(db, self.authority, "phone", 1, 2, "Go here", 900, self.now, key)
        with gateway.connect(self.path) as db:
            self.assertIsNone(gateway.stage(db, envelope, self.now + 10))
            self.assertEqual(db.execute("SELECT count(*) FROM candidates").fetchone()[0], 0)

    def test_downlink_retries_until_expiry_not_for_a_fixed_count(self):
        """A responder who is unreachable for a while must still get the task.

        The selection predicate the serve loop uses is the whole retry policy.
        With a three-attempt cap it stopped after roughly two minutes, so a
        recipient in a dead spot missed a task that stayed valid for another
        thirteen -- and nothing redelivered when they came back.
        """
        task_id = self.make_task()
        select = ("SELECT id FROM tasks WHERE expires>? AND state='queued' "
                  "AND last_attempt<=?")
        with gateway.connect(self.path) as db:
            # Ten attempts already spent, well past any fixed cap.
            with db:
                db.execute("UPDATE tasks SET attempts=10, last_attempt=? WHERE id=?",
                           (self.now, task_id))
            later = self.now + gateway.RETRY_SECONDS
            due = db.execute(select, (later, later - gateway.RETRY_SECONDS)).fetchall()
            self.assertEqual([row["id"] for row in due], [task_id])

            # Spacing is still honoured between tries.
            too_soon = self.now + gateway.RETRY_SECONDS - 1
            self.assertEqual(
                db.execute(select, (too_soon, too_soon - gateway.RETRY_SECONDS)).fetchall(), [])

            # Expiry is the bound that stops it, and a decision stops it sooner.
            expired = self.task.expires + 1
            self.assertEqual(
                db.execute(select, (expired, expired - gateway.RETRY_SECONDS)).fetchall(), [])
            with db:
                db.execute("UPDATE tasks SET state='accepted' WHERE id=?", (task_id,))
            self.assertEqual(
                db.execute(select, (later, later - gateway.RETRY_SECONDS)).fetchall(), [])

    def test_database_is_private(self):
        self.make_task()
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o600)


if __name__ == "__main__":
    unittest.main()
