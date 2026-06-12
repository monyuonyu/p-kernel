/*
 *  arch/aarch64/sign_entropy.c — first-boot keygen entropy, BARE-METAL aarch64.
 *
 *  signing.md §3.1 / D3(b) + §6.2: no /dev/urandom on bare metal. The ARMv8.5
 *  RNDR/RNDRRS system registers are the right hardware source, but they are
 *  ABSENT on the targets this port actually runs (RPi 3 = ARMv8.0; QEMU virt
 *  default = no FEAT_RNG). Rather than read an undefined register (a trap) or
 *  PRETEND we have entropy, this returns a CLEARLY-FLAGGED weak seed from the
 *  cycle counter (CNTVCT) jitter and returns 0 so the caller prints the loud
 *  "WEAK KEYGEN — bare-metal entropy is a follow-up" warning.
 *
 *  A weak keygen RNG silently weakens every signature; making it VISIBLE (not
 *  silent) is the whole point. Real aarch64 entropy (FEAT_RNG detection via
 *  ID_AA64ISAR0_EL1.RNDR, or a board TRNG) is the named follow-up.
 *
 *  Bare-metal aarch64 TU: no host libc, freestanding, inline asm for the
 *  virtual count register.
 */

static unsigned long long cntvct(void)
{
    unsigned long long v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* Fill out[0..n) with entropy. Returns 0 = WEAK always (no HW RNG on target). */
int sign_entropy(unsigned char *out, int n)
{
    unsigned long long x = cntvct();
    int i;
    for (i = 0; i < n; i++) {
        x ^= cntvct();
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = (unsigned char)(x >> 33);
    }
    return 0;                              /* WEAK — flagged at the call site */
}
