/*
 * gpu_compute.h — GPU-1 Vulkan compute backend (Android/host-only).
 *
 * Thin C ABI over a Vulkan compute matmul. NO Vulkan types leak across this
 * boundary — callers (the libc-free inference engine, the device-test harness,
 * the JNI bridge) see only plain C. This mirrors the design doc §3.2 contract.
 *
 * THE COMPATIBILITY CONTRACT (design doc §0, §3.5):
 *   - GPU is a PURELY OPTIONAL accelerator. The CPU path (qz_matmul / a plain
 *     CPU matmul) is the permanent reference AND fallback.
 *   - gpu_available() tells the caller whether a usable compute device exists.
 *     If it returns 0, the caller MUST use the CPU path.
 *   - gpu_matmul() returns <0 on ANY problem (no device, OOM, disabled,
 *     unavailable). It NEVER crashes and NEVER requires the GPU. <0 means
 *     "slow, use CPU" — never "broken".
 *   - libvulkan.so is loaded via dlopen at runtime (see gpu_vk.c). It is NOT a
 *     DT_NEEDED of libpkernel.so, so a device WITHOUT Vulkan still loads the
 *     .so and runs entirely on the CPU.
 *
 * Lifecycle: gpu_init() once (idempotent), then gpu_matmul() many times, then
 * gpu_shutdown() at teardown. gpu_available()/gpu_set_enabled() are cheap and
 * safe to call any time, including before gpu_init().
 */
#ifndef PK_GPU_COMPUTE_H
#define PK_GPU_COMPUTE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The .so is built with -fvisibility=hidden; mark the GPU API default so a
 * future JNI bridge / engineer-page readout (and the device-test exe) can
 * reach these symbols, and so they aren't dead-stripped from the library. */
#define PK_GPU_API __attribute__((visibility("default")))

/*
 * Runtime enable flag (the future settings toggle flips this).
 *   enabled != 0  -> GPU may be used (still gated by availability + size).
 *   enabled == 0  -> GPU is OFF; every path falls back to CPU.
 * Conservative default: DISABLED until explicitly turned on. Safe to call
 * before gpu_init(). Returns the previous value.
 */
PK_GPU_API int  gpu_set_enabled(int enabled);
PK_GPU_API int  gpu_get_enabled(void);

/*
 * Create the Vulkan instance/device/queue/pipeline ONCE. Idempotent: calling
 * again is a no-op that returns the cached result. Returns 0 if a usable
 * compute backend is live, <0 otherwise (caller stays on CPU). NEVER crashes:
 * a missing libvulkan.so, no instance, or no compute device all return <0.
 */
PK_GPU_API int  gpu_init(void);

/*
 * 1 iff the GPU path is usable RIGHT NOW: enabled AND a Vulkan compute device
 * + pipeline are live. 0 means the caller MUST use the CPU path. This is the
 * single gate the inference engine checks before considering gpu_matmul().
 * Calling it lazily triggers gpu_init() the first time.
 */
PK_GPU_API int  gpu_available(void);

/*
 * 1 iff this device HAS a usable Vulkan compute device + pipeline, REGARDLESS
 * of the enable flag. This is the CAPABILITY query (does the hardware exist?),
 * distinct from gpu_available() which is the IN-USE query (capable AND switched
 * on). The settings toggle greys itself on !gpu_capable() (not !gpu_available),
 * so a capable device shows a working toggle that defaults OFF but CAN be
 * turned on — without this the flag-gated gpu_available() deadlocks the UI
 * (greyed because OFF, can't turn ON because greyed). Probing here does NOT
 * enable the GPU and does NOT change the flag. Lazily triggers gpu_init() the
 * first time; crash-free on a Vulkan-less device (returns 0). Also populates
 * gpu_name() on success. Always safe to call.
 */
PK_GPU_API int  gpu_capable(void);

/*
 * Plain-float matmul on the GPU:  y[i] = sum_j A[i*in + j] * x[j],
 * for i in [0, out).  A is row-major (out rows x in cols), x has length `in`,
 * y has length `out`. This is the GPU-1 reference op (dequant arrives in
 * GPU-2). Contract mirrors a CPU matmul exactly so the [gpu-matmul-matches-cpu]
 * cert can diff GPU vs CPU on identical inputs.
 *
 * Returns 0 on success (y filled from the GPU), <0 on ANY failure — in which
 * case y is untouched and the caller MUST fall back to the CPU. Returns <0
 * (without touching the GPU) when disabled or unavailable, so it is always
 * safe to call.
 */
PK_GPU_API int  gpu_matmul_f32(const float *A, size_t in, size_t out,
                    const float *x, float *y);

/*
 * ----------------------------------------------------------------------------
 * RESIDENT WEIGHTS (GPU-2): upload A to a GPU SSBO ONCE, reuse across calls.
 * ----------------------------------------------------------------------------
 * gpu_matmul_f32 re-uploads the whole weight matrix A on EVERY call. For the
 * inference path that dominates: the SAME weights are multiplied by a new x
 * for every token. These two calls keep A resident on the GPU so only x is
 * uploaded and y read back per matmul — the realistic inference pattern.
 *
 * gpu_upload_weight(A, in, out): copies A (out rows x in cols, row-major) into
 *   a GPU-resident storage buffer ONCE. Returns an opaque non-NULL handle on
 *   success, or NULL on ANY failure (no GPU, disabled, OOM, bad args) — in
 *   which case the caller MUST use the CPU path for these weights. The handle
 *   stays valid until gpu_free_weight() (or gpu_shutdown()).
 *
 * gpu_matmul_resident(h, x, y): y[i] = sum_j A[i*in+j] * x[j], reusing the
 *   resident A behind handle h (the in/out are remembered from upload). Only x
 *   is uploaded and y read back. Returns 0 on success (y filled), <0 on ANY
 *   failure (y untouched -> caller falls back to CPU). Never crashes.
 *
 * gpu_free_weight(h): release the resident buffer. NULL-safe, idempotent.
 *
 * All three are safe to call any time; if the GPU is unavailable/disabled
 * gpu_upload_weight returns NULL and the caller simply stays on the CPU.
 */
typedef struct gpu_weight *gpu_weight_t;
PK_GPU_API gpu_weight_t gpu_upload_weight(const float *A, size_t in, size_t out);
PK_GPU_API int          gpu_matmul_resident(gpu_weight_t h,
                                            const float *x, float *y);
PK_GPU_API void         gpu_free_weight(gpu_weight_t h);

/*
 * The picked GPU's device name (e.g. "Adreno (TM) 840"), read from the Vulkan
 * physical-device properties at gpu_init(). Returns a pointer to an internal,
 * always-NUL-terminated static string; never NULL. Before a successful init
 * (or with no GPU) it returns "" (empty). For the status/toggle UI. Always
 * safe to call.
 */
PK_GPU_API const char *gpu_name(void);

/* Destroy the Vulkan objects and dlclose libvulkan. Idempotent. */
PK_GPU_API void gpu_shutdown(void);

/*
 * Human-readable one-liner about the backend state (device name or the reason
 * it is unavailable). Writes a NUL-terminated string into buf (never
 * overflows). For the engineer-page / harness readout. Always safe.
 */
PK_GPU_API void gpu_describe(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* PK_GPU_COMPUTE_H */
