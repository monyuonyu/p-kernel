/*
 *  android/app/src/main/cpp/pkernel_jni.c
 *
 *  JNI bridge for UMP on Android. The native side wraps the existing
 *  Linux boot path so the same arch/linux/aarch64 + arch/common +
 *  kernel/common sources compile into libpkernel.so.
 *
 *  Java side (PKernel.kt or PKernel.java) calls nativeBoot() once,
 *  passing in a node_id; the kernel runs forever inside that thread.
 *  A second native method nativeWriteStdin() feeds shell input from
 *  the Activity's text field; kernel output is captured via a pipe
 *  whose read end the UI reads with nativeReadStdout() and renders
 *  to a TerminalView.
 *
 *  This file is intentionally minimal — the heavy lifting is in the
 *  existing arch/linux/aarch64 code, which works as-is on Bionic.
 */

#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Bionic ↔ glibc errno shim.
 * arch/linux/aarch64/{net_unix,net_relay,vfs_stub,…}.c declare an
 * extern `__errno_location()` (the glibc accessor) so they can read
 * errno without including <errno.h> — that header is shadowed by the
 * T-Kernel placeholder on our include path. Bionic exposes the same
 * thing as `__errno()`. Provide a one-line forwarder so the link
 * succeeds on Android without touching the kernel-side sources. */
extern volatile int *__errno(void);
int *__errno_location(void) { return (int *)__errno(); }

/* From boot/linux/main.c (renamed via -Dmain=pkernel_main for the
 * Android build so JNI's _start isn't shadowed). */
extern int pkernel_main(int argc, char **argv);

static int stdout_pipe[2] = { -1, -1 };
static int stdin_pipe[2]  = { -1, -1 };
static pthread_t kernel_thread;

static void *kernel_thread_main(void *arg)
{
    (void)arg;

    /* Re-route stdin to our pipe's read end, stdout to our pipe's
     * write end. After this dup2, every printf / sio_send_frame call
     * from the kernel reaches the UI via stdout_pipe[0]; every byte
     * the UI writes via nativeWriteStdin reaches the kernel via
     * stdin_pipe[1]. */
    dup2(stdin_pipe[0],  STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stdout_pipe[1], STDERR_FILENO);

    char *argv[] = { (char *)"p-kernel", NULL };
    pkernel_main(1, argv);
    return NULL;
}

/*
 *  nativeSetDataDir — persistence SLICE 1 (docs/architecture/persistence.md):
 *  point the durable p-fs store at the app's private files dir so the Self
 *  layer (profile + hash-chain lineage) AND the learned-mind weights (rw[],
 *  SLICE 2) survive a process death / reboot. Must be invoked BEFORE
 *  nativeBoot(): pfs_dur_dir() resolves $PKERNEL_PFS_DIR lazily at first use,
 *  which happens inside the kernel thread's boot path (pfs_durable_restore).
 *
 *  `dir` is the app's getFilesDir() (app-private, backup-excluded, no
 *  permission needed). We append "/ark" and the kernel mkdir()s it on first
 *  use (pfs_dur_dir). Pass null/empty to leave persistence OFF (memory-only,
 *  the pre-SLICE-1 Android behaviour).
 */
JNIEXPORT void JNICALL
Java_io_pkernel_PKernel_nativeSetDataDir(JNIEnv *env, jobject self,
                                         jstring jdir)
{
    (void)self;
    if (!jdir) { unsetenv("PKERNEL_PFS_DIR"); return; }
    const char *dir = (*env)->GetStringUTFChars(env, jdir, NULL);
    if (dir && *dir) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/ark", dir);
        setenv("PKERNEL_PFS_DIR", path, 1);
    } else {
        unsetenv("PKERNEL_PFS_DIR");
    }
    if (dir) (*env)->ReleaseStringUTFChars(env, jdir, dir);
}

JNIEXPORT void JNICALL
Java_io_pkernel_PKernel_nativeBoot(JNIEnv *env, jobject self, jint node_id)
{
    (void)env; (void)self;

    /* Set the node ID env var so net_unix.c picks it up. */
    char val[16]; snprintf(val, sizeof(val), "%d", (int)node_id);
    setenv("PKERNEL_NODE_ID", val, 1);

    /* Auto-bring-up the network on Android so the user doesn't have
     * to type `net` in the in-app terminal. */
    setenv("PKERNEL_AUTONET", "1", 1);

    /* Capture stdout/stderr / inject stdin via pipes. */
    if (stdout_pipe[0] < 0) pipe(stdout_pipe);
    if (stdin_pipe[0]  < 0) pipe(stdin_pipe);

    pthread_create(&kernel_thread, NULL, kernel_thread_main, NULL);
}

