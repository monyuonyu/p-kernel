#include "idt.h"
#include "pic.h"
#include "timer.h"

/* IDTテーブル（256エントリ） */
static struct idt_entry idt_table[IDT_ENTRIES];
static struct idt_ptr idt_pointer;

/* IDTエントリ設定関数 */
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t sel, uint8_t flags) {
    idt_table[num].offset_low  = handler & 0xFFFF;
    idt_table[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt_table[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt_table[num].selector    = sel;
    idt_table[num].ist         = 0;           // IST使用しない
    idt_table[num].type_attr   = flags;
    idt_table[num].zero        = 0;
}

/* IDT初期化 */
void idt_init(void) {
    /* IDTポインタ設定 */
    idt_pointer.limit = (sizeof(struct idt_entry) * IDT_ENTRIES) - 1;
    idt_pointer.base  = (uint64_t)&idt_table;

    /* IDTテーブルをクリア */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_table[i].offset_low  = 0;
        idt_table[i].offset_mid  = 0;
        idt_table[i].offset_high = 0;
        idt_table[i].selector    = 0;
        idt_table[i].ist         = 0;
        idt_table[i].type_attr   = 0;
        idt_table[i].zero        = 0;
    }

    /* 例外ハンドラを設定 (CS=0x18: IA-32eモードでは64ビットCSが必須) */
    idt_set_gate(EXCEPTION_DE, (uint64_t)isr0,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_DB, (uint64_t)isr1,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_NMI,(uint64_t)isr2,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_BP, (uint64_t)isr3,  KERNEL64_CS, IDT_TRAP_GATE);
    idt_set_gate(EXCEPTION_OF, (uint64_t)isr4,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_BR, (uint64_t)isr5,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_UD, (uint64_t)isr6,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_NM, (uint64_t)isr7,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_DF, (uint64_t)isr8,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_TS, (uint64_t)isr10, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_NP, (uint64_t)isr11, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_SS, (uint64_t)isr12, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_GP, (uint64_t)isr13, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_PF, (uint64_t)isr14, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_MF, (uint64_t)isr16, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_AC, (uint64_t)isr17, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_MC, (uint64_t)isr18, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(EXCEPTION_XM, (uint64_t)isr19, KERNEL64_CS, IDT_INTERRUPT_GATE);

    /* IRQハンドラ登録 (INT 32-47, CS=0x18: 64ビットゲート必須) */
    idt_set_gate(IRQ_VECTOR_BASE +  0, (uint64_t)irq0,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  1, (uint64_t)irq1,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  2, (uint64_t)irq2,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  3, (uint64_t)irq3,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  4, (uint64_t)irq4,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  5, (uint64_t)irq5,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  6, (uint64_t)irq6,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  7, (uint64_t)irq7,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  8, (uint64_t)irq8,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE +  9, (uint64_t)irq9,  KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 10, (uint64_t)irq10, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 11, (uint64_t)irq11, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 12, (uint64_t)irq12, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 13, (uint64_t)irq13, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 14, (uint64_t)irq14, KERNEL64_CS, IDT_INTERRUPT_GATE);
    idt_set_gate(IRQ_VECTOR_BASE + 15, (uint64_t)irq15, KERNEL64_CS, IDT_INTERRUPT_GATE);
}

/* IDTをCPUに登録 */
void idt_install(void) {
    asm volatile ("lidt %0" : : "m" (idt_pointer));
}

/* ----------------------------------------------------------------- */
/* #DF honest-halt net on a dedicated IST stack (general hardening).   */
/*                                                                     */
/* This x86 port runs in IA-32e LONG MODE (64-bit IDT gates, CS=0x18), */
/* so the 64-bit Interrupt Stack Table (TSS.IST1..7 + the gate IST     */
/* field) DOES exist — contrary to a 32-bit-i686 reading.  A non-zero  */
/* 3-bit IST index in a 64-bit interrupt gate (gate byte offset 4,     */
/* bits[2:0]) makes the CPU UNCONDITIONALLY load RSP from TSS.IST<n>    */
/* on entry, even on a ring0->ring0 interrupt where no privilege       */
/* change occurs and RSP0 is NOT consulted.                            */
/*                                                                     */
/* WHY ONLY #DF (and NOT the IRQ/#PF vectors): routing IRQs onto a     */
/* shared IST stack is INCOMPATIBLE with this dispatcher.  The PIT     */
/* IRQ0 handler calls knl_timer_handler -> END_CRITICAL_SECTION ->     */
/* knl_dispatch() SYNCHRONOUSLY, inside the IRQ, and knl_dispatch      */
/* saves the CURRENT %esp into ctxtsk->ssp (cpu_support.S).  An IRQ    */
/* on a shared IST stack persists an IST-relative ssp that the next    */
/* interrupt overwrites -> corruption (verified: a #PF in              */
/* knl_make_wait_reltim at first boot when IRQs were on IST1).  IRQs   */
/* MUST keep running on the preempted task's own kernel stack here.    */
/*                                                                     */
/* WHAT IST BUYS US: #DF (double fault).  A #DF is the fault raised    */
/* when the CPU cannot even PUSH a fault frame — the kernel-stack-     */
/* overflow case.  Without an IST stack the #DF push also fails ->     */
/* triple fault -> silent reset.  Routed to its OWN IST2 stack, a true */
/* overflow surfaces as an HONEST "Double Fault / System halted"       */
/* instead of silent corruption.  #DF never returns/dispatches, so the */
/* shared-stack/dispatch conflict above does not apply to it.          */
/*                                                                     */
/* SCOPE HONESTY — this does NOT close gap-ledger KILL-CHURN-CRASH.    */
/* The wave-32 audit framed the residual as an "IRQ-frame-on-deep-     */
/* stack overflow".  That framing is FALSIFIED here: the residual      */
/* garbage-PC #PF (always EIP in knl_make_wait_reltim, write to a      */
/* non-present knl_ctxtsk/TCB) reproduces IDENTICALLY with 2 KB, 8 KB  */
/* AND 16 KB kernel stacks — so it is NOT a stack overflow, and this   */
/* #DF net never fires for it (the corruption is a dangling pointer,   */
/* not a frame the CPU failed to push).  The real root cause is a      */
/* KILL-PATH TCB USE-AFTER-FREE: infer_d blocks in a timed sem wait    */
/* (kdds_sub -> tk_wai_sem, on the timer + semaphore queues); the      */
/* kill/heal churn (tk_ter_tsk + user_proc_teardown + tk_del_tsk)      */
/* leaves a queue node referencing the freed victim TCB, which a later */
/* knl_make_wait_reltim/knl_timer_insert dereferences.  That fix       */
/* belongs in the kill/teardown path, not here.  This #DF net is kept  */
/* as correct general hardening (and documents the IST reality).       */
/*                                                                     */
/* TSS.IST2 is populated by gdt_init_userspace() (arch/x86/gdt_user.c) */
/* just before it calls this function.                                 */
/* ----------------------------------------------------------------- */
#define IDT_IST_DF   2u   /* TSS.IST2 — #DF only */

void idt_enable_ist(void) {
    unsigned long flags;
    asm volatile ("pushf\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");

    /* #DF onto its own IST stack: an honest halt on kernel-stack
     * overflow instead of a triple-fault reset / garbage-PC #PF.
     * ist byte low 3 bits = IST index; upper bits reserved (0). */
    idt_table[EXCEPTION_DF].ist = IDT_IST_DF & 0x7u;

    /* IDT is already loaded (lidt); editing the in-memory table is
     * picked up on the next interrupt — no re-lidt needed. */
    if (flags & 0x200u) asm volatile ("sti" ::: "memory");
}

/* 外部シリアル出力関数 */
extern void print(const char *str);

/* ----------------------------------------------------------------- */
/* ring3-core Wave B — 生存メカニズム (docs/architecture/ring3-core.md I.3) */
/*                                                                     */
/* saved CS の RPL で「カーネルのバグ (ring0)」と「ring3 テナントの    */
/* フォールト」を見分ける。ring0 → 従来どおり print + halt (正直な停止)。*/
/* ring3 → 診断 1 行 + カウンタ更新 + 当該ユーザータスクを reap して    */
/* スケジューラへ戻る (halt しない)。                                  */
/*                                                                     */
/* ring3_faults_reaped  : reap 完了の単調カウンタ (shell `ring3 test`  */
/*                        が ==1 の増分を厳密検査する — 二重フォールト */
/*                        やフォールトストームでは 1 にならず gate が  */
/*                        落ちる)。                                    */
/* last_fault_from_ring : 直近フォールトの発生リング (saved CS & 3)。  */
/* ----------------------------------------------------------------- */
volatile uint32_t ring3_faults_reaped  = 0;
volatile uint32_t last_fault_from_ring = 0;

/* arch/x86/syscall.c — SYS_EXIT と同一のアンワインドで現行ユーザー
 * タスクを終了し、tk_ext_tsk() でスケジューラへ移る。戻らない。 */
extern void user_fault_reap(void);

/* 16進/10進の簡易表示 (この時点で tm_printf に依存しないため) */
static void print_hex32(uint32_t v) {
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        int d = (int)(v & 0xF);
        buf[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    buf[8] = '\0';
    print(buf);
}
static void print_dec(uint32_t v) {
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    print(buf + i);
}

/* 共通例外ハンドラ
 *   isr.S の exc_call32_stub から cdecl で呼ばれる。
 *   saved_cs / saved_eip は CPU がプッシュした割り込みフレームの値。 */
void exception_handler(uint32_t exception_num, uint32_t error_code,
                       uint32_t saved_cs, uint32_t saved_eip) {
    const char* exception_messages[] = {
        "Division Error",
        "Debug Exception", 
        "Non-Maskable Interrupt",
        "Breakpoint",
        "Overflow",
        "BOUND Range Exceeded",
        "Invalid Opcode",
        "Device Not Available",
        "Double Fault",
        "Coprocessor Segment Overrun",
        "Invalid TSS",
        "Segment Not Present",
        "Stack-Segment Fault",
        "General Protection Fault",
        "Page Fault",
        "Reserved",
        "x87 FPU Error",
        "Alignment Check",
        "Machine Check",
        "SIMD Floating-Point Exception"
    };

    last_fault_from_ring = saved_cs & 3u;

    if ((saved_cs & 3u) == 3u) {
        /* ---- ring3 テナントのフォールト → カーネルは生き残る ---- */
        /* 診断 1 行 (ring3-core.md II.1b の書式) */
        print("[core] ring3 fault #");
        print_dec(exception_num);
        print(" @0x");
        print_hex32(saved_eip);
        print(" - task reaped\r\n");
        ring3_faults_reaped++;

        /* 例外ゲートは IF=0 で入る。reap は syscall コンテキスト
         * (IF=1, int 0x80 トラップゲート) と同条件で走らせる。 */
        asm volatile ("sti");

        /* SYS_EXIT と同じアンワインド + tk_ext_tsk() — 戻らない。
         * フォールトした命令へ iretq で戻る道はない (戻れば同じ
         * フォールトの無限ループ)。死んだコンテキストのカーネル
         * スタック上に残った例外フレームは、タスクごと破棄される。 */
        user_fault_reap();
        /* NOTREACHED */
    }

    /* ---- ring0 = カーネル自身のバグ → 従来どおり print + halt ---- */
    print("\r\n=== KERNEL EXCEPTION ===\r\n");
    print("Exception: ");
    if (exception_num < 20) {
        print(exception_messages[exception_num]);
    } else {
        print("Unknown Exception");
    }
    print("\r\n");
    
    // エラーコードがある例外の場合
    if (exception_num == 8 || (exception_num >= 10 && exception_num <= 14) || exception_num == 17) {
        print("Error Code: 0x");
        print_hex32(error_code);
        print("\r\n");
    }

    // ブレークポイント例外の場合は復帰可能
    if (exception_num == 3) {
        print("Breakpoint exception handled - continuing execution...\r\n");
        return;  // 例外から復帰
    }
    
    print("CS=0x"); print_hex32(saved_cs);
    print(" EIP=0x"); print_hex32(saved_eip);
    print("\r\n");

#ifdef KCC_DIAG
    /* KILL-CHURN-CRASH diagnostic (gap-ledger): print the decisive state
     * at a ring0 #PF — faulting linear address (CR2), the active CR3, and
     * the kernel's dispatch pointers (raw, no struct deref so this TU need
     * not see the TCB layout).  Temporary; behind -DKCC_DIAG; drop at
     * merge. */
    {
        extern void *knl_ctxtsk;
        extern void *knl_schedtsk;
        unsigned int cr2, cr3;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        asm volatile ("mov %%cr3, %0" : "=r"(cr3));
        print("CR2=0x"); print_hex32(cr2);
        print(" CR3=0x"); print_hex32(cr3);
        print("\r\n");
        print("knl_ctxtsk=0x");
        print_hex32((unsigned int)(unsigned long)knl_ctxtsk);
        print(" knl_schedtsk=0x");
        print_hex32((unsigned int)(unsigned long)knl_schedtsk);
        print("\r\n");
    }
#endif

#ifdef KCC_TRACE
    {
        extern volatile unsigned int kcc_trace[16][3];
        extern volatile unsigned int kcc_trace_pos;
        print("--- dispatch trace (newest last) ---\r\n");
        unsigned int n = kcc_trace_pos < 16 ? kcc_trace_pos : 16;
        unsigned int start = kcc_trace_pos >= 16 ? kcc_trace_pos - 16 : 0;
        for (unsigned int k = 0; k < n; k++) {
            unsigned int idx = (start + k) % 16;
            print("  tid=0x");  print_hex32(kcc_trace[idx][0]);
            print(" ssp=0x");   print_hex32(kcc_trace[idx][1]);
            print(" ret=0x");   print_hex32(kcc_trace[idx][2]);
            print("\r\n");
        }
    }
#endif

    print("System halted.\r\n");

    /* 致命的例外の場合はシステム停止 */
    while (1) {
        asm volatile ("hlt");
    }
}

/*
 * x86_irq_handlers[] - T-Kernel IRQディスパッチテーブル
 *   tkdev_init.c で設定される。NULLの場合は既定動作にフォールバック。
 */
void (*x86_irq_handlers[16])(void);

/* 共通IRQディスパッチャ (isr.S の irq_common_stub から呼ばれる) */
/* regparm(1): 第1引数を %eax/rax レジスタで渡す (i686でもレジスタ渡し) */
void __attribute__((regparm(1))) irq_handler(uint32_t irq_num) {
    /* T-Kernelハンドラが登録済みならそちらに委譲 */
    if (irq_num < 16 && x86_irq_handlers[irq_num] != 0) {
        x86_irq_handlers[irq_num]();
    }

    /* PICにEnd of Interruptを送信
     * IRQ0(タイマー)でT-Kernelハンドラが登録済みの場合は
     * knl_clear_hw_timer_interrupt() が既にEOIを送信済みなのでスキップ */
    if (irq_num != 0 || x86_irq_handlers[0] == 0) {
        pic_send_eoi((uint8_t)irq_num);
    }
}

/* IRQハンドラ登録関数 (将来の拡張用スタブ) */
void irq_register_handler(uint8_t irq, void (*handler)(void)) {
    (void)irq;
    (void)handler;
}