/*
 *  student_devfit_test.c — host cert for DEVICE-CAPACITY mind-sizing (DEVFIT-1).
 *  Design: docs/architecture/device-capacity-mind-sizing-plan.md (HARDENED).
 *
 *  CLAIM: the SAME student binary, given different INJECTED device profiles
 *  (PKERNEL_DEVICE_RAM_BYTES / PKERNEL_DEVICE_CORES, fixture-injected so CI
 *  needs no real hardware), sizes its student to the matching tier and REFUSES
 *  to OOM a small device. This is the mind-sizing analogue of SMP-AUTODETECT's
 *  "same binary, -smp 2/4/8, adapts" proof.
 *
 *  Certs (all in-process, no network):
 *    [device-fit]            each profile -> tier_of() == expected tier AND the
 *                            student arena allocates (no ST_E_OOM). RAM is the
 *                            bottleneck (512MB/8-core -> S, not L). Thresholds
 *                            are the COMPUTED per-tier arena cost (printed) so a
 *                            degenerate always-M tier_of would FAIL the L/S rows
 *                            (non-vacuous).
 *    [device-fit-monotone]   sweep RAM up -> tier_of non-decreasing (saturates
 *                            at L, never inverts).
 *    [device-fit-ceiling]    SS-4 reconciliation (RULE [device-ceiling-fleet-
 *                            grows-within]): on an S device a large fleet
 *                            (cap_experts=10) clamps the growth target to
 *                            ST_E_S=2, NOT cap_experts (=10) / CAP_E_MAX (=16).
 *                            Closes SS-4 open-risk #7.
 *    [tier-forward-pin]      the per-tier student forward hash is pinned (RE-
 *                            PINNED post SCALE-WALL C1 = RoPE + ST_MAXSEQ 64->256,
 *                            commit 7d497d7c, which moved st_forward every tier):
 *                            S=6baf2e14f370de17, M=604895ba9c2b1c9d,
 *                            L=5ba2c68f6ca0ba89. SAME recipe as student_ss6_test
 *                            (CORPUS, warm_train 1 step, FNV-1a logit hash), so
 *                            the pins match the SS-6 single= hashes bit-for-bit.
 *
 *  THE FALSIFIER (driven by run_devfit.sh, not this TU): a binary built
 *  -DDEVFIT_IGNORE_MEASURE makes st_init_device hardcode ST_TIER_L and DISABLE
 *  the step-down. Under the 512MB profile it tries to malloc the ~314MB L arena;
 *  if it OOMs the falsifier cert goes RED -> proves the measurement is load-
 *  bearing. (We ALSO exercise st_init_device's real step-down here, [step-down].)
 *
 *  Build (wave-49): -O1 -ffp-contract=off (one math). The forward hashes are
 *  bit-stable so the pins hold cross-arch.
 *
 *  Usage:
 *    ./student_devfit            # human cert (exit 0 = all PASS)
 *    ./student_devfit --machine  # one line per profile (profile -> tier)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../arch/common/llm/student.h"
#include "../../arch/common/llm/dev_capacity.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); g_pass++; } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } } while (0)

static const char *TNAME[ST_NTIER] = { "S", "M", "L" };

/* ---- the forward-hash recipe, IDENTICAL to student_ss6_test --------------- */
static const uint8_t CORPUS[] =
    "the cat sat on the mat. the dog ran in the sun. she said the sea is "
    "blue and the sky is blue too. a bird sang and the wind blew softly.";

static void warm_train(st_model *m, int steps)
{
    int n = (int)sizeof(CORPUS) - 1;
    int win = n < ST_MAXSEQ ? n : ST_MAXSEQ;
    float *logits = (float *)malloc((size_t)win * 256 * sizeof(float));
    if (!logits) return;
    for (int s = 0; s < steps; s++) {
        st_zero_grad(m);
        st_forward(m, CORPUS, win, logits);
        st_backward(m, CORPUS, win);
        st_adam_step(m, 0.02f);
    }
    free(logits);
}

static uint64_t logit_hash(const float *logits, int n_tok)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)logits;
    size_t bytes = (size_t)n_tok * 256 * sizeof(float);
    for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628253ULL; }
    return h;
}

/* the SS-6 single-node forward hash for a tier (same seed 0x5A5A00+tier). */
static uint64_t tier_forward_hash(int tier)
{
    st_model m;
    if (st_init_tier(&m, 0x5A5A00u + (uint32_t)tier, tier) != ST_OK) return 0;
    warm_train(&m, 1);
    int n = (int)sizeof(CORPUS) - 1;
    int win = n < ST_MAXSEQ ? n : ST_MAXSEQ;
    float *logits = (float *)malloc((size_t)win * 256 * sizeof(float));
    uint64_t h = 0;
    if (logits) {
        st_set_remote_expert(NULL, NULL, NULL);  /* single-node oracle */
        st_forward(&m, CORPUS, win, logits);
        h = logit_hash(logits, win);
        free(logits);
    }
    st_free(&m);
    return h;
}

