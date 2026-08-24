#!/usr/bin/env python3
"""Deterministic multi-message and capacity test for an LXMF propagation node.

Each run creates isolated sender/recipient identities under the ignored
``state/`` directory. Messages are pushed while the recipient is offline, then
downloaded in repeated sync rounds until the node reports no more mail.

Sending more messages than the configured store capacity intentionally exercises
oldest-first eviction. That mode is destructive to pre-existing messages on the
node and therefore requires ``--confirm-eviction``.
"""

import argparse
import os
import shutil
import sys
import time
import uuid

import LXMF
import RNS
from RNS.vendor import umsgpack as msgpack


DEFAULT_TIMEOUT = 240
DEFAULT_STATE_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "state", "lxmf-stress")
)
STORE_CAPACITY_MESSAGES = 128


def wait_until(predicate, timeout, label):
    started = time.time()
    while time.time() - started < timeout:
        if predicate():
            return time.time() - started
        time.sleep(0.2)
    raise TimeoutError("timed out waiting for %s after %.1fs" % (label, timeout))


def ensure_path(destination_hash, timeout=30):
    if not RNS.Transport.has_path(destination_hash):
        RNS.Transport.request_path(destination_hash)
        wait_until(
            lambda: RNS.Transport.has_path(destination_hash), timeout, "propagation path"
        )
    identity = RNS.Identity.recall(destination_hash)
    if identity is None:
        raise RuntimeError("path exists but propagation identity was not recalled")
    return identity


def load_or_create_identity(path):
    if os.path.isfile(path):
        return RNS.Identity.from_file(path)
    identity = RNS.Identity()
    identity.to_file(path)
    return identity


def wait_for_delivery(router, message, timeout):
    state = {"result": None}
    message.delivery_callbacks = []
    message.register_delivery_callback(lambda _: state.update(result="DELIVERED"))
    message.register_failed_callback(lambda _: state.update(result="FAILED"))
    router.handle_outbound(message)
    elapsed = wait_until(lambda: state["result"] is not None, timeout, "message delivery")
    if state["result"] != "DELIVERED":
        raise RuntimeError("propagation node rejected message %s" % message.content_as_string())
    return elapsed


def sync_once(router, identity, received, timeout):
    before = len(received)
    router.acknowledge_sync_completion(reset_state=True)
    router.request_messages_from_propagation_node(identity)
    wait_until(
        lambda: router.propagation_transfer_state == LXMF.LXMRouter.PR_COMPLETE,
        timeout,
        "sync completion",
    )
    # The purge acknowledgement is fire-and-forget. Give it a moment to reach
    # the node before asking for the next list, otherwise the same messages can
    # legitimately appear in two consecutive rounds.
    time.sleep(1.0)
    return len(received) - before, router.propagation_transfer_last_result


