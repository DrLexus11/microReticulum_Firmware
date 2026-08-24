#!/usr/bin/env python3
# Watch for LXMF delivery announces so we can pick up Columba's address
# without reading a hash off a phone screen.
import time, RNS, LXMF
from RNS.vendor import umsgpack as msgpack

RNS.loglevel = RNS.LOG_WARNING
r = RNS.Reticulum()

class Handler:
    def __init__(self, aspect):
        self.aspect_filter = aspect
        self.receive_path_responses = False
    def received_announce(self, destination_hash, announced_identity, app_data):
        name = "?"
        if app_data:
            try:
                d = msgpack.unpackb(app_data)
                name = d[0].decode() if isinstance(d, list) and d and d[0] else str(d)
            except Exception:
                try: name = app_data.decode("utf-8", "replace")
                except Exception: name = RNS.hexrep(app_data[:16])
        print(f"[{self.aspect_filter}] {RNS.prettyhexrep(destination_hash)}  name={name!r}", flush=True)

RNS.Transport.register_announce_handler(Handler("lxmf.delivery"))
RNS.Transport.register_announce_handler(Handler("lxmf.propagation"))
print("watching for lxmf announces... (hit Announce in Columba)", flush=True)
while True: time.sleep(1)
