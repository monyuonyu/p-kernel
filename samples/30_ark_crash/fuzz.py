#!/usr/bin/env python3
# ===========================================================================
# fuzz.py - format-agnostic crash-safety fuzzer for ARK (arch/common/arkfs.c).
#
# WHAT IT PROVES (or disproves), honestly:
#   ARK claims: "a fresh mount yields either the last committed version
#   complete, or the new version complete, and NEVER serves corrupt data."
#
#   The existing crash harness (samples/25) models ONLY prefix truncation
#   (SIGKILL => writes stop at some sector). Real storage can also REORDER
#   and DROP the un-fsync'd tail, and individual sectors can TEAR or ROT.
#   This fuzzer perturbs the on-disk image at the SECTOR level (512 B) in four
#   realistic ways and re-mounts a FRESH process after each perturbation.
#
# DECOUPLING / FORMAT-AGNOSTIC:
#   This tool NEVER links arkfs.c, NEVER includes arkfs.h, and knows NOTHING
#   about ARK's on-disk record layout. It drives ARK only through the stable
#   sample CLI verbs of the samples/25 `arkfs_test` binary (format/write/read/
#   version/readv). It discovers the "write-set" (the sectors a single ARK
#   write touched) by snapshotting the image and diffing 512 B sectors before
#   and after the write -- no internal knowledge required. The only ARK-shaped
#   thing it looks for is its OWN payload marker string inside changed sectors,
#   to tell payload sectors apart from metadata sectors.
#
# CLASSIFICATION of each perturbed re-mount:
#   OK-new      mount serves the NEW version, complete & self-verified.
#   OK-old      mount serves a PRIOR committed version, complete & verified.
#   SAFE-reject mount degraded: the current view is withheld (ARK_E_CORRUPT /
#               not-found) but NO corrupt bytes are served AND a prior good
#               version is still recoverable. (Integrity kept; "always
#               last-good" NOT kept -- the known 🟡6 corner.)
#   BUG         corrupt bytes were served, OR the store is wedged /
#               unrecoverable (no good version obtainable at all).
#
# Exit 0 iff zero BUG runs. Deterministic: all randomness is seeded.
# ===========================================================================
import argparse
import os
import subprocess
import sys
from collections import Counter, OrderedDict
import random

SECTOR = 512  # ARK_SECTOR (the I/O quantum); this is the only ARK constant we

# ---- the three versions we drive through ARK (markers must be unique) -------
C1 = "ARKFUZZ::v1::alpha::" + ("." * 40)
C2 = "ARKFUZZ::v2::beta::" + ("-" * 40)
# v3 is intentionally ~600 bytes so its payload spans MORE THAN ONE sector,
# which makes torn-sector tests land on live bytes (not just zero padding).
C3 = "ARKFUZZ::v3::gamma::" + ("G" * 600)
MARK3 = b"ARKFUZZ::v3::gamma::"

# read content -> (role, version). v2 is the immediate prior committed version.
KNOWN = {C1: ("old", 1), C2: ("old", 2), C3: ("new", 3)}

GARBAGE = 0xA5  # the byte a "torn" / "rotted" sector half is filled with.


class Ark:
    """Black-box driver around the samples/25 arkfs_test binary."""

    def __init__(self, binpath, img):
        self.bin = binpath
        self.img = img

    def _run(self, *args):
        r = subprocess.run([self.bin, *args], capture_output=True, text=True)
        return r.stdout.strip(), r.returncode

    def format(self, nsect):
        self._run("format", self.img, str(nsect))

    def write(self, path, content):
        self._run("write", self.img, path, content)

    def snapshot(self):
        with open(self.img, "rb") as f:
            return bytearray(f.read())

    def restore(self, data):
        with open(self.img, "wb") as f:
            f.write(data)

    def read(self, path="/r.txt"):
        return self._run("read", self.img, path)

    def version(self, path="/r.txt"):
        return self._run("version", self.img, path)

    def readv(self, path, ver):
        return self._run("readv", self.img, path, str(ver))


