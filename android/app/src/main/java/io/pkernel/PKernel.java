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
    public native void   nativeConfigureRelay(String host, int port, String keyHex);
    public native int    nativeReadStdout(byte[] dst, int maxlen);
    public native int    nativeWriteStdin(byte[] src, int len);

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

    public int readStdout(byte[] dst, int maxlen) { return nativeReadStdout(dst, maxlen); }
    public int writeStdin(byte[] src, int len)    { return nativeWriteStdin(src, len); }
}