/* ---- profile injection (the fixture the cert drives) ---------------------- */
static void set_profile(unsigned long ram, unsigned cores, int ram_trust)
{
    char b[64];
    snprintf(b, sizeof b, "%lu", ram);     setenv("PKERNEL_DEVICE_RAM_BYTES", b, 1);
    snprintf(b, sizeof b, "%u",  cores);   setenv("PKERNEL_DEVICE_CORES", b, 1);
    snprintf(b, sizeof b, "%d",  ram_trust);setenv("PKERNEL_DEVICE_RAM_TRUST", b, 1);
    unsetenv("PKERNEL_DEVICE_TIER");        /* raw profile, not a forced tier   */
}

/* one [device-fit] row: inject profile -> probe -> tier_of -> st_init_device
 * -> assert m->tier == expected AND the arena allocated (no OOM). */
static void devfit_row(const char *label, unsigned long ram, unsigned cores,
                       int ram_trust, int expect, int machine)
{
    set_profile(ram, cores, ram_trust);

    struct dev_capacity c;
    dev_capacity_probe(&c);
    int t = tier_of(&c);

    st_model m;
    int rc = st_init_device(&m, 0xC0FFEEu);

    if (machine) {
        printf("DEVFIT %-22s ram=%-12lu cores=%u trust=%d -> tier_of=%s m->tier=%s rc=%d\n",
               label, ram, cores, ram_trust, TNAME[t],
               (rc == ST_OK) ? TNAME[m.tier] : "OOM", rc);
        if (rc == ST_OK) st_free(&m);
        return;
    }

    char msg[200];
    snprintf(msg, sizeof msg,
        "[device-fit] %-22s ram=%luB cores=%u -> tier_of=%s, student=%s, %s",
        label, ram, cores, TNAME[t],
        (rc == ST_OK) ? TNAME[m.tier] : "OOM",
        (rc == ST_OK) ? "arena fits" : "OOM!");
    /* tier_of picks the expected tier AND the student actually allocates at a
     * tier <= the picked one (step-down may reduce it; never OOM at the cert
     * RAM, which is real host RAM injected only as the THRESHOLD, the arena is
     * the small real one). */
    CHECK(t == expect && rc == ST_OK && m.tier == t, msg);
    if (rc == ST_OK) st_free(&m);
}

