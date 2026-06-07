/*
 * test_so_load.c — quick host-side check that libpkernel.so loads,
 * exports pkernel_main, and runs to the T-Kernel boot banner.
 *
 * Mimics what the Android JNI bridge does: dlopen + dlsym + spawn a
 * thread that pipes stdin/stdout. If this works on a normal Linux
 * box, the same .so should load fine under Android's Bionic.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    setenv("PKERNEL_NODE_ID", "1", 1);

    void *h = dlopen("./libpkernel.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    pk_main = (pkernel_main_fn)dlsym(h, "pkernel_main");
    if (!pk_main) { fprintf(stderr, "dlsym: %s\n", dlerror()); return 1; }

    fprintf(stderr, "[test] libpkernel.so loaded, pkernel_main @ %p\n", (void*)pk_main);

    pthread_t t;
    pthread_create(&t, NULL, kernel_thread, NULL);

    /* Let the kernel boot and print its banner. */
    sleep(2);

    fprintf(stderr, "[test] sleep done — kernel should have printed banner\n");
    return 0;
}
