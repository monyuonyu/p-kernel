/*
 * PKernel.java — Java/Kotlin-callable surface for the UMP native library.
 *
 * Loads libpkernel.so once at class initialisation, then exposes the
 * methods that mirror the JNI bridge in pkernel_jni.c:
 *
 *   PKernel.boot(nodeId)             — launches the kernel thread (loopback).
 *   PKernel.bootWithRelay(...)       — same, joining a Phase B v2 relay mesh.
 *   PKernel.configureRelay(...)      — set relay env vars without booting.
 *   PKernel.readStdout(dst, maxlen)  — pulls kernel output bytes.
 *   PKernel.writeStdin(src, len)     — pushes shell input bytes.
 *
 * The Activity drives readStdout in a loop on its UI/IO thread and
 * appends decoded text to a TerminalView; the on-screen keyboard
 * routes its byte stream into writeStdin.
 */
package io.pkernel;

public final class PKernel {

    static {
        System.loadLibrary("pkernel");
    }

    public native void   nativeBoot(int nodeId);
    public native void   nativeSetDataDir(String dir);
    public native void   nativeConfigureRelay(String host, int port, String keyHex);
    public native void   nativeConfigureLan(boolean on, int port);
    public native int    nativeReadStdout(byte[] dst, int maxlen);
    public native int    nativeWriteStdin(byte[] src, int len);

    /**
     * Point the durable p-fs store at a directory (persistence SLICE 1/2).
     * Pass the app's getFilesDir().getAbsolutePath(); the native side
     * appends "/ark". Must be called BEFORE boot()/bootWithRelay() so the
     * kernel's boot-time restore path sees PKERNEL_PFS_DIR. Without this the
     * node is memory-only (identity + learned mind evaporate on restart).
     */
    public void setDataDir(String dir) { nativeSetDataDir(dir); }

    /** Boot the kernel using the loopback transport (no relay). */
    public void boot(int nodeId) { nativeBoot(nodeId); }

    /**
     * Boot the kernel joining a Phase B v2 relay mesh.
     *
     * @param nodeId    1..255, must be unique within the mesh
     * @param relayHost hostname or IPv4 dotted-quad
     * @param relayPort UDP port (typical: 7400)
     * @param relayKey  32-byte key as 64 hex chars, or null for v1 wire
     *                  (only acceptable to a relay started with --insecure)
     */
    public void bootWithRelay(int nodeId, String relayHost, int relayPort,
                              String relayKey) {
        nativeConfigureRelay(relayHost, relayPort, relayKey);
        nativeBoot(nodeId);
    }

    /** Set relay env vars without booting (e.g. for staged init). */
    public void configureRelay(String host, int port, String keyHex) {
        nativeConfigureRelay(host, port, keyHex);
    }

    /**
     * N-1b: opt into the LAN-DIRECT transport (relay-free same-WiFi mesh)
     * without booting. Must be called BEFORE boot(); net_dispatch selects
     * net_lan when PKERNEL_LAN=1, ahead of the relay/loopback paths.
     *
     * The shared PSK is the EXISTING relay key (PKERNEL_RELAY_KEY): set it via
     * configureRelay(host, port, keyHex) — even with an empty host — so two of
     * the owner's own phones authenticate with the SAME key. Both phones must
     * carry the same key; a blank key falls back to v1 plaintext (trusted-LAN
     * only). The MulticastLock must be held by the caller for inbound
     * broadcast to be delivered (PKernelService does this while LAN is ON).
     *
     * @param on   true to join the LAN mesh; false to clear the opt-in
     * @param port UDP port (default 7351 when <= 0)
     */
    public void configureLan(boolean on, int port) {
        nativeConfigureLan(on, port);
    }

    public int readStdout(byte[] dst, int maxlen) { return nativeReadStdout(dst, maxlen); }
    public int writeStdin(byte[] src, int len)    { return nativeWriteStdin(src, len); }

    /* --- GPU (GPU-1/GPU-2 Vulkan compute backend) UI surface ----------------
     *
     * These are STATIC: the GPU backend's state (availability, the runtime
     * enable flag, the picked device name) is process-global in gpu_vk.c, so
     * no PKernel instance is needed. The settings toggle and the engineer page
     * read them via the Gpu object below. They are crash-free at any time,
     * including on a device with no usable Vulkan (then available()==false).
     *
     * IMPORTANT: this only surfaces availability + the enable flag. It does NOT
     * route any inference through the GPU — the mind still computes on the CPU
     * (GPU-3 wires the matmul to the GPU). The engineer page says so honestly.
     */
    public static native boolean nativeGpuAvailable();
    public static native boolean nativeGpuCapable();
    public static native String  nativeGpuName();
    public static native boolean nativeGpuGetEnabled();
    public static native void    nativeGpuSetEnabled(boolean enabled);

    /**
     * Gpu — the Kotlin/Java-friendly facade over the GPU natives. Ensures
     * libpkernel.so is loaded (the PKernel static initialiser does it) before
     * any GPU call. Pure read/flag surface; never touches inference.
     */
    public static final class Gpu {
        private Gpu() {}

        /** True iff a usable Vulkan compute device is live RIGHT NOW, i.e.
         *  capable AND the enable flag is ON. This is the "in use" query. */
        public static boolean available() { return nativeGpuAvailable(); }

        /** True iff this device HAS a usable Vulkan compute device, REGARDLESS
         *  of the enable flag — the CAPABILITY query. The settings toggle greys
         *  itself on !capable() (not !available()), so a capable device with the
         *  flag OFF still shows a working, switchable toggle. Without this the
         *  flag-gated available() would deadlock the UI (greyed because OFF). */
        public static boolean capable() { return nativeGpuCapable(); }

        /** The picked GPU's device name (e.g. "Adreno (TM) 840"), or "" if
         *  no GPU / not yet initialised. Never null. */
        public static String name() { return nativeGpuName(); }

        /** The runtime enable flag (the settings toggle drives it). */
        public static boolean isEnabled() { return nativeGpuGetEnabled(); }

        /** Flip the runtime enable flag. Persisting it to prefs is the
         *  caller's job (the settings screen does both). */
        public static void setEnabled(boolean enabled) { nativeGpuSetEnabled(enabled); }
    }
}