int main(int argc, char **argv)
{
    int machine = (argc > 1 && strcmp(argv[1], "--machine") == 0);

    /* computed per-tier arena costs + thresholds (the non-vacuity basis). */
    size_t aS = st_arena_bytes_for_tier(ST_TIER_S);
    size_t aM = st_arena_bytes_for_tier(ST_TIER_M);
    size_t aL = st_arena_bytes_for_tier(ST_TIER_L);
    size_t M_FIT = aM * DEVCAP_HEADROOM, L_FIT = aL * DEVCAP_HEADROOM;

    const unsigned long MB = 1024ul * 1024ul, GB = 1024ul * MB;

    if (machine) {
        devfit_row("8GB/8core",   8 * GB,   8, DEVCAP_TRUST_HIGH, ST_TIER_L, 1);
        devfit_row("2GB/4core",   2 * GB,   4, DEVCAP_TRUST_HIGH, ST_TIER_M, 1);
        devfit_row("512MB/2core", 512 * MB, 2, DEVCAP_TRUST_HIGH, ST_TIER_S, 1);
        devfit_row("512MB/8core", 512 * MB, 8, DEVCAP_TRUST_HIGH, ST_TIER_S, 1);
        devfit_row("lowtrust/2c", 99 * GB,  2, DEVCAP_TRUST_LOW,  ST_TIER_S, 1);
        printf("FWD S=%016llx M=%016llx L=%016llx\n",
               (unsigned long long)tier_forward_hash(ST_TIER_S),
               (unsigned long long)tier_forward_hash(ST_TIER_M),
               (unsigned long long)tier_forward_hash(ST_TIER_L));
        return 0;
    }

    printf("== student_devfit_test (DEVICE-CAPACITY mind-sizing, DEVFIT-1) ==\n\n");
    printf("computed per-tier arena cost (n_params*4*sizeof(float), the threshold basis):\n");
    printf("  S = %zu B (%.2f MB)   M = %zu B (%.2f MB)   L = %zu B (%.2f MB)\n",
           aS, aS / 1048576.0, aM, aM / 1048576.0, aL, aL / 1048576.0);
    printf("  HEADROOM = %u x  ->  M_FIT = %zu B (%.0f MB), L_FIT = %zu B (%.2f GB)\n\n",
           (unsigned)DEVCAP_HEADROOM, M_FIT, M_FIT / 1048576.0,
           L_FIT, L_FIT / 1073741824.0);

    /* ---- [device-fit]: same binary, injected profiles -> matching tier ---- */
    printf("[device-fit] same binary + injected device profiles -> matching tier:\n");
    devfit_row("8GB/8core (-> L)",   8 * GB,   8, DEVCAP_TRUST_HIGH, ST_TIER_L, 0);
    devfit_row("2GB/4core (-> M)",   2 * GB,   4, DEVCAP_TRUST_HIGH, ST_TIER_M, 0);
    devfit_row("512MB/2core (-> S)", 512 * MB, 2, DEVCAP_TRUST_HIGH, ST_TIER_S, 0);
    devfit_row("512MB/8core (RAM bottleneck wins -> S)",
                                     512 * MB, 8, DEVCAP_TRUST_HIGH, ST_TIER_S, 0);
    devfit_row("LOW-trust RAM + 2 cores (cores-only -> S)",
                                     99 * GB,  2, DEVCAP_TRUST_LOW,  ST_TIER_S, 0);
    devfit_row("LOW-trust RAM + 4 cores (cores-only -> M)",
                                     99 * GB,  4, DEVCAP_TRUST_LOW,  ST_TIER_M, 0);

    /* ---- [device-fit-monotone]: sweep RAM up -> tier non-decreasing -------- */
    printf("\n[device-fit-monotone] sweep RAM up at 8 cores -> tier_of non-decreasing:\n");
    {
        unsigned long ramv[] = { 64*MB, 256*MB, 512*MB, 1*GB, 2*GB, 4*GB, 8*GB, 16*GB, 64*GB };
        int prev = -1, mono = 1, last = -1;
        for (size_t i = 0; i < sizeof(ramv)/sizeof(ramv[0]); i++) {
            struct dev_capacity c = { 8, ramv[i], DEVCAP_TRUST_HIGH, DEVCAP_TRUST_HIGH };
            int t = tier_of(&c);
            if (t < prev) mono = 0;
            prev = t; last = t;
        }
        char msg[160];
        snprintf(msg, sizeof msg,
            "[device-fit-monotone] tier_of non-decreasing over the RAM sweep (saturates at L=%s)",
            TNAME[last]);
        CHECK(mono && last == ST_TIER_L, msg);
    }

    /* ---- [step-down]: a lying-high RAM number whose arena cannot allocate
     * degrades to a fitting mind (we cannot easily force malloc-fail in-proc, so
     * we assert the step-down LOGIC: st_init_device on the L profile returns OK
     * at L when L fits; the falsifier build (run_devfit.sh) proves the converse
     * — hardcoded L + no step-down OOMs the small profile). Here we assert the
     * production wrapper never returns OOM for any of the 3 valid tiers (the
     * arenas are small enough to fit on the CI host). --------------------- */
    printf("\n[step-down] production st_init_device never OOMs a valid profile:\n");
    {
        int ok = 1;
        set_profile(8 * GB, 8, DEVCAP_TRUST_HIGH);
        st_model mL; if (st_init_device(&mL, 1) != ST_OK) ok = 0; else st_free(&mL);
        set_profile(2 * GB, 4, DEVCAP_TRUST_HIGH);
        st_model mM; if (st_init_device(&mM, 1) != ST_OK) ok = 0; else st_free(&mM);
        set_profile(512 * MB, 2, DEVCAP_TRUST_HIGH);
        st_model mS; if (st_init_device(&mS, 1) != ST_OK) ok = 0; else st_free(&mS);
        CHECK(ok, "[step-down] st_init_device brings up a fitting student for S/M/L");
    }

    /* ---- [device-fit-ceiling]: SS-4 reconciliation (open-risk #7) --------- */
    printf("\n[device-fit-ceiling] RULE [device-ceiling-fleet-grows-within]:\n");
    printf("  device tier = local expert ceiling ST_E_<tier> (S=%d,M=%d,L=%d);\n",
           ST_E_S, ST_E_M, ST_E_L);
    printf("  SS-4 cap_experts_of(N) grows nexpert WITHIN it, clamped to ST_E_<tier>\n");
    printf("  NOT CAP_E_MAX(=16). e_target = min(cap_experts_of(N), ST_E_<tier>):\n");
    {
        /* a BIG fleet (cap_experts = 10, as cap_experts_of(10) would give) on
         * each device tier. The device ceiling must WIN. */
        int big = 10;
        int eS = st_fleet_expert_target(big, ST_TIER_S);
        int eM = st_fleet_expert_target(big, ST_TIER_M);
        int eL = st_fleet_expert_target(big, ST_TIER_L);
        char msg[200];
        snprintf(msg, sizeof msg,
            "[device-fit-ceiling] big fleet (cap_experts=%d): S->%d (<=ST_E_S=%d), "
            "M->%d (<=ST_E_M=%d), L->%d (<=ST_E_L=%d) — device ceiling wins, never 16",
            big, eS, ST_E_S, eM, ST_E_M, eL, ST_E_L);
        CHECK(eS == ST_E_S && eM == ST_E_M && eL == ST_E_L
              && eS == 2 && eM == 4 && eL == 8, msg);

        /* a SMALL fleet (cap_experts = 3) must NOT be forced up to the ceiling —
         * the fleet grows WITHIN; on M it stays 3, on S it clamps to 2. */
        int sm = 3;
        int gS = st_fleet_expert_target(sm, ST_TIER_S);  /* 3 clamped to 2     */
        int gM = st_fleet_expert_target(sm, ST_TIER_M);  /* 3 (< ceiling 4)    */
        int gL = st_fleet_expert_target(sm, ST_TIER_L);  /* 3 (< ceiling 8)    */
        snprintf(msg, sizeof msg,
            "[device-fit-ceiling] small fleet (cap_experts=%d): S->%d (ceiling 2), "
            "M->%d (grows within), L->%d (grows within) — fleet drives growth under the ceiling",
            sm, gS, gM, gL);
        CHECK(gS == 2 && gM == 3 && gL == 3, msg);

        /* never below K_min (ST_TOPK). */
        int floor1 = st_fleet_expert_target(1, ST_TIER_L);
        snprintf(msg, sizeof msg,
            "[device-fit-ceiling] floor: cap_experts=1 -> e_target=%d (>= K_min=%d), never undersizes firing width",
            floor1, ST_TOPK);
        CHECK(floor1 >= ST_TOPK, msg);
    }

    /* ---- [tier-forward-pin]: per-tier forward hash UNMOVED ----------------- */
    printf("\n[tier-forward-pin] per-tier student forward hash RE-PINNED post-SCALE-WALL-C1"
           " (RoPE + ST_MAXSEQ 64->256; == SS-6 single=):\n");
    {
        uint64_t hS = tier_forward_hash(ST_TIER_S);
        uint64_t hM = tier_forward_hash(ST_TIER_M);
        uint64_t hL = tier_forward_hash(ST_TIER_L);
        /* RE-PINNED after SCALE-WALL C1 (commit 7d497d7c: RoPE positional
         * encoding + ST_MAXSEQ 64->256, NS v2) intentionally moved st_forward
         * for EVERY tier. The prior pins (S=0a5bf44c.., M=63e8de33.., L=67f2434f..)
         * were the PRE-C1 forward; student_ss6_test's single= hashes WERE updated
         * to the post-C1 values but this cert's copies were missed -> the RED.
         * By this cert's OWN invariant ("SAME recipe as student_ss6_test, so the
         * pins match the SS-6 single= hashes bit-for-bit") the correct values ARE
         * the current GREEN run_ss6.sh single= hashes, restored here. */
        const uint64_t PIN_S = 0x6baf2e14f370de17ULL;  /* SS-6 S (post-C1) */
        const uint64_t PIN_M = 0x604895ba9c2b1c9dULL;  /* SS-6 M (post-C1) */
        const uint64_t PIN_L = 0x5ba2c68f6ca0ba89ULL;  /* SS-6 L (post-C1) */
        char msg[160];
        snprintf(msg, sizeof msg,
            "[tier-forward-pin] S=%016llx == 6baf2e14f370de17 (post-C1; == SS-6 single=)",
            (unsigned long long)hS);
        CHECK(hS == PIN_S, msg);
        snprintf(msg, sizeof msg,
            "[tier-forward-pin] M=%016llx == 604895ba9c2b1c9d (post-C1; == SS-6 single=)",
            (unsigned long long)hM);
        CHECK(hM == PIN_M, msg);
        snprintf(msg, sizeof msg,
            "[tier-forward-pin] L=%016llx == 5ba2c68f6ca0ba89 (post-C1; == SS-6 single=)",
            (unsigned long long)hL);
        CHECK(hL == PIN_L, msg);
    }

    printf("\nSUMMARY: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
