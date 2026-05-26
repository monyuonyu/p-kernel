/*
 * test_so_load_relay.c — host-side smoke that libpkernel.so loads AND
 * joins a Phase B v2 relay mesh.
 *
 * Sibling of test_so_load.c. Spawns ./relay (../../relay/relay) on a
 * non-default port with a fixed 32-byte key, then dlopens libpkernel.so
 * with PKERNEL_RELAY_{HOST,PORT,KEY}+AUTONET set. Success looks like:
 *
 *   [relay] node 1 registered: 127.0.0.1:XXXXX
 *
 * appearing in the relay's stderr, which proves the .so booted, ran
 * net_dispatch.c, picked the relay transport, and v2-authenticated a
 * REGISTER packet end-to-end. This is the artifact-level analogue of
 * what the Android app does at runtime — the same .so, the same env
 * surface, the same wire.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>

#define TEST_PORT "27460"
#define TEST_KEY  "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5" \
                  "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"

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
    /* 1. Spawn the relay child. */
    pid_t relay_pid = fork();
    if (relay_pid < 0) { perror("fork relay"); return 1; }
    if (relay_pid == 0) {
        setenv("PKERNEL_RELAY_KEY", TEST_KEY, 1);
        execl("../../relay/relay", "relay", "-p", TEST_PORT, "-v",
              (char *)NULL);
        perror("execl ./relay");
        _exit(127);
    }
    /* Give the relay a beat to bind. */
    usleep(400 * 1000);

    /* 2. Tell libpkernel where to find it. */
    setenv("PKERNEL_NODE_ID",    "1",         1);
    setenv("PKERNEL_AUTONET",    "1",         1);
    setenv("PKERNEL_RELAY_HOST", "127.0.0.1", 1);
    setenv("PKERNEL_RELAY_PORT", TEST_PORT,   1);
    setenv("PKERNEL_RELAY_KEY",  TEST_KEY,    1);

    /* 3. dlopen + dlsym + spawn kernel. */
    void *h = dlopen("./libpkernel.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        kill(relay_pid, SIGTERM); waitpid(relay_pid, NULL, 0);
        return 1;
    }
    pk_main = (pkernel_main_fn)dlsym(h, "pkernel_main");
    if (!pk_main) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        kill(relay_pid, SIGTERM); waitpid(relay_pid, NULL, 0);
        return 1;
    }
    fprintf(stderr, "[test-relay] libpkernel.so loaded, pkernel_main @ %p\n",
            (void *)pk_main);

    pthread_t t;
    pthread_create(&t, NULL, kernel_thread, NULL);

    /* 4. Let the kernel boot + autonet + REGISTER. The relay's stderr
     *    is interleaved with ours; watch for "node 1 registered". */
    sleep(3);

    fprintf(stderr, "[test-relay] sleep done — relay should have logged the REGISTER\n");

    kill(relay_pid, SIGTERM);
    waitpid(relay_pid, NULL, 0);
    return 0;
}
