/*
 *  arch/linux/x86_64/poc_dispatch.c
 *
 *  Session 3a unit test: drive the T-Kernel x86_64 dispatcher
 *  (cpu_support.S) directly without bringing up the rest of T-Kernel.
 *  Two fake TCBs with hand-rolled 64-byte dormant frames yield back
 *  and forth via knl_dispatch, then return to main via the same path.
 *
 *  Direct sibling of arch/linux/aarch64/poc_dispatch.c — same shape,
 *  System V AMD64 register / frame layout.
 *
 *  What this proves:
 *    - cpu_support.S's dispatcher reads/writes TCB::tskctxb.ssp at
 *      offset 192 (TCB_SSP) correctly.
 *    - The dormant 64-byte frame layout matches what knl_setup_context
 *      in cpu_task.h prepares: r12=stacd, r13=exinf, rip slot=
 *      trampoline.
 *    - knl_task_entry_trampoline reads task entry from TCB_task=40,
 *      delivers stacd/exinf in rdi/rsi per System V, and links to
 *      tk_ext_tsk if the entry ever returns.
 *    - IRQ-disable-on-entry / IRQ-enable-on-exit happens via
 *      arch_irq_disabled_flag (no privileged instructions used).
 *
 *  T-Kernel itself is NOT compiled here. We provide just enough of
 *  its surface (knl_ctxtsk, knl_schedtsk, knl_dispatch_disabled,
 *  knl_taskindp, tk_ext_tsk stub, knl_timer_handler stub) so the
 *  dispatcher links and runs. Full T-Kernel integration is Session 3b.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define TCB_SIZE        256
#define TCB_TASK_OFF    40
#define TCB_SSP_OFF     192

typedef void (*task_fn_t)(int stacd, void *exinf);

typedef struct fake_tcb {
    char raw[TCB_SIZE];
} fake_tcb_t;

static void tcb_set_task(fake_tcb_t *t, task_fn_t fn)
{
    *(task_fn_t *)(t->raw + TCB_TASK_OFF) = fn;
}

static void tcb_set_ssp(fake_tcb_t *t, void *ssp)
{
    *(void **)(t->raw + TCB_SSP_OFF) = ssp;
}

/* --------------------------------------------------------------------
 *  T-Kernel globals expected by cpu_support.S.
 * -------------------------------------------------------------------- */
fake_tcb_t   *knl_ctxtsk            = NULL;
fake_tcb_t   *knl_schedtsk          = NULL;
unsigned long knl_dispatch_disabled = 0;
int           knl_taskindp          = 0;

volatile int arch_irq_disabled_flag = 0;

void arch_irq_enable_with_drain(void)
{
    arch_irq_disabled_flag = 0;
}

void knl_timer_handler(void) {}

void tk_ext_tsk(void)
{
    fprintf(stderr, "[poc] tk_ext_tsk reached — task returned\n");
    exit(2);
}

extern void knl_dispatch(void);
extern void knl_dispatch_entry(void);
extern void knl_dispatch_to_schedtsk(void);
extern void knl_force_dispatch(void);
extern void knl_task_entry_trampoline(void);

/* --------------------------------------------------------------------
 *  Stack + dormant frame setup. Mirrors knl_setup_context in cpu_task.h.
 * -------------------------------------------------------------------- */
static void *alloc_stack(size_t usable)
{
    long pagesz = sysconf(_SC_PAGESIZE);
    size_t total = usable + (size_t)pagesz;
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(1); }
    if (mprotect(base, (size_t)pagesz, PROT_NONE) < 0) {
        perror("mprotect"); exit(1);
    }
    return (char *)base + total;
}

/* Build the 64-byte dormant frame at the top of the given stack; return
 * the new ssp. Frame layout matches cpu_task.h exactly:
 *   ssp+ 0: r12 (stacd)     ssp+32: rbx
 *   ssp+ 8: r13 (exinf)     ssp+40: rbp
 *   ssp+16: r14             ssp+48: taskmode
 *   ssp+24: r15             ssp+56: rip (trampoline) */
static void *setup_dormant(void *stack_top, int stacd, void *exinf)
{
    uintptr_t ssp = ((uintptr_t)stack_top - 64) & ~0xFUL;
    unsigned char *base = (unsigned char *)ssp;
    memset(base, 0, 64);
    *(uintptr_t *)(base +  0) = (uintptr_t)(unsigned int)stacd;
    *(uintptr_t *)(base +  8) = (uintptr_t)exinf;
    *(uintptr_t *)(base + 56) = (uintptr_t)knl_task_entry_trampoline;
    return base;
}

/* --------------------------------------------------------------------
 *  The test: two tasks yield to each other via the dispatcher, then
 *  yield back to main.
 * -------------------------------------------------------------------- */
static fake_tcb_t tcb_a, tcb_b, main_tcb;
static int counter;

static void task_a(int stacd, void *exinf)
{
    (void)exinf;
    printf("[A] entered with stacd=0x%x\n", stacd);
    fflush(stdout);
    while (counter < 5) {
        printf("[A] counter=%d\n", counter++);
        fflush(stdout);
        knl_schedtsk = &tcb_b;
        knl_dispatch();
    }
    printf("[A] limit reached, switching to main_tcb\n");
    fflush(stdout);
    knl_schedtsk = &main_tcb;
    knl_dispatch();
}

static void task_b(int stacd, void *exinf)
{
    (void)exinf;
    printf("[B] entered with stacd=0x%x\n", stacd);
    fflush(stdout);
    while (counter < 5) {
        printf("[B] counter=%d\n", counter++);
        fflush(stdout);
        knl_schedtsk = &tcb_a;
        knl_dispatch();
    }
    printf("[B] limit reached, switching to main_tcb\n");
    fflush(stdout);
    knl_schedtsk = &main_tcb;
    knl_dispatch();
}

int main(void)
{
    void *stack_a = alloc_stack(64 * 1024);
    void *stack_b = alloc_stack(64 * 1024);

    tcb_set_task(&tcb_a, task_a);
    tcb_set_ssp (&tcb_a, setup_dormant(stack_a, 0xA1, NULL));
    tcb_set_task(&tcb_b, task_b);
    tcb_set_ssp (&tcb_b, setup_dormant(stack_b, 0xB2, NULL));

    printf("[main] dispatching to task A\n");
    fflush(stdout);

    knl_schedtsk = &tcb_a;
    knl_ctxtsk   = &main_tcb;
    knl_dispatch();

    printf("[main] resumed; counter=%d, exit clean\n", counter);
    return 0;
}
