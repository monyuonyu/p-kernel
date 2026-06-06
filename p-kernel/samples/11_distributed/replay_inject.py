#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# Wave 11: client-side relay-frame REPLAY injector.
#
# Wave 10 (G4) proved the client drops frames with a bad HMAC. But a frame
# captured off the wire and resent verbatim carries a *valid* HMAC, so MAC
# verification alone re-admits it. This injector proves the client also keeps
# a per-source replay window and drops repeats.
#
# Stands up a *malicious relay* on 127.0.0.1:<port>, waits for the node to
# REGISTER (so we learn its UDP tuple), then:
#   1. sends three DISTINCT correctly-MAC'd BROADCAST frames (nonces n,n+1,n+2)
#      -> all fresh, must ALL be accepted, ZERO replay drops;
#   2. resends frame #2 (nonce n+1) verbatim -> a valid-MAC REPLAY, which the
#      receive-side window MUST drop, emitting
#      "[net_relay] replay drop n=..." (see arch/linux/*/net_relay.c).
#   3. resends frame #1 (nonce n) verbatim -> a second, older replay.
#
# The fresh-then-replay ordering (n,n+1,n+2 then replay n+1) exercises the
# "already-seen bit inside the window" path, not just the trivial max case.
#
# Usage: replay_inject.py <port> <hex_key>
# Exits 0 once a node has registered and all injections were sent.
# ---------------------------------------------------------------------------
import socket, struct, hashlib, time, sys

MAGIC = 0x52454C59


def hmac16(key, ver, typ, src, dst, nonce, payload):
    pre = bytes([ver, typ, src, dst]) + struct.pack("<Q", nonce)
    msg = pre + payload
    bs = 64
    k = key + b"\x00" * (bs - len(key))
    ipad = bytes(b ^ 0x36 for b in k)
    opad = bytes(b ^ 0x5c for b in k)
    inner = hashlib.sha256(ipad + msg).digest()
    return hashlib.sha256(opad + inner).digest()[:16]


def frame(key, ver, typ, src, dst, nonce, payload):
    # Wire head is 12 bytes: magic(4) ver type src dst (4) + 4 zero pad bytes
    # (HEAD_LEN=12 in net_relay.c — buf[8..11] are zeroed). Getting this wrong
    # silently shifts the nonce/mac/payload by 4 bytes and every frame fails
    # MAC verification regardless of the key.
    h = struct.pack("<IBBBB", MAGIC, ver, typ, src, dst) + b"\x00\x00\x00\x00"
    if ver == 2:
        mac = hmac16(key, ver, typ, src, dst, nonce, payload)
        return h + struct.pack("<Q", nonce) + mac + payload
    return h + payload


def main():
    port = int(sys.argv[1])
    key = bytes.fromhex(sys.argv[2])

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(20)
    print("[replay] malicious relay up on 127.0.0.1:%d" % port, flush=True)

    node = None
    while node is None:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            print("[replay] FAIL: no REGISTER seen", flush=True)
            return 1
        if len(data) >= 12 and data[5] == 1:   # type == REL_REGISTER
            node = addr
            print("[replay] node registered from %s src=%d"
                  % (addr, data[6]), flush=True)

    payload = b"\x00" * 40
    n = (int(time.time()) << 24) | 5

    # Build three distinct, individually-valid BROADCAST frames (src=2).
    f0 = frame(key, 2, 4, 2, 0, n,     payload)
    f1 = frame(key, 2, 4, 2, 0, n + 1, payload)
    f2 = frame(key, 2, 4, 2, 0, n + 2, payload)

    # 1) three fresh frames -> all must be accepted, none counted as replay.
    s.sendto(f0, node); print("[replay] sent FRESH nonce+0", flush=True)
    time.sleep(0.2)
    s.sendto(f1, node); print("[replay] sent FRESH nonce+1", flush=True)
    time.sleep(0.2)
    s.sendto(f2, node); print("[replay] sent FRESH nonce+2", flush=True)
    time.sleep(0.5)

    # 2) replay frame #2 (already inside the window) -> MUST be dropped.
    s.sendto(f1, node)
    print("[replay] re-sent nonce+1 (REPLAY, valid mac)", flush=True)
    time.sleep(1.1)   # > rate-limit so a second drop logs too
    # 3) replay frame #1 (older, still inside the window) -> MUST be dropped.
    s.sendto(f0, node)
    print("[replay] re-sent nonce+0 (REPLAY, valid mac)", flush=True)
    time.sleep(1)
    print("[replay] done", flush=True)
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