def abandon_sync(router):
    """Tear down a stalled request before retrying it on a fresh link."""
    link = router.outbound_propagation_link
    if link is not None:
        try:
            link.teardown()
        except Exception:
            pass
    router.outbound_propagation_link = None
    router.acknowledge_sync_completion(reset_state=True)
    time.sleep(1.0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pn_hash", help="32-hex-character propagation destination")
    parser.add_argument("--messages", type=int, default=3, help="messages to enqueue")
    parser.add_argument(
        "--body-bytes", type=int, default=48, help="minimum UTF-8 body length per message"
    )
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    parser.add_argument(
        "--client-limit-kb",
        type=float,
        default=1000,
        help="LXMF delivery limit advertised by the recipient (default: stock client's 1000 KB)",
    )
    parser.add_argument(
        "--sync-retries",
        type=int,
        default=2,
        help="fresh-link retries after a stalled sync round",
    )
    parser.add_argument("--state-root", default=DEFAULT_STATE_ROOT)
    parser.add_argument("--keep-state", action="store_true")
    parser.add_argument(
        "--resume-run",
        metavar="RUN_ID",
        help="reuse a preserved run's recipient identity and skip the send phase",
    )
    parser.add_argument(
        "--rns-log-level",
        choices=("warning", "info", "debug"),
        default="warning",
        help="Reticulum diagnostic verbosity (default: warning)",
    )
    parser.add_argument(
        "--confirm-eviction",
        action="store_true",
        help="allow a capacity run that can evict pre-existing node messages",
    )
    args = parser.parse_args()

    if args.messages < 1:
        parser.error("--messages must be positive")
    if args.body_bytes < 1:
        parser.error("--body-bytes must be positive")
    if args.client_limit_kb <= 0:
        parser.error("--client-limit-kb must be positive")
    if args.sync_retries < 0:
        parser.error("--sync-retries cannot be negative")
    if args.messages > STORE_CAPACITY_MESSAGES and not args.confirm_eviction:
        parser.error(
            "more than %d messages tests eviction and may remove existing mail; "
            "pass --confirm-eviction" % STORE_CAPACITY_MESSAGES
        )

    try:
        pn_hash = bytes.fromhex(args.pn_hash)
    except ValueError:
        parser.error("pn_hash must be hexadecimal")
    if len(pn_hash) != RNS.Identity.TRUNCATED_HASHLENGTH // 8:
        parser.error("pn_hash must be 16 bytes / 32 hex characters")

    if args.resume_run:
        run_id = os.path.basename(args.resume_run.rstrip(os.sep))
        run_dir = os.path.join(args.state_root, run_id)
        if not os.path.isdir(run_dir):
            parser.error("preserved run does not exist: %s" % run_dir)
        for name in ("sender.id", "recipient.id"):
            if not os.path.isfile(os.path.join(run_dir, name)):
                parser.error("preserved run is missing %s" % name)
    else:
        run_id = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
        run_dir = os.path.join(args.state_root, run_id)
        os.makedirs(run_dir, exist_ok=False)

    RNS.loglevel = {
        "warning": RNS.LOG_WARNING,
        "info": RNS.LOG_INFO,
        "debug": RNS.LOG_DEBUG,
    }[args.rns_log_level]
    RNS.Reticulum()
    ensure_path(pn_hash)
    app_data = RNS.Identity.recall_app_data(pn_hash)
    if app_data is None or not LXMF.pn_announce_data_is_valid(app_data):
        raise RuntimeError("missing or invalid propagation announce data")
    config = msgpack.unpackb(app_data)
    print("run:", run_id)
    print("node:", RNS.prettyhexrep(pn_hash), "announce:", config)
    print("path:", RNS.Transport.hops_to(pn_hash), "hop(s)")

    sender_id = load_or_create_identity(os.path.join(run_dir, "sender.id"))
    recipient_id = load_or_create_identity(os.path.join(run_dir, "recipient.id"))

    prefix = "rad-stress:%s:" % run_id
    expected = []
    for index in range(args.messages):
        marker = "%04d" % index
        body = prefix + marker + ":"
        if len(body.encode("utf-8")) < args.body_bytes:
            body += "x" * (args.body_bytes - len(body.encode("utf-8")))
        expected.append(body)

    if args.resume_run:
        print("send phase: skipped (resuming preserved run)")
    else:
        sender = LXMF.LXMRouter(
            identity=sender_id, storagepath=os.path.join(run_dir, "sender")
        )
        sender.register_delivery_identity(sender_id, display_name="stress sender")
        sender.set_outbound_propagation_node(pn_hash)

        recipient_destination = RNS.Destination(
            recipient_id, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery"
        )
        sender_destination = RNS.Destination(
            sender_id, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery"
        )

        send_started = time.time()
        for index, body in enumerate(expected):
            message = LXMF.LXMessage(
                recipient_destination,
                sender_destination,
                body,
                "capacity test",
                desired_method=LXMF.LXMessage.PROPAGATED,
            )
            elapsed = wait_for_delivery(sender, message, args.timeout)
            representation = {1: "packet", 2: "resource"}.get(
                message.representation, str(message.representation)
            )
            print(
                "sent %d/%d %s in %.1fs packed=%s"
                % (
                    index + 1,
                    args.messages,
                    representation,
                    elapsed,
                    len(message.propagation_packed) if message.propagation_packed else "?",
                ),
                flush=True,
            )
        print("send phase: %.1fs" % (time.time() - send_started))

    recipient = LXMF.LXMRouter(
        identity=recipient_id,
        storagepath=os.path.join(run_dir, "recipient"),
        delivery_limit=args.client_limit_kb,
    )
    received_messages = []
    recipient.register_delivery_identity(recipient_id, display_name="stress recipient")
    recipient.register_delivery_callback(lambda message: received_messages.append(message))
    recipient.set_outbound_propagation_node(pn_hash)

    rounds = 0
    while True:
        rounds += 1
        for attempt in range(args.sync_retries + 1):
            try:
                added, node_result = sync_once(
                    recipient, recipient_id, received_messages, args.timeout
                )
                break
            except TimeoutError:
                if attempt >= args.sync_retries:
                    raise
                print(
                    "sync round %d stalled; retrying on a fresh link (%d/%d)"
                    % (rounds, attempt + 1, args.sync_retries),
                    flush=True,
                )
                abandon_sync(recipient)
        print(
            "sync round %d: node returned %s, delivered %d new (total %d)"
            % (rounds, node_result, added, len(received_messages)),
            flush=True,
        )
        if node_result == 0:
            break
        if rounds > args.messages + 2:
            raise RuntimeError("sync made no terminating progress")

    actual = [message.content_as_string() for message in received_messages]
    if args.messages > STORE_CAPACITY_MESSAGES:
        wanted = expected[-STORE_CAPACITY_MESSAGES:]
    else:
        wanted = expected

    missing = [body for body in wanted if body not in actual]
    unexpected = [body for body in actual if body not in wanted]
    duplicates = len(actual) - len(set(actual))
    passed = not missing and not unexpected and duplicates == 0 and len(actual) == len(wanted)
    print(
        "result: %s sent=%d expected=%d received=%d rounds=%d missing=%d unexpected=%d duplicates=%d"
        % (
            "PASS" if passed else "FAIL",
            args.messages,
            len(wanted),
            len(actual),
            rounds,
            len(missing),
            len(unexpected),
            duplicates,
        )
    )

    if not args.keep_state:
        shutil.rmtree(run_dir, ignore_errors=True)
    else:
        print("state retained at", run_dir)
    return 0 if passed else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        sys.exit(130)
    except Exception as error:
        print("FAIL:", error, file=sys.stderr)
        sys.exit(1)
