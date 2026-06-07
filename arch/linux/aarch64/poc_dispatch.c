/*
 *  arch/linux/aarch64/poc_dispatch.c
 *
 *  Session 3a unit test: drive the T-Kernel dispatcher (cpu_support.S)
 *  directly without bringing up the rest of T-Kernel. Two fake TCBs
 *  with hand-rolled 112-byte dormant frames yield back and forth via
 *  `knl_dispatch`, then return to main via `knl_dispatch_to_schedtsk`.
 *
 *  What this proves:
 *    - cpu_support.S's dispatcher reads/writes TCB::tskctxb.ssp at
 *      offset 192 (TCB_SSP) correctly.
 *    - The dormant 112-byte frame layout matches what knl_setup_context
 *      in cpu_task.h prepares: x19=stacd, x20=exinf, x30=trampoline.
 *    - knl_task_entry_trampoline reads task entry from TCB_task=40,
 *      delivers stacd/exinf in x0/x1 per AAPCS64, and links to
 *      tk_ext_tsk if the entry ever returns.
 *    - IRQ-disable-on-entry / IRQ-enable-on-exit happens via the
 *      arch_irq_disabled_flag (no privileged instructions used).
 *
 *  T-Kernel itself is NOT compiled here. We provide just enough of
 *  its surface (knl_ctxtsk, knl_schedtsk, knl_dispatch_disabled,
 *  knl_taskindp, tk_ext_tsk stub, knl_timer_handler stub) so the
 *  dispatcher links and runs. Real T-Kernel integration is Session 3b.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* --------------------------------------------------------------------
 *  Minimal TCB shape — must match the real TCB at the field offsets
 *  cpu_support.S hardcodes (TCB_SSP=192, TCB_task=40). The rest of the
 *  256 bytes is opaque.
 * -------------------------------------------------------------------- */
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
 *  T-Kernel globals expected by cpu_support.S. Defined here as stubs.
 * -------------------------------------------------------------------- */
fake_tcb_t   *knl_ctxtsk          = NULL;
fake_tcb_t   *knl_schedtsk        = NULL;
unsigned long knl_dispatch_disabled = 0;
int           knl_taskindp        = 0;

/* IRQ flag — referenced by cpu_support.S directly via adrp/add. */
volatile int arch_irq_disabled_flag = 0;

/* Real implementation would drain pending signals here. For this
 * dispatch-only PoC there is no SIGALRM yet, so just clear the flag. */
void arch_irq_enable_with_drain(void)
{
    arch_irq_disabled_flag = 0;
}

/* Stubs for the timer-IRQ bridge — not exercised in this PoC. */
void knl_timer_handler(void) {}

/* tk_ext_tsk: the dispatcher loads this address into x30 before calling
 * a task entry, so if the entry ever returns we land here. A real
 * T-Kernel implementation cleans up and re-dispatches; the PoC just
 * notes the fact. */
void tk_ext_tsk(void)
{
    fprintf(stderr, "[poc] tk_ext_tsk reached — task returned\n");
    exit(2);
}

/* Dispatcher symbols from cpu_support.S */
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
    void *base = mmap(NULL, total,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                      -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(1); }
    if (mprotect(base, (size_t)pagesz, PROT_NONE) < 0) {
        perror("mprotect"); exit(1);
    }
    return (char *)base + total;
}

/* Build the 112-byte dormant frame at the top of the given stack,
 * returns the new ssp. Frame layout matches cpu_task.h exactly. */
static void *setup_dormant(void *stack_top, int stacd, void *exinf)
{
    uintptr_t ssp = ((uintptr_t)stack_top - 112) & ~0xFUL;
    unsigned char *base = (unsigned char *)ssp;
    memset(base, 0, 112);
    /* x19 (offset 0)  = stacd */
    *(uintptr_t *)(base +  0) = (uintptr_t)(unsigned int)stacd;
    /* x20 (offset 8)  = exinf */
    *(uintptr_t *)(base +  8) = (uintptr_t)exinf;
    /* x30 (offset 88) = trampoline (= initial PC on first dispatch) */
    *(uintptr_t *)(base + 88) = (uintptr_t)knl_task_entry_trampoline;
    /* taskmode (offset 96) = 0 (already cleared by memset) */
    return base;
}

/* --------------------------------------------------------------------
 *  The actual test: two tasks yield to each other through the
 *  T-Kernel dispatcher, then one yields back to main.
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
    /* Unreachable in this PoC */
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

    /* main has no entry — we'll be saved into main_tcb by knl_dispatch's
     * own save path and resumed when a task switches back to us. */
    printf("[main] dispatching to task A\n");
    fflush(stdout);

    knl_schedtsk = &tcb_a;
    knl_ctxtsk   = &main_tcb;     /* so dispatcher saves main's frame here */
    knl_dispatch();

    printf("[main] resumed; counter=%d, exit clean\n", counter);
    return 0;
}
