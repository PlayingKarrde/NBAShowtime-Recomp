// Game process switching on host fibers.
//
// The game's scheduler (main loop at 0x801B4270) resumes a process by restoring its registers,
// swapping sp with k1 and doing `jr ra` into it; a process gives control back by swapping the
// stacks again and jumping into the middle of the scheduler (0x801B4764), either from sleep
// (0x801B3F98, after saving its registers) or from exit (0x801B4088). Recompiled C can't express
// those jumps, so each game process runs on its own host fiber: the generated code calls
// recomp_proc_switch_in at the scheduler's `jr ra` and recomp_proc_switch_out at the jumps back.
// All guest register saving/restoring is still done by the guest code itself.
//
// Every program the BIOS can exec (the game, the operator menu DIAG.EXE) has its own copy of this
// scheduler. exec (recomp_exec) throws away the old program's processes and starts the new
// program's main thread on a fresh fiber, so going back and forth never nests host stacks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp.h"
#include "vegas.h"

#ifdef _WIN32
#include <windows.h>
typedef LPVOID HostFiber;
#else
#include <sys/mman.h>
#include <ucontext.h>
typedef struct { ucontext_t uc; void* stack; size_t stack_size; } HostFiberImpl;
typedef HostFiberImpl* HostFiber;
#endif

#define MAX_PROCS 256
#define FIBER_STACK (1u << 20) /* 1 MB host stack per game process */
#define MAIN_STACK (8u << 20)  /* a program's main thread after exec (like the host main thread) */

typedef struct {
  int used;
  int suspended;       /* waiting in switch_out; can be resumed */
  uint32_t resume_ra;  /* guest address it will continue at */
  uint32_t resume_sp;  /* guest stack pointer of the process */
  uint32_t entry;      /* guest entry point (first run) */
  HostFiber fiber;
} Proc;

static Proc procs[MAX_PROCS];
static Proc* current;          /* running process, NULL = scheduler */
static HostFiber scheduler_fiber;
static RecompContext* g_ctx;
static Proc* starting;         /* fiber being started (passes args into the entry trampoline) */
static int scheduler_owned;    /* scheduler_fiber was created by exec (not the host thread) */
static HostFiber zombie;       /* fiber left by exec; freed from the next one */
static uint32_t exec_entry;    /* entry of the program being started by exec */
uint64_t recomp_proc_switches, recomp_proc_created;

