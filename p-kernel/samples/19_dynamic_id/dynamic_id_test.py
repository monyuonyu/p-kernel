#!/usr/bin/env python3
"""
dynamic_id_test.py — relay-side dynamic node-id lease verification.

Drives ./relay (built by run_dynamic_id.sh) as a pseudo-client over the
v2 wire (HMAC-SHA256 + nonce). It verifies the G6 first step: a client
that REGISTERs with src=0 ("auto") is leased a unique node id, fixed-id
clients never collide with leased ids, and a leased id is reclaimed and
reused after the leasing client goes idle.

No kernel wiring here — net_relay.c client integration is a follow-up
wave. This exercises only the relay's lease behaviour.

Exits 0 on PASS, non-zero on any failed assertion.
"""

import hashlib
import hmac
import os
import socket
import struct
import subprocess
import sys
import time

MAGIC   = 0x52454C59
VER_V2  = 2
HEAD    = 12
AUTH    = 24
HMAC16  = 16
REL_REGISTER = 1
REL_DATA     = 2

# 32-byte test key (0xb7 repeated) — distinct from the relay test's 0xa5.
KEY     = bytes([0xb7]) * 32
KEY_HEX = KEY.hex()

RELAY = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "..", "relay", "relay")

_fail = 0


def check(cond, msg):
    global _fail
    if cond:
        print(f"  ok   - {msg}")
    else:
        print(f"  FAIL - {msg}")
        _fail += 1


def mac(ver, typ, src, dst, nonce, payload):
    preamble = struct.pack("<BBBB", ver, typ, src, dst) + struct.pack("<Q", nonce)
    return hmac.new(KEY, preamble + payload, hashlib.sha256).digest()[:HMAC16]


def build_v2(typ, src, dst, nonce, payload=b""):
    hdr = struct.pack("<IBBBBBBBB", MAGIC, VER_V2, typ, src, dst, 0, 0, 0, 0)
    return hdr + struct.pack("<Q", nonce) + mac(VER_V2, typ, src, dst, nonce, payload) + payload


def parse(buf):
    if len(buf) < HEAD + AUTH:
        return None
    magic, ver, typ, src, dst = struct.unpack("<IBBB B", buf[:8])
    return {"ver": ver, "typ": typ, "src": src, "dst": dst}


class Client:
    """One UDP client = one source address from the relay's point of view."""
    _nonce = 1

    def __init__(self, port):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))
        self.s.settimeout(1.5)
        self.to = ("127.0.0.1", port)

    def _n(self):
        Client._nonce += 1
        return Client._nonce

    def auto_register(self):
        """Send REGISTER src=0 (auto). Return leased id from the grant, or None."""
        self.s.sendto(build_v2(REL_REGISTER, 0, 0, self._n()), self.to)
        try:
            buf, _ = self.s.recvfrom(2048)
        except socket.timeout:
            return None
        p = parse(buf)
        if not p or p["typ"] != REL_REGISTER or p["src"] != 0:
            return None
        return p["dst"]   # 0 means "denied / pool exhausted"

    def register(self, node_id):
        """Fixed-id REGISTER (legacy path). No reply expected."""
        self.s.sendto(build_v2(REL_REGISTER, node_id, 0, self._n()), self.to)

    def close(self):
        self.s.close()


def start_relay(port, idle=None):
    env = dict(os.environ)
    env["PKERNEL_RELAY_KEY"] = KEY_HEX
    if idle is not None:
        env["PKERNEL_RELAY_IDLE"] = str(idle)
    p = subprocess.Popen([RELAY, "-p", str(port), "-v"], env=env)
    time.sleep(0.5)   # let it bind
    return p


def stop_relay(p):
    p.terminate()
    try:
        p.wait(timeout=2)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


def phase_lease_and_fixed():
    print("[phase 1] unique lease + no collision with fixed ids (port 28400)")
    relay = start_relay(28400)
    try:
        a = Client(28400); b = Client(28400)
        ida = a.auto_register()
        idb = b.auto_register()
        check(ida == 1, f"first auto lease == 1 (got {ida})")
        check(idb == 2, f"second auto lease == 2 (got {idb})")
        check(ida != idb, "two auto clients get distinct ids")

        # Idempotency: same address re-requesting gets the same id back.
        ida2 = a.auto_register()
        check(ida2 == ida, f"re-request from same addr reuses id {ida} (got {ida2})")

        # A human-pinned node takes id 5, then autos must skip it.
        f5 = Client(28400)
        f5.register(5)
        time.sleep(0.15)

        c = Client(28400); d = Client(28400); e = Client(28400)
        idc = c.auto_register()
        idd = d.auto_register()
        ide = e.auto_register()
        check(idc == 3, f"auto after fixed-5 gets 3 (got {idc})")
        check(idd == 4, f"next auto gets 4 (got {idd})")
        check(ide == 6, f"next auto skips fixed-5, gets 6 (got {ide})")
        leased = {ida, idb, idc, idd, ide}
        check(5 not in leased, f"no leased id collides with fixed-5 (leased={sorted(leased)})")
        check(len(leased) == 5, f"all leased ids unique (leased={sorted(leased)})")

        for cl in (a, b, f5, c, d, e):
            cl.close()
    finally:
        stop_relay(relay)


def phase_reclaim():
    print("[phase 2] leased id reclaimed + reused after idle (port 28401, idle=1s)")
    relay = start_relay(28401, idle=1)
    try:
        f = Client(28401)
        idf = f.auto_register()
        check(idf == 1, f"fresh pool: first lease == 1 (got {idf})")
        f.close()              # client goes away; UDP has no disconnect

        # relay's last_seen/now are whole-second time_t; sleep well past
        # idle+1 so the integer-second diff always exceeds the window.
        time.sleep(2.6)        # exceed idle window so the slot is reclaimable

        g = Client(28401)      # g's REGISTER triggers eviction of idle f, then leases
        idg = g.auto_register()
        check(idg == idf, f"reclaimed id reused: g got {idg} (== f's {idf})")
        g.close()
    finally:
        stop_relay(relay)


def main():
    if not os.path.exists(RELAY):
        print(f"FATAL: relay binary not found at {RELAY} — run 'make -C relay' first")
        return 2
    phase_lease_and_fixed()
    phase_reclaim()
    print()
    if _fail == 0:
        print("[dynamic-id] PASS — all lease assertions green")
        return 0
    print(f"[dynamic-id] FAIL — {_fail} assertion(s) failed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
