/* arch/linux/aarch64/poc_ctx_switch.c
 *
 * Standalone proof of concept for the raw-asm context switching that
 * the planned arch/linux/ port is built on. No T-Kernel involvement;
 * just a tight verification loop for components 1, 2, and 5 of the
 * design captured in memory/project_linux_userspace_port.md.
 *
 * Behaviour: two cooperative "tasks" alternate printing a shared
 * counter via arch_ctx_switch(). When the counter saturates, one of
 * them switches control back to main(), which then exits cleanly.
 *
 * What this proves:
 *   - struct arch_ctx (component 1) is the right shape on AArch64
 *   - arch_ctx_switch (component 2) preserves enough state that C
 *     functions resume correctly across switches
 *   - mmap + mprotect (component 5) yields usable per-task stacks
 *     with a guard page below them
 *
 * What this deliberately does NOT include — those land in Session 2:
 *   - A trampoline. If a task's entry returns, its function epilogue
 *     restores x30 from the stack — but the prologue had saved the
 *     value of x30 at entry, which was the entry function itself. The
 *     return therefore re-enters the entry function. The PoC sidesteps
 *     the issue by never returning from task entries.
 *   - Signal-based "interrupts" (SIGALRM as timer IRQ etc.).
 *   - IRQ-disable flag and pending-IRQ drain.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../include/arch_ctx.h"

/* --------------------------------------------------------------------
 * Component 5: per-task stack with a guard page just below it.
 * mmap returns page-aligned memory; we make the first page non-readable
 * so a stack overflow trips an immediate SIGSEGV instead of silently
 * corrupting adjacent allocations.
 * -------------------------------------------------------------------- */
static void *alloc_stack(size_t usable_bytes)
{
    long pagesz = sysconf(_SC_PAGESIZE);
    size_t total = usable_bytes + (size_t)pagesz;

    void *base = mmap(NULL, total,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                      -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    if (mprotect(base, (size_t)pagesz, PROT_NONE) != 0) {
        perror("mprotect");
        exit(1);
    }
    return (char *)base + total;   /* top of usable region */
}

/* --------------------------------------------------------------------
 * Minimal initial-context setup. AAPCS64 says sp must be 16-byte
 * aligned at function entry; mmap'd pages already are, but we mask
 * defensively in case future callers pass non-aligned tops.
 * -------------------------------------------------------------------- */
static void init_ctx(arch_ctx_t *ctx, void *stack_top_raw,
                     void (*entry)(void))
{
    memset(ctx, 0, sizeof(*ctx));
    unsigned long sp = (unsigned long)stack_top_raw & ~0xFUL;
    ctx->sp  = sp;
    ctx->x30 = (unsigned long)entry;
}

/* --------------------------------------------------------------------
 * The PoC itself: two tasks plus the main context to fall back to.
 * -------------------------------------------------------------------- */
static arch_ctx_t main_ctx, ctx_a, ctx_b;
static int counter;

static void task_a(void)
{
    while (counter < 5) {
        printf("[A] counter=%d\n", counter++);
        fflush(stdout);
        arch_ctx_switch(&ctx_a, &ctx_b);
    }
    printf("[A] limit reached, yielding to main\n");
    fflush(stdout);
    arch_ctx_switch(&ctx_a, &main_ctx);
}

static void task_b(void)
{
    while (counter < 5) {
        printf("[B] counter=%d\n", counter++);
        fflush(stdout);
        arch_ctx_switch(&ctx_b, &ctx_a);
    }
    printf("[B] limit reached, yielding to main\n");
    fflush(stdout);
    arch_ctx_switch(&ctx_b, &main_ctx);
}

int main(void)
{
    const size_t stack_size = 16 * 4096;
    void *stack_a = alloc_stack(stack_size);
    void *stack_b = alloc_stack(stack_size);

    printf("[main] host=aarch64-linux, stack_a top=%p, stack_b top=%p\n",
           stack_a, stack_b);

    init_ctx(&ctx_a, stack_a, task_a);
    init_ctx(&ctx_b, stack_b, task_b);

    printf("[main] entering task A\n");
    fflush(stdout);
    arch_ctx_switch(&main_ctx, &ctx_a);

    printf("[main] back in main — PoC complete\n");
    return 0;
}