def sector(buf, i):
    return bytes(buf[i * SECTOR:(i + 1) * SECTOR])


def n_sectors(buf):
    return len(buf) // SECTOR


def diff_sectors(a, b):
    """Sector indices that differ between two images (the write-set)."""
    n = min(n_sectors(a), n_sectors(b))
    return [i for i in range(n) if sector(a, i) != sector(b, i)]


def apply_sectors(base, src, indices):
    """Return base with the given sector indices overwritten from src."""
    img = bytearray(base)
    for i in indices:
        img[i * SECTOR:(i + 1) * SECTOR] = sector(src, i)
    return img


def tear_sector(base, src, idx):
    """Return base with sector idx torn: first half from src, 2nd half garbage.
    Models a sector that began writing then lost power mid-sector."""
    img = bytearray(base)
    half = SECTOR // 2
    img[idx * SECTOR:idx * SECTOR + half] = src[idx * SECTOR:idx * SECTOR + half]
    img[idx * SECTOR + half:(idx + 1) * SECTOR] = bytes([GARBAGE] * half)
    return img


# ---------------------------------------------------------------------------
# build the baseline scenario: a durable v1+v2, then v3 as the "crashing" write
# ---------------------------------------------------------------------------
def build_scenario(ark, nsect):
    ark.format(nsect)
    ark.write("/r.txt", C1)            # v1 (durable)
    ark.write("/r.txt", C2)            # v2 (durable; the LAST committed version)
    committed = ark.snapshot()         # <- prior-committed baseline
    ark.write("/r.txt", C3)            # v3 (the write we will "crash")
    new = ark.snapshot()               # <- new fully-committed image

    # sanity: a clean, fully-applied v3 must be OK-new, else the harness is wrong
    klass, _, _ = classify(ark, new)
    if klass != "OK-new":
        sys.exit(f"FATAL: clean v3 image did not classify OK-new (got {klass}); "
                 f"the arkfs_test CLI may have changed.")

    ws = diff_sectors(committed, new)
    # payload sectors = changed sectors whose NEW content carries our marker.
    payload = [i for i in ws if MARK3 in sector(new, i)]
    return committed, new, ws, payload


# ---------------------------------------------------------------------------
# classify a perturbed image by remounting a fresh process and reading
# ---------------------------------------------------------------------------
def classify(ark, img, path="/r.txt"):
    """Returns (klass, served, detail)."""
    ark.restore(img)
    out, rc = ark.read(path)

    if out == "MOUNT-FAIL":
        return "BUG", None, "store unmountable (total loss / wedged)"

    if out.startswith("READ: "):
        served = out[len("READ: "):]
        if served not in KNOWN:
            return "BUG", served, "CORRUPT BYTES SERVED (not any written version)"
        role, ver = KNOWN[served]
        vout, _ = ark.version(path)
        if role == "new":
            return "OK-new", served, f"served new version ({vout})"
        return "OK-old", served, f"served prior committed version ({vout})"

    # current view withheld (CORRUPT / NOTFOUND). Is a prior version recoverable,
    # and is everything it would serve still self-consistent (never corrupt)?
    reason = "CORRUPT" if out == "CORRUPT" else (out if out else "empty")
    recovered = None
    for ver in (2, 1):
        vout, vrc = ark.readv(path, ver)
        if vout.startswith("READV("):
            body = vout.split("): ", 1)[1] if "): " in vout else ""
            if body in KNOWN:
                recovered = ver
                break
            if vrc == 0 and body and body not in KNOWN:
                return "BUG", body, "CORRUPT BYTES SERVED via readv"
    if recovered is not None:
        return ("SAFE-reject", None,
                f"current view {reason}; prior v{recovered} still recoverable "
                f"(no auto-fallback -- the 🟡6 corner)")
    return "BUG", None, f"current view {reason} AND no prior version recoverable"


