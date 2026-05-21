/* arch/linux/x86_64/poc_ctx_switch.c
 *
 * Standalone proof of concept for the raw-asm context switching that
 * the arch/linux/ port is built on, on a System V AMD64 host. This is
 * the x86_64 sibling of arch/linux/aarch64/poc_ctx_switch.c — same
 * structure, just System V AMD64 callee-saved set instead of AAPCS64.
 *
 * Behaviour: two cooperative "tasks" alternate printing a shared
 * counter via arch_ctx_switch(). When the counter saturates, one of
 * them switches control back to main(), which then exits cleanly.
 *
 * What this proves:
 *   - struct arch_ctx is the right shape for x86_64 (7 callee-saved
 *     regs: rbx, rbp, r12-r15, rsp)
 *   - arch_ctx_switch preserves enough state that C functions resume
 *     correctly across switches on this ABI
 *   - mmap + mprotect yields a usable per-task stack with a guard page
 *
 * What this deliberately does NOT include — those land in Session 2:
 *   - A trampoline. If a task's entry returns, x86_64 `ret` pops [rsp]
 *     and jumps — but the initial setup put `entry` itself in that
 *     slot, so the return loops back into entry. PoC sidesteps by
 *     never returning from task entries.
 *   - Signal-based "interrupts" (SIGALRM as timer IRQ etc.).
 *   - IRQ-disable flag and pending-IRQ drain.
 *
 * Stack alignment note: System V AMD64 ABI says rsp must be ≡ 8
 * (mod 16) at function entry — i.e. the caller's rsp before the
 * `call` instruction is 16-aligned, and `call` pushes 8 bytes. We
 * synthesise the same condition with `ret`: our initial rsp must
 * be 16-aligned and point at the entry address slot. After `ret`
 * pops, rsp becomes (initial + 8) ≡ 8 (mod 16). ✓
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../include/arch_ctx.h"

static void *alloc_stack(size_t usable_bytes)
{
    long pagesz = sysconf(_SC_PAGESIZE);
    size_t total = usable_bytes + (size_t)pagesz;

    void *base = mmap(NULL, total,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                      -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(1); }
    if (mprotect(base, (size_t)pagesz, PROT_NONE) != 0) {
        perror("mprotect"); exit(1);
    }
    return (char *)base + total;   /* top of usable region */
}

/* Minimal initial-context setup.
 *
 * On x86_64 the "first switch in" is synthesised via `ret`, which
 * pops a return address from [rsp] and jumps to it. So:
 *   1. Round stack_top down to a 16-byte boundary.
 *   2. Reserve 16 bytes (only 8 are used for the entry address; the
 *      other 8 are slack so that initial rsp stays 16-aligned, which
 *      is what the ABI requires immediately before a `call`/`ret`).
 *   3. Write entry at that slot; set ctx->rsp = that slot.
 */
static void init_ctx(arch_ctx_t *ctx, void *stack_top_raw,
                     void (*entry)(void))
{
    memset(ctx, 0, sizeof(*ctx));
    unsigned long sp_top = (unsigned long)stack_top_raw & ~0xFUL;
    unsigned long sp     = sp_top - 16;        /* 16-aligned slot */
    *(unsigned long *)sp = (unsigned long)entry;
    ctx->rsp = sp;
}

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

    printf("[main] host=x86_64-linux, stack_a top=%p, stack_b top=%p\n",
           stack_a, stack_b);

    init_ctx(&ctx_a, stack_a, task_a);
    init_ctx(&ctx_b, stack_b, task_b);

    printf("[main] entering task A\n");
    fflush(stdout);
    arch_ctx_switch(&main_ctx, &ctx_a);

    printf("[main] back in main — PoC complete\n");
    return 0;
}
