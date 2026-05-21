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