# ---------------------------------------------------------------------------
# perturbation classes
# ---------------------------------------------------------------------------
def class_prefix(ark, committed, new, ws):
    """(a) PREFIX TRUNCATE at every sector boundary of the write-set.
    This is the baseline the existing harness already covers."""
    runs = []
    order = sorted(ws)  # ARK appends sequentially -> ascending lba == write order
    for j in range(len(order) + 1):
        landed = order[:j]
        img = apply_sectors(committed, new, landed)
        klass, served, detail = classify(ark, img)
        runs.append((klass, f"prefix cut={j} landed={landed}", detail, served))
    return runs


def class_reorder(ark, committed, new, ws, payload, rng, nrand):
    """(b) REORDER / DROP the recently-written tail. Models a storage cache
    that reorders or loses an arbitrary SUBSET of the un-fsync'd sectors."""
    runs = []
    order = sorted(ws)

    # deterministic, targeted: drop EACH single payload sector but keep the
    # rest of the write-set (incl. the commit) -- the exact reorder that the
    # prefix-only harness can never produce, and that triggers 🟡6.
    for ps in payload:
        landed = [i for i in order if i != ps]
        img = apply_sectors(committed, new, landed)
        klass, served, detail = classify(ark, img)
        runs.append((klass, f"reorder DROP payload sector {ps}, keep {landed}",
                     detail, served))

    # randomized subsets (permute, then keep a random prefix => arbitrary subset)
    for _ in range(nrand):
        perm = order[:]
        rng.shuffle(perm)
        k = rng.randint(0, len(perm))
        landed = sorted(perm[:k])
        img = apply_sectors(committed, new, landed)
        klass, served, detail = classify(ark, img)
        runs.append((klass, f"reorder landed={landed}", detail, served))
    return runs


def class_tear(ark, committed, new, ws, payload):
    """(c) TORN SECTOR: a sector half-written then power lost.
    Two variants per payload sector:
      stop : apply the write-set prefix up to the torn sector, then STOP
             (classic crash -- commit never reached). Expect OK-old.
      keep : apply the WHOLE write-set, then tear an interior payload sector
             (the commit SURVIVES the tear -- the reordering window). This is
             the case the prefix-only harness cannot reach."""
    runs = []
    order = sorted(ws)
    for ps in payload:
        # variant: stop right after the torn sector
        prefix = [i for i in order if i < ps]
        base = apply_sectors(committed, new, prefix)
        img = tear_sector(base, new, ps)
        klass, served, detail = classify(ark, img)
        runs.append((klass, f"tear+STOP at payload sector {ps} (prefix {prefix})",
                     detail, served))

        # variant: full write-set applied, then tear this payload sector
        base = apply_sectors(committed, new, order)
        img = tear_sector(base, new, ps)
        klass, served, detail = classify(ark, img)
        runs.append((klass, f"tear payload sector {ps} but commit SURVIVES",
                     detail, served))
    return runs