/* ---- host fiber primitives ---- */
#ifdef _WIN32
static void ensure_scheduler(void) {
  if (!scheduler_fiber) scheduler_fiber = ConvertThreadToFiber(NULL);
}
static void fiber_switch(HostFiber from, HostFiber to) { (void)from; SwitchToFiber(to); }
static void WINAPI fiber_entry(LPVOID arg);
static void WINAPI exec_fiber_entry(LPVOID arg);
static HostFiber fiber_create_with(void (WINAPI* fn)(LPVOID), size_t stack) { return CreateFiber(stack, fn, NULL); }
static HostFiber fiber_create(void) { return fiber_create_with(fiber_entry, FIBER_STACK); }
static void fiber_destroy(HostFiber f) { DeleteFiber(f); }
#else
static HostFiberImpl scheduler_impl, abandoned_impl;
static void ensure_scheduler(void) { if (!scheduler_fiber) scheduler_fiber = &scheduler_impl; }
static void fiber_switch(HostFiber from, HostFiber to) {
  swapcontext(&from->uc, &to->uc);
  recomp_fpu_host_sync(); /* the saved context may carry an older MXCSR */
}
static void fiber_entry(void);
static void exec_fiber_entry(void);
static HostFiber fiber_create_with(void (*fn)(void), size_t stack) {
  HostFiberImpl* f = calloc(1, sizeof *f);
  f->stack_size = stack;
  f->stack = mmap(NULL, f->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  getcontext(&f->uc);
  f->uc.uc_stack.ss_sp = f->stack;
  f->uc.uc_stack.ss_size = f->stack_size;
  f->uc.uc_link = NULL;
  makecontext(&f->uc, fn, 0);
  return f;
}
static HostFiber fiber_create(void) { return fiber_create_with(fiber_entry, FIBER_STACK); }
static void fiber_destroy(HostFiber f) {
  munmap(f->stack, f->stack_size);
  free(f);
}
#endif

/* Body of every process fiber: run the process from its entry point. Processes normally never
   return (they call the exit routine); if one does, treat it as finished. */
#ifdef _WIN32
static void WINAPI fiber_entry(LPVOID arg) {
  (void)arg;
#else
static void fiber_entry(void) {
#endif
  recomp_fpu_host_sync();
  Proc* self = starting;
  for (;;) {
    recomp_lookup(g_ctx, self->entry)(g_ctx);
    vegas_log("proc: process %08X returned from its entry function", self->entry);
    self->suspended = 0; /* never resumable */
    current = NULL;
    fiber_switch(self->fiber, scheduler_fiber);
  }
}

static Proc* find_resumable(uint32_t ra, uint32_t sp) {
  for (int i = 0; i < MAX_PROCS; ++i)
    if (procs[i].used && procs[i].suspended && procs[i].resume_ra == ra && procs[i].resume_sp == sp)
      return &procs[i];
  return NULL;
}

static Proc* new_proc(uint32_t entry) {
  Proc* p = NULL;
  for (int i = 0; i < MAX_PROCS && !p; ++i)
    if (!procs[i].used) p = &procs[i];
  if (!p) /* recycle a fiber that can never run again */
    for (int i = 0; i < MAX_PROCS && !p; ++i)
      if (!procs[i].suspended && &procs[i] != current) {
        p = &procs[i];
        fiber_destroy(p->fiber);
      }
  if (!p) { recomp_unhandled(g_ctx, entry, "proc: too many processes"); return NULL; }
  memset(p, 0, sizeof *p);
  p->used = 1;
  p->entry = entry;
  starting = p;
  p->fiber = fiber_create();
  recomp_proc_created++;
  return p;
}

/* Scheduler -> process. Guest state is already the process's (registers restored, sp swapped);
   ra is where the process continues. Returns when the process switches back out. */
void recomp_proc_switch_in(RecompContext* ctx) {
  ensure_scheduler();
  g_ctx = ctx;
  const uint32_t ra = R32(31), sp = R32(29);
  Proc* p = find_resumable(ra, sp);
  if (!p) p = new_proc(ra);
  if (!p) return;
  p->suspended = 0;
  current = p;
  recomp_proc_switches++;
  fiber_switch(scheduler_fiber, p->fiber);
}

/* Process -> scheduler. `from` is the guest function doing the switch: sleep (resumable: ra is
   the process's return address, k1 its stack pointer, since the stacks were just swapped) or
   exit (never resumed). Returns when the process is resumed. */
void recomp_proc_switch_out(RecompContext* ctx, uint32_t from) {
  Proc* p = current;
  if (!p) { recomp_unhandled(ctx, R32(31), "proc: switch out with no current process"); return; }
  p->resume_ra = R32(31);
  p->resume_sp = R32(27);
  p->suspended = (from == recomp_program()->proc_sleep); /* sleep saves the registers: resumable */
  current = NULL;
  fiber_switch(p->fiber, scheduler_fiber);
  /* resumed by recomp_proc_switch_in */
}

/* ---- exec: start another program ---- */

/* Main thread of a program started by exec. */
#ifdef _WIN32
static void WINAPI exec_fiber_entry(LPVOID arg) {
  (void)arg;
#else
static void exec_fiber_entry(void) {
#endif
  recomp_fpu_host_sync();
  if (zombie) { fiber_destroy(zombie); zombie = NULL; }
  recomp_lookup(g_ctx, exec_entry)(g_ctx);
  recomp_unhandled(g_ctx, exec_entry, "exec: program returned from its entry point");
  abort();
}

void recomp_proc_exec(RecompContext* ctx, uint32_t entry) {
  ensure_scheduler();
  g_ctx = ctx;
  /* The new program has overwritten the old one's memory: none of its processes can run again. */
  HostFiber self = current ? current->fiber : scheduler_fiber;
  for (int i = 0; i < MAX_PROCS; ++i) {
    if (procs[i].used && procs[i].fiber != self) fiber_destroy(procs[i].fiber);
    memset(&procs[i], 0, sizeof procs[i]);
  }
  /* The fiber we are on (a process, or the old main thread) is never resumed. Fibers exec made
     are freed once off them; the host thread's own context is just left behind. */
  const int self_owned = self != scheduler_fiber || scheduler_owned;
  if (self != scheduler_fiber && scheduler_owned) fiber_destroy(scheduler_fiber);
  zombie = self_owned ? self : NULL;
  current = NULL;
  exec_entry = entry;
  scheduler_fiber = fiber_create_with(exec_fiber_entry, MAIN_STACK);
  scheduler_owned = 1;
#ifdef _WIN32
  SwitchToFiber(scheduler_fiber);
#else
  /* the old context is never resumed: save it somewhere harmless when it isn't ours to free */
  fiber_switch(self_owned ? self : &abandoned_impl, scheduler_fiber);
#endif
  recomp_unhandled(ctx, entry, "exec: old program resumed");
}

void (*recomp_exec_started_hook)(RecompContext* ctx, const RecompProgram* p);
void (*recomp_exec_unknown_hook)(RecompContext* ctx, uint32_t entry);

/* Called by the BIOS exec (syscall 45) in place of its `jalr` into the program it has just
   loaded, checked and moved into place. */
void recomp_exec(RecompContext* ctx, uint32_t entry) {
  const char* dump = getenv("NBA_EXEC_DUMP"); /* developers: save each exec'd image (docs/recompiler.md) */
  uint32_t file_size, crc;
  memcpy(&file_size, recomp_rdram + 0xE1FA4u, 4); /* BIOS exec's copy of the file size and CRC */
  memcpy(&crc, recomp_rdram + 0xE1FC8u, 4);
  if (dump && file_size > 8 && file_size - 8 <= RECOMP_RAM_SIZE - recomp_phys(entry)) {
    char path[1200];
    snprintf(path, sizeof path, "%s/exec_%08X_%08X.bin", dump, entry, crc);
    FILE* f = fopen(path, "wb");
    if (f) {
      fwrite(recomp_rdram + recomp_phys(entry), 1, file_size - 8, f);
      fclose(f);
      fprintf(stderr, "exec: saved the %u-byte image at %08X to %s\n", file_size - 8, entry, path);
    }
  }
  const RecompProgram* p = recomp_identify_program();
  if (!p) {
    vegas_log("exec: the program loaded at %08X (file CRC %08X, %u bytes) was not recompiled", entry, crc, file_size);
    if (recomp_exec_unknown_hook) recomp_exec_unknown_hook(ctx, entry);
    recomp_unhandled(ctx, entry, "exec: program not recompiled");
    return;
  }
  vegas_log("exec: starting %s at %08X", p->name, entry);
  recomp_set_program(p);
  if (recomp_exec_started_hook) recomp_exec_started_hook(ctx, p);
  recomp_proc_exec(ctx, entry);
}
