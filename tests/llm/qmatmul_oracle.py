#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# qmatmul_oracle.py — INDEPENDENT reference for the M1b quantized matmul cert.
#
# This is the "validator-trap" guard: it does NOT call the C code at all. It
# parses the SAME GGUF file from scratch (its own struct-based header reader),
# locates the SAME tensor by name, dequantizes the Q8_0 blocks with its OWN
# fp16->fp32 routine and its OWN matmul, regenerates the SAME pseudo-random x
# from the shared seed, and diffs its y against the C harness's dumped y[].
#
# Independence: different language, different file parser, different fp16 decode
# (Python float arithmetic), different accumulation. A bug shared by both would
# have to be in the *spec interpretation*, which is exactly what we want to
# catch by also having the self-contained synthetic C cert.
#
# Usage:
#   python3 qmatmul_oracle.py <model.gguf> <dut_dump.txt> [tol]
#   (dut_dump.txt is written by qmatmul_test.c: tensor/seed/in/out + y[] lines)
#
# Exit 0 = PASS (max abs err < tol, default 1e-3 absolute OR 1e-4 relative).
# ---------------------------------------------------------------------------
import sys, struct

# ---- minimal independent GGUF reader (header + metadata + tensor table) ----
GGUF_MAGIC = b"GGUF"
(T_U8, T_I8, T_U16, T_I16, T_U32, T_I32, T_F32, T_BOOL,
 T_STR, T_ARR, T_U64, T_I64, T_F64) = range(13)

_SCALAR_FMT = {
    T_U8: ("<B", 1), T_I8: ("<b", 1), T_U16: ("<H", 2), T_I16: ("<h", 2),
    T_U32: ("<I", 4), T_I32: ("<i", 4), T_F32: ("<f", 4), T_BOOL: ("<B", 1),
    T_U64: ("<Q", 8), T_I64: ("<q", 8), T_F64: ("<d", 8),
}

class Reader:
    def __init__(self, buf):
        self.b = buf
        self.o = 0
    def take(self, n):
        v = self.b[self.o:self.o + n]; self.o += n; return v
    def u32(self): return struct.unpack_from("<I", self.b, self._adv(4))[0]
    def u64(self): return struct.unpack_from("<Q", self.b, self._adv(8))[0]
    def _adv(self, n):
        o = self.o; self.o += n; return o
    def gstr(self):
        n = self.u64(); return self.take(n)
    def scalar(self, t):
        fmt, sz = _SCALAR_FMT[t]
        v = struct.unpack_from(fmt, self.b, self._adv(sz))[0]
        return v

def skip_value(r, t):
    """advance past a metadata value of type t (we only need to skip)."""
    if t == T_STR:
        r.gstr()
    elif t == T_ARR:
        et = r.u32(); n = r.u64()
        for _ in range(n):
            skip_value(r, et)
    else:
        r.scalar(t)

def parse_gguf(path):
    with open(path, "rb") as f:
        buf = f.read()
    r = Reader(buf)
    if r.take(4) != GGUF_MAGIC:
        raise ValueError("not a GGUF file")
    version = r.u32()
    if version not in (2, 3):
        raise ValueError("unsupported GGUF version %d" % version)
    n_tensors = r.u64()
    n_kv = r.u64()
    alignment = 32
    # metadata: we only need general.alignment; skip the rest
    for _ in range(n_kv):
        key = r.gstr()
        vt = r.u32()
        if key == b"general.alignment" and vt == T_U32:
            alignment = r.scalar(T_U32)
        else:
            skip_value(r, vt)
    # tensor table
    tensors = []
    for _ in range(n_tensors):
        name = r.gstr()
        nd = r.u32()
        dims = [r.u64() for _ in range(nd)]
        ggml_type = r.u32()
        offset = r.u64()
        tensors.append((name, dims, ggml_type, offset))
    # data section start = align(current offset, alignment)
    data_start = (r.o + alignment - 1) // alignment * alignment
    return buf, alignment, data_start, tensors

# ---- independent fp16 -> python float -------------------------------------
def fp16_to_float(h):
    sign = -1.0 if (h & 0x8000) else 1.0
    exp = (h >> 10) & 0x1F
    mant = h & 0x3FF
    if exp == 0:
        if mant == 0:
            return sign * 0.0
        return sign * mant * (2.0 ** -24)          # subnormal
    if exp == 0x1F:
        return sign * (float("nan") if mant else float("inf"))
    return sign * (1.0 + mant / 1024.0) * (2.0 ** (exp - 15))