def class_bitflip(ark, committed, new, ws, rng, nrand):
    """(d) BIT-FLIP random bytes in random sectors (media rot)."""
    runs = []

    # deterministic: rot the SUPERBLOCK (sector 0). No backup copy exists.
    img = bytearray(new)
    img[10] ^= 0xFF
    klass, served, detail = classify(ark, img)
    runs.append((klass, "bitflip SUPERBLOCK sector 0 byte 10",
                 detail + "  [SB has no replica -> 🟡5]", served))

    # deterministic: rot the FIRST log record's HEADER (sector 1). The mount
    # scan STOPS at the first invalid header, so everything after it is
    # discarded -- one rotted header byte silently empties the whole library.
    img = bytearray(new)
    img[1 * SECTOR + 0] ^= 0xFF
    klass, served, detail = classify(ark, img)
    runs.append((klass, "bitflip FIRST log record header (sector 1 byte 0)",
                 detail + "  [scan stops at torn header -> whole log truncated]",
                 served))

    # randomized: flip bytes inside the written LOG region (sectors >= 1).
    log_sectors = sorted(set(range(1, n_sectors(new))) & set(
        diff_sectors(bytearray(n_sectors(new) * SECTOR), new)))
    if not log_sectors:
        log_sectors = list(range(1, max(2, n_sectors(new))))
    for _ in range(nrand):
        img = bytearray(new)
        nflip = rng.randint(1, 4)
        ops = []
        for _ in range(nflip):
            s = rng.choice(log_sectors)
            off = rng.randint(0, SECTOR - 1)
            img[s * SECTOR + off] ^= (1 << rng.randint(0, 7))
            ops.append((s, off))
        klass, served, detail = classify(ark, img)
        runs.append((klass, f"bitflip log {ops}", detail, served))
    return runs


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="path to arkfs_test binary")
    ap.add_argument("--img", required=True, help="scratch image file path")
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--sectors", type=int, default=256)
    ap.add_argument("--runs", type=int, default=64,
                    help="randomized runs per randomized class")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    ark = Ark(args.bin, args.img)

    print("=" * 75)
    print(" ARK crash-safety fuzzer (samples/30_ark_crash)")
    print(f"   binary : {args.bin}")
    print(f"   seed   : {args.seed}    image: {args.sectors} sectors"
          f"    randomized runs/class: {args.runs}")
    print("=" * 75)

    committed, new, ws, payload = build_scenario(ark, args.sectors)
    print(f" baseline: durable v1+v2 committed; v3 write-set = sectors {ws}")
    print(f"           payload sectors (carry v3 marker) = {payload}")
    print(f"           prior committed = v2, new = v3; clean v3 => OK-new (ok)")
    print()

    classes = OrderedDict()
    classes["(a) prefix-truncate"] = class_prefix(ark, committed, new, ws)
    classes["(b) reorder/drop"] = class_reorder(ark, committed, new, ws,
                                                payload, rng, args.runs)
    classes["(c) torn-sector"] = class_tear(ark, committed, new, ws, payload)
    classes["(d) bit-flip"] = class_bitflip(ark, committed, new, ws, rng, args.runs)

    cols = ["OK-old", "OK-new", "SAFE-reject", "BUG"]
    print(" PERTURBATION                 runs  OK-old  OK-new  SAFE-rej   BUG")
    print(" " + "-" * 71)
    grand = Counter()
    bugs = []
    safe_samples = []
    for name, runs in classes.items():
        c = Counter(k for (k, _, _, _) in runs)
        grand.update(c)
        print(f" {name:<27}{len(runs):>5}{c['OK-old']:>8}{c['OK-new']:>8}"
              f"{c['SAFE-reject']:>10}{c['BUG']:>6}")
        for (k, desc, detail, served) in runs:
            if k == "BUG":
                bugs.append((name, desc, detail, served))
            elif k == "SAFE-reject" and len(safe_samples) < 6:
                safe_samples.append((name, desc, detail))
    print(" " + "-" * 71)
    total = sum(grand.values())
    print(f" {'TOTAL':<27}{total:>5}{grand['OK-old']:>8}{grand['OK-new']:>8}"
          f"{grand['SAFE-reject']:>10}{grand['BUG']:>6}")
    print()

    if safe_samples:
        print(" SAFE-reject samples (no corruption served, but NOT auto-last-good):")
        for (name, desc, detail) in safe_samples:
            print(f"   [{name}] {desc}")
            print(f"        -> {detail}")
        print()

    print(" REPRODUCERS for every BUG (corrupt-served / wedged):")
    if not bugs:
        print("   (none)")
    else:
        for (name, desc, detail, served) in bugs:
            print(f"   seed={args.seed} class={name}")
            print(f"        op : {desc}")
            print(f"        -> {detail}"
                  + (f"  served={served!r}" if served else ""))
    print()

    print("=" * 75)
    if grand["BUG"] == 0:
        print(" VERDICT: no corrupt bytes served and no wedge across all classes.")
        print("          (SAFE-reject runs are honest degradations, not failures.)")
        print("=" * 75)
        return 0
    print(f" VERDICT: {grand['BUG']} BUG run(s) -- ARK served corrupt data or "
          f"wedged. See reproducers above.")
    print("=" * 75)
    return 1


if __name__ == "__main__":
    sys.exit(main())
