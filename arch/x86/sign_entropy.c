/*
 *  arch/x86/sign_entropy.c — first-boot keygen entropy, BARE-METAL x86.
 *
 *  signing.md §3.1 / D3(b) + §6.2: bare metal has no /dev/urandom. The seed
 *  for the node's Ed25519 keypair must still be unpredictable, because a weak
 *  keygen RNG SILENTLY weakens every signature the node ever makes. So:
 *    - prefer RDRAND (Intel DRNG) when CPUID reports it — a real hardware RNG;
 *    - otherwise fall back to a CLEARLY-FLAGGED weak source (TSC jitter) and
 *      return 0 so the caller prints the loud "WEAK KEYGEN" warning.
 *  Never silent: a weak seed is always visible at the call site.
 *
 *  Bare-metal x86 TU: no host libc. Plain types; inline asm for cpuid/rdrand/
 *  rdtsc. Freestanding.
 */

/* CPUID leaf 1, ECX bit 30 = RDRAND available. */
static int have_rdrand(void)
{
    unsigned int eax, ebx, ecx, edx;
    eax = 1;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(eax), "c"(0));
    return (ecx >> 30) & 1u;
}

static int rdrand32(unsigned int *out)
{
    unsigned char ok;
    unsigned int v;
    int tries;
    for (tries = 0; tries < 10; tries++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) ::);
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

static unsigned long long rdtsc64(void)
{
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/* Fill out[0..n) with entropy. Returns 1 = strong (RDRAND), 0 = weak. */
int sign_entropy(unsigned char *out, int n)
{
    int i;
    if (have_rdrand()) {
        int ok = 1;
        for (i = 0; i < n; i += 4) {
            unsigned int v;
            if (!rdrand32(&v)) { ok = 0; break; }
            out[i + 0] = (unsigned char)(v);
            if (i + 1 < n) out[i + 1] = (unsigned char)(v >> 8);
            if (i + 2 < n) out[i + 2] = (unsigned char)(v >> 16);
            if (i + 3 < n) out[i + 3] = (unsigned char)(v >> 24);
        }
        if (ok) return 1;                  /* STRONG */
    }
    /* WEAK fallback — TSC jitter. NOT cryptographic; caller warns loudly. */
    {
        unsigned long long x = rdtsc64();
        for (i = 0; i < n; i++) {
            x ^= rdtsc64();
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            out[i] = (unsigned char)(x >> 33);
        }
    }
    return 0;                              /* WEAK */
}
