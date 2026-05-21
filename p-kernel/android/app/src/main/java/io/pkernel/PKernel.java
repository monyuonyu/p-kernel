/*
 * PKernel.java — Java/Kotlin-callable surface for the UMP native library.
 *
 * Loads libpkernel.so once at class initialisation, then exposes three
 * methods that mirror the JNI bridge in pkernel_jni.c:
 *
 *   PKernel.boot(nodeId)            — launches the kernel thread.
 *   PKernel.readStdout(dst, maxlen) — pulls kernel output bytes.
 *   PKernel.writeStdin(src, len)    — pushes shell input bytes.
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
    public native int    nativeReadStdout(byte[] dst, int maxlen);
    public native int    nativeWriteStdin(byte[] src, int len);

    public void boot(int nodeId)                    { nativeBoot(nodeId); }
    public int  readStdout(byte[] dst, int maxlen)  { return nativeReadStdout(dst, maxlen); }
    public int  writeStdin(byte[] src, int len)     { return nativeWriteStdin(src, len); }
}