/*
 *  nativeConfigureRelay — wire the Phase B v2 relay transport into the
 *  upcoming nativeBoot() call by setting PKERNEL_RELAY_{HOST,PORT,KEY}
 *  before the kernel thread starts. Must be invoked BEFORE nativeBoot();
 *  net_dispatch.c reads these env vars at arch_linux_net_init() time
 *  (which happens in the kernel's usermain after the boot banner).
 *
 *  Args:
 *    host    — relay hostname or dotted-quad IPv4. Pass null/empty to
 *              clear, which leaves the dispatcher on loopback (net_unix).
 *    port    — relay UDP port (typical: 7400). Ignored if <= 0.
 *    keyHex  — 64-char hex (32-byte key). Pass null/empty to fall back
 *              to v1 wire (relay must be running with --insecure).
 */
JNIEXPORT void JNICALL
Java_io_pkernel_PKernel_nativeConfigureRelay(JNIEnv *env, jobject self,
                                              jstring jhost, jint jport,
                                              jstring jkey)
{
    (void)self;

    if (jhost) {
        const char *host = (*env)->GetStringUTFChars(env, jhost, NULL);
        if (host && *host) setenv("PKERNEL_RELAY_HOST", host, 1);
        else               unsetenv("PKERNEL_RELAY_HOST");
        (*env)->ReleaseStringUTFChars(env, jhost, host);
    } else {
        unsetenv("PKERNEL_RELAY_HOST");
    }

    if (jport > 0) {
        char val[16]; snprintf(val, sizeof(val), "%d", (int)jport);
        setenv("PKERNEL_RELAY_PORT", val, 1);
    }

    if (jkey) {
        const char *key = (*env)->GetStringUTFChars(env, jkey, NULL);
        if (key && *key) setenv("PKERNEL_RELAY_KEY", key, 1);
        else             unsetenv("PKERNEL_RELAY_KEY");
        (*env)->ReleaseStringUTFChars(env, jkey, key);
    } else {
        unsetenv("PKERNEL_RELAY_KEY");
    }
}

/*
 *  nativeConfigureLan — N-1b: wire the LAN-DIRECT transport (relay-free
 *  same-WiFi mesh) into the upcoming nativeBoot() by setting PKERNEL_LAN /
 *  PKERNEL_LAN_PORT before the kernel thread starts. Mirrors
 *  nativeConfigureRelay exactly: it ONLY sets env vars; net_dispatch.c reads
 *  them at arch_linux_net_init() time and selects net_lan.c when PKERNEL_LAN
 *  is set to a non-"0" value (the LAN check runs BEFORE the relay check, so an
 *  explicit LAN opt-in wins). Must be invoked BEFORE nativeBoot().
 *
 *  The shared PSK that net_lan authenticates with is PKERNEL_RELAY_KEY — the
 *  SAME env var the relay uses — so the LAN PSK is plumbed through
 *  nativeConfigureRelay's key path, NOT here. Two of the owner's phones mesh
 *  only when both carry the same key (blank key = v1 plaintext, trusted-LAN
 *  only). See net_lan.c.
 *
 *  Args:
 *    on    — true to opt into the LAN mesh (sets PKERNEL_LAN=1); false clears
 *            it (unsetenv), so the dispatcher falls through to relay/loopback.
 *    port  — UDP port net_lan binds + broadcasts on (default 7351). Ignored
 *            if <= 0 (net_lan then uses its built-in default).
 */
JNIEXPORT void JNICALL
Java_io_pkernel_PKernel_nativeConfigureLan(JNIEnv *env, jobject self,
                                           jboolean on, jint port)
{
    (void)env; (void)self;

    if (on) {
        setenv("PKERNEL_LAN", "1", 1);
        if (port > 0) {
            char val[16]; snprintf(val, sizeof(val), "%d", (int)port);
            setenv("PKERNEL_LAN_PORT", val, 1);
        }
    } else {
        unsetenv("PKERNEL_LAN");
    }
}

JNIEXPORT jint JNICALL
Java_io_pkernel_PKernel_nativeReadStdout(JNIEnv *env, jobject self,
                                          jbyteArray dst, jint maxlen)
{
    (void)self;
    if (stdout_pipe[0] < 0) return 0;
    jbyte *buf = (*env)->GetByteArrayElements(env, dst, NULL);
    ssize_t n = read(stdout_pipe[0], buf, (size_t)maxlen);
    (*env)->ReleaseByteArrayElements(env, dst, buf, 0);
    return (jint)(n > 0 ? n : 0);
}

JNIEXPORT jint JNICALL
Java_io_pkernel_PKernel_nativeWriteStdin(JNIEnv *env, jobject self,
                                          jbyteArray src, jint len)
{
    (void)self;
    if (stdin_pipe[1] < 0) return 0;
    jbyte *buf = (*env)->GetByteArrayElements(env, src, NULL);
    ssize_t n = write(stdin_pipe[1], buf, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, src, buf, JNI_ABORT);
    return (jint)(n > 0 ? n : 0);
}