# ---- the same xorshift PRNG as the C harness ------------------------------
def prng(seed):
    state = seed & 0xFFFFFFFF
    if state == 0:
        state = 0x9E3779B9
    while True:
        x = state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        state = x
        r = x >> 8                                 # 24 bits
        yield r / 8388608.0 - 1.0                  # [-1, 1)

GGML_TYPE_Q8_0 = 8

def dequant_q8_0_matmul(buf, base, in_f, out_f, x):
    """y[i] = sum_j W[i][j]*x[j], dequantizing Q8_0 blocks independently."""
    nblk = in_f // 32
    row_bytes = nblk * 34
    y = [0.0] * out_f
    for i in range(out_f):
        row = base + i * row_bytes
        acc = 0.0
        col = 0
        for b in range(nblk):
            blk = row + b * 34
            d = fp16_to_float(buf[blk] | (buf[blk + 1] << 8))
            for k in range(32):
                q = buf[blk + 2 + k]
                if q >= 128:
                    q -= 256                       # int8
                acc += d * q * x[col + k]
            col += 32
        y[i] = acc
    return y

def main():
    if len(sys.argv) < 3:
        print("usage: qmatmul_oracle.py <model.gguf> <dut_dump.txt> [tol]")
        return 2
    gguf_path = sys.argv[1]
    dump_path = sys.argv[2]
    tol_abs = float(sys.argv[3]) if len(sys.argv) >= 4 else 1e-3

    # read the C harness's dump
    name = None; seed = None; in_f = None; out_f = None; dut = []
    with open(dump_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("tensor "):
                name = line.split(" ", 1)[1]
            elif line.startswith("seed "):
                seed = int(line.split()[1])
            elif line.startswith("in "):
                in_f = int(line.split()[1])
            elif line.startswith("out "):
                out_f = int(line.split()[1])
            else:
                dut.append(float(line))
    if None in (name, seed, in_f, out_f):
        print("ORACLE FAIL: malformed dump"); return 1
    if len(dut) != out_f:
        print("ORACLE FAIL: dump has %d y[] but out=%d" % (len(dut), out_f)); return 1

    buf, alignment, data_start, tensors = parse_gguf(gguf_path)

    target = None
    for (tname, dims, ttype, off) in tensors:
        if tname.decode("utf-8", "replace") == name:
            target = (dims, ttype, off); break
    if target is None:
        print("ORACLE FAIL: tensor %r not found" % name); return 1
    dims, ttype, off = target
    if ttype != GGML_TYPE_Q8_0:
        print("ORACLE FAIL: tensor is not Q8_0 (type %d)" % ttype); return 1
    if dims[0] != in_f or dims[1] != out_f:
        print("ORACLE FAIL: shape mismatch %r vs (in=%d,out=%d)" % (dims, in_f, out_f)); return 1

    base = data_start + off

    # regenerate x from the shared seed (must match the C harness byte-for-byte)
    g = prng(seed)
    x = [next(g) for _ in range(in_f)]

    y = dequant_q8_0_matmul(buf, base, in_f, out_f, x)

    # diff
    max_abs = 0.0
    max_rel = 0.0
    for a, b in zip(y, dut):
        e = abs(a - b)
        if e > max_abs:
            max_abs = e
        denom = max(abs(a), abs(b), 1e-9)
        r = e / denom
        if r > max_rel:
            max_rel = r

    print("ORACLE: tensor=%s in=%d out=%d" % (name, in_f, out_f))
    print("ORACLE: oracle y[0..4] = %s" % " ".join("%.6f" % v for v in y[:5]))
    print("ORACLE: dut    y[0..4] = %s" % " ".join("%.6f" % v for v in dut[:5]))
    print("ORACLE: max abs err = %.3e   max rel err = %.3e" % (max_abs, max_rel))

    # accept if absolute small OR relative small (fp accumulation-order caveat)
    ok = (max_abs < tol_abs) or (max_rel < 1e-4)
    print("ORACLE: %s (tol_abs=%.1e)" % ("PASS" if ok else "FAIL", tol_abs))
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
