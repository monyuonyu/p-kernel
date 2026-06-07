/*
 * so_node.c — minimal launcher that runs ONE libpkernel.so node as its
 * own process, the way the Android app's nativeBoot() does.
 *
 * Unlike test_so_load_relay.c (which also forks a relay), this is a pure
 * node: it reads PKERNEL_* from the environment, dlopens ./libpkernel.so,
 * runs pkernel_main on a thread, and then just copies the calling
 * process's stdin/stdout straight through (the kernel dup2's nothing —
 * it inherits our FD 0/1/2). Run N of these as separate processes to
 * build a real multi-node mesh out of the SAME .so artifact the APK ships.
 *
 *   PKERNEL_NODE_ID=2 PKERNEL_AUTONET=1 PKERNEL_RELAY_* ./so_node
 *
 * Each kernel instance lives in its own address space, so the global
 * kernel state doesn't collide — exactly the per-phone model of Phase D.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

typedef int (*pkernel_main_fn)(int, char **);
static pkernel_main_fn pk_main;

static void *kernel_thread(void *arg)
{
    (void)arg;
    char *argv[] = { "libpkernel", NULL };
    pk_main(1, argv);
    return NULL;
}

int main(void)
{
    void *h = dlopen("./libpkernel.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "[so_node] dlopen: %s\n", dlerror()); return 1; }
    pk_main = (pkernel_main_fn)dlsym(h, "pkernel_main");
    if (!pk_main) { fprintf(stderr, "[so_node] dlsym: %s\n", dlerror()); return 1; }

    /* Run the kernel on a thread and join it — the kernel runs forever
     * (its shell reads our inherited stdin), so this effectively blocks
     * until the kernel exits or we're killed. */
    pthread_t t;
    pthread_create(&t, NULL, kernel_thread, NULL);
    pthread_join(t, NULL);
    return 0;
}
