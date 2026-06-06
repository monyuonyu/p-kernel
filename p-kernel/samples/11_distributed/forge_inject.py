#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# G4 (wave 10): client-side relay-frame forgery injector.
#
# Stands up a *malicious relay* on 127.0.0.1:<port>, waits for a p-kernel
# node to REGISTER (so we learn its UDP tuple), then injects v2 frames with
# bogus HMACs from the relay's own address — exactly the "spoof the relay,
# inject arbitrary frames" attack the audit flagged. A correct client must
# verify the HMAC on inbound frames and DROP these, emitting
# "[net_relay] mac drop n=..." (see arch/linux/*/net_relay.c).
#
# It also sends one CORRECTLY-MAC'd frame to prove the same path accepts
# legitimate traffic (no false drop).
#
# Usage: forge_inject.py <port> <hex_key>
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

def frame(key, ver, typ, src, dst, nonce, payload, mac=None):
    # Wire head is 12 bytes: magic(4) ver type src dst (4) + 4 zero pad bytes
    # (HEAD_LEN=12 in net_relay.c — buf[8..11] are zeroed). Omitting the pad
    # shifts the nonce/mac/payload by 4 bytes so EVERY frame fails MAC
    # verification — which silently made the "valid" frame below fail too.
    h = struct.pack("<IBBBB", MAGIC, ver, typ, src, dst) + b"\x00\x00\x00\x00"
    if ver == 2:
        if mac is None:
            mac = hmac16(key, ver, typ, src, dst, nonce, payload)
        return h + struct.pack("<Q", nonce) + mac + payload
    return h + payload

def main():
    port = int(sys.argv[1])
    key = bytes.fromhex(sys.argv[2])

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(20)
    print("[forge] malicious relay up on 127.0.0.1:%d" % port, flush=True)

    node = None
    while node is None:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            print("[forge] FAIL: no REGISTER seen", flush=True)
            return 1
        if len(data) >= 12 and data[5] == 1:   # type == REL_REGISTER
            node = addr
            print("[forge] node registered from %s src=%d" % (addr, data[6]),
                  flush=True)

    payload = b"\x00" * 40
    n = (int(time.time()) << 24) | 5

    # 1) forged BROADCAST, garbage MAC -> must be dropped
    s.sendto(frame(key, 2, 4, 2, 0, n, payload, mac=b"\xde\xad\xbe\xef" * 4),
             node)
    print("[forge] sent FORGED frame (garbage mac)", flush=True)
    time.sleep(1.1)   # > rate-limit so a second drop would log too
    # 2) forged BROADCAST, zero MAC -> must be dropped
    s.sendto(frame(key, 2, 4, 2, 0, n + 1, payload, mac=b"\x00" * 16), node)
    print("[forge] sent FORGED frame #2 (zero mac)", flush=True)
    time.sleep(0.5)
    # 3) correctly-MAC'd BROADCAST -> must NOT be dropped
    s.sendto(frame(key, 2, 4, 2, 0, n + 2, payload), node)
    print("[forge] sent VALID frame (good mac)", flush=True)
    time.sleep(1)
    print("[forge] done", flush=True)
    s.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
