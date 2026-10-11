#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <tlhelp32.h>

#include "slopkit_debug.h"

/* The agent provides these, because -nostdlib leaves no CRT. */
void* memset(void* destination, int value, size_t count);
void* memcpy(void* destination, const void* source, size_t count);

/* A short spin for the polling loops; never a library call, so it is safe on a
   target thread that may hold a Wine lock. */
static void d_pause(void)
{
    YieldProcessor();
}

/* The calling thread's Linux tid. Wine maps each target thread 1:1 onto a
   Linux thread, but the Windows and Linux tid spaces are disjoint, and this is
   the only translation the agent needs. The first field of `thread-self/stat`
   is the thread's own Linux tid, and the Unix device path is the one Wine
   always exposes; the file handle is opened with no buffering and closed
   immediately, so the window in which it could take a Wine lock is short. */
static uint32_t d_self_linux_tid(void)
{
    static const char path[] = "\\\\?\\unix\\proc\\thread-self\\stat";
    const HANDLE      file =
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    char  buffer[128];
    DWORD count = 0;
    const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &count, NULL);
    CloseHandle(file);
    if (!ok)
    {
        return 0;
    }
    uint32_t value = 0;
    for (DWORD index = 0; index < count; ++index)
    {
        if (buffer[index] < '0' || buffer[index] > '9')
        {
            break;
        }
        value = value * 10u + (uint32_t)(buffer[index] - '0');
    }
    return value;
}

/* The engine's state. One region exists per process, so a file-static is
   enough; it is set once at attach and cleared at detach. */
static slopkit_agent_region* d_region;
static slopkit_agent_debug*  d_block;

static PVOID                 d_veh;
static HANDLE                d_watchdog;
static volatile LONG         d_watchdog_stop;
static volatile LONG         d_watchdog_noted;
static CRITICAL_SECTION      d_lock;
static int                   d_lock_ready;

/* The tids of the agent's own threads, which are never frozen or debugged. */
#define D_AGENT_THREADS 16u
static uint32_t d_agent_tids[D_AGENT_THREADS];
static uint32_t d_agent_tid_count;

/* -1 = no owner; 1 = the VEH holds the stop; 2 = the watchdog holds it. */
#define D_OWNER_NONE 0
#define D_OWNER_VEH 1
#define D_OWNER_WATCHDOG 2
static volatile LONG d_owner;

/* The stopped thread's context, kept while the watchdog owns a freeze stop. */
static CONTEXT d_stop_context;
static volatile LONG d_context_valid;

/* The trap re-arm dance: the release after a trap lets the original
   instruction run once with TF set, then the single-step handler re-instates
   the 0xCC byte. `d_rearm_address` is the address to re-arm, and
   `d_step_publish` says the single step must become a published stop instead
   of a silent re-arm. */
static volatile LONG d_tf_owner;
static volatile LONG d_step_publish;
static uint64_t      d_rearm_address;

/* Every target thread the agent knows, with its Linux tid once probed. */
#define D_MAX_THREADS 512u
typedef struct d_thread
{
    uint32_t win_tid;
    uint32_t linux_tid;
    HANDLE   handle;
    uint32_t suspended;
    uint32_t reserved;
} d_thread;

static d_thread d_threads[D_MAX_THREADS];
static uint32_t d_thread_count;

/* The thread a freeze stop holds; its context is restored on release. */
static HANDLE   d_leader_handle;
static uint32_t d_leader_win_tid;
static uint32_t d_leader_linux_tid;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t d_ptr(const void* pointer)
{
    return (uint64_t)(uintptr_t)pointer;
}

/* Writes one byte into the target's code, protecting the page around it and
   flushing the instruction cache so a running core sees the change. */
static int d_write_byte(uint64_t address, uint8_t value)
{
    DWORD previous = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)address, 1, PAGE_EXECUTE_READWRITE, &previous))
    {
        return 0;
    }
    *(volatile uint8_t*)(uintptr_t)address = value;
    VirtualProtect((LPVOID)(uintptr_t)address, 1, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)address, 1);
    return 1;
}

static uint8_t d_read_byte(uint64_t address)
{
    return *(volatile uint8_t*)(uintptr_t)address;
}

static int d_is_agent_tid(uint32_t win_tid)
{
    for (uint32_t index = 0; index < d_agent_tid_count; ++index)
    {
        if (d_agent_tids[index] == win_tid)
        {
            return 1;
        }
    }
    return 0;
}

static int d_is_excluded_linux_tid(uint32_t linux_tid)
{
    if (linux_tid == 0 || d_block == NULL)
    {
        return 0;
    }
    for (uint32_t index = 0; index < SLOPKIT_AGENT_DEBUG_MAX_EXCLUDED; ++index)
    {
        const uint32_t entry = d_block->excluded_tid[index];
        if (entry == 0xFFFFFFFFu)
        {
            break;
        }
        if (entry == linux_tid)
        {
            return 1;
        }
    }
    return 0;
}

void slopkit_debug_note_agent_thread(void)
{
    const uint32_t tid = (uint32_t)GetCurrentThreadId();
    if (d_is_agent_tid(tid))
    {
        return;
    }
    if (d_agent_tid_count < D_AGENT_THREADS)
    {
        d_agent_tids[d_agent_tid_count] = tid;
        ++d_agent_tid_count;
    }
}

int slopkit_debug_active(void)
{
    return d_region != NULL && d_block != NULL
           && (uint32_t)InterlockedCompareExchange((volatile LONG*)&d_block->state, 0, 0)
                  != SLOPKIT_AGENT_DBG_STATE_DETACHED;
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

/* WIN32's CONTEXT spells the general registers differently per bitness; the
   engine is one translation unit over both. */
#if defined(__x86_64__)
#define D_CTX_WORD DWORD64
#define D_CTX_RAX(c) ((c)->Rax)
#define D_CTX_RBX(c) ((c)->Rbx)
#define D_CTX_RCX(c) ((c)->Rcx)
#define D_CTX_RDX(c) ((c)->Rdx)
#define D_CTX_RSI(c) ((c)->Rsi)
#define D_CTX_RDI(c) ((c)->Rdi)
#define D_CTX_RBP(c) ((c)->Rbp)
#define D_CTX_RSP(c) ((c)->Rsp)
#define D_CTX_RIP(c) ((c)->Rip)
#define D_CTX_FLAGS(c) ((c)->EFlags)
#else
#define D_CTX_WORD DWORD
#define D_CTX_RAX(c) ((c)->Eax)
#define D_CTX_RBX(c) ((c)->Ebx)
#define D_CTX_RCX(c) ((c)->Ecx)
#define D_CTX_RDX(c) ((c)->Edx)
#define D_CTX_RSI(c) ((c)->Esi)
#define D_CTX_RDI(c) ((c)->Edi)
#define D_CTX_RBP(c) ((c)->Ebp)
#define D_CTX_RSP(c) ((c)->Esp)
#define D_CTX_RIP(c) ((c)->Eip)
#define D_CTX_FLAGS(c) ((c)->EFlags)
#endif

/* Index order the host expects: RAX,RBX,RCX,RDX,RSI,RDI,RBP,RSP,R8..R15,RIP,
   RFLAGS. Only 18 slots are defined on both bitnesses; the high registers read
   as zero on i686. The struct is eighteen contiguous 64-bit words, so the
   snapshot and the staged writes move through one local array. */
static uint64_t d_reg_get(const CONTEXT* context, uint32_t index)
{
    switch (index)
    {
    case 0:
        return (uint64_t)D_CTX_RAX(context);
    case 1:
        return (uint64_t)D_CTX_RBX(context);
    case 2:
        return (uint64_t)D_CTX_RCX(context);
    case 3:
        return (uint64_t)D_CTX_RDX(context);
    case 4:
        return (uint64_t)D_CTX_RSI(context);
    case 5:
        return (uint64_t)D_CTX_RDI(context);
    case 6:
        return (uint64_t)D_CTX_RBP(context);
    case 7:
        return (uint64_t)D_CTX_RSP(context);
#if defined(__x86_64__)
    case 8:
        return (uint64_t)context->R8;
    case 9:
        return (uint64_t)context->R9;
    case 10:
        return (uint64_t)context->R10;
    case 11:
        return (uint64_t)context->R11;
    case 12:
        return (uint64_t)context->R12;
    case 13:
        return (uint64_t)context->R13;
    case 14:
        return (uint64_t)context->R14;
    case 15:
        return (uint64_t)context->R15;
#else
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
        return 0;
#endif
    case 16:
        return (uint64_t)D_CTX_RIP(context);
    case 17:
        return (uint64_t)context->EFlags;
    default:
        return 0;
    }
}

static void d_reg_set(CONTEXT* context, uint32_t index, uint64_t value)
{
    switch (index)
    {
    case 0:
        D_CTX_RAX(context) = (D_CTX_WORD)value;
        break;
    case 1:
        D_CTX_RBX(context) = (D_CTX_WORD)value;
        break;
    case 2:
        D_CTX_RCX(context) = (D_CTX_WORD)value;
        break;
    case 3:
        D_CTX_RDX(context) = (D_CTX_WORD)value;
        break;
    case 4:
        D_CTX_RSI(context) = (D_CTX_WORD)value;
        break;
    case 5:
        D_CTX_RDI(context) = (D_CTX_WORD)value;
        break;
    case 6:
        D_CTX_RBP(context) = (D_CTX_WORD)value;
        break;
    case 7:
        D_CTX_RSP(context) = (D_CTX_WORD)value;
        break;
#if defined(__x86_64__)
    case 8:
        context->R8 = value;
        break;
    case 9:
        context->R9 = value;
        break;
    case 10:
        context->R10 = value;
        break;
    case 11:
        context->R11 = value;
        break;
    case 12:
        context->R12 = value;
        break;
    case 13:
        context->R13 = value;
        break;
    case 14:
        context->R14 = value;
        break;
    case 15:
        context->R15 = value;
        break;
#else
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
        break;
#endif
    case 16:
        D_CTX_RIP(context) = (D_CTX_WORD)value;
        break;
    case 17:
        D_CTX_FLAGS(context) = (D_CTX_WORD)value;
        break;
    default:
        break;
    }
}

static void d_fill_registers(slopkit_agent_registers* out, const CONTEXT* context)
{
    uint64_t values[SLOPKIT_AGENT_DEBUG_REG_COUNT];
    for (uint32_t index = 0; index < SLOPKIT_AGENT_DEBUG_REG_COUNT; ++index)
    {
        values[index] = d_reg_get(context, index);
    }
    memcpy(out, values, sizeof(values));
}

/* ------------------------------------------------------------------ */
/* Region words                                                        */
/* ------------------------------------------------------------------ */

/* The block's words are published to the host under the release/acquire
   conventions the slot ring already uses; Interlocked gives both the atomicity
   and the ordering on both bitnesses. */
static uint32_t d_load(volatile uint32_t* word)
{
    return (uint32_t)InterlockedCompareExchange((volatile LONG*)word, 0, 0);
}

static void d_store(volatile uint32_t* word, uint32_t value)
{
    InterlockedExchange((volatile LONG*)word, (LONG)value);
}

#define D_RELEASE_SERVE 0
#define D_RELEASE_RESUME 1
#define D_RELEASE_STEP 2
#define D_RELEASE_DETACH 3

/* ------------------------------------------------------------------ */
/* Thread table                                                        */
/* ------------------------------------------------------------------ */

static d_thread* d_find_linux(uint32_t linux_tid)
{
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        if (d_threads[index].linux_tid != 0 && d_threads[index].linux_tid == linux_tid)
        {
            return &d_threads[index];
        }
    }
    return NULL;
}

/* The calling thread's cached Linux tid. The VEH runs on a target thread and
   must not open a file there, so it uses the mapping built at attach. */
static uint32_t d_self_linux_tid_cached(void)
{
    const uint32_t win_tid = (uint32_t)GetCurrentThreadId();
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        if (d_threads[index].win_tid == win_tid)
        {
            return d_threads[index].linux_tid;
        }
    }
    return 0;
}

static void d_close_threads(void)
{
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        if (d_threads[index].handle != NULL)
        {
            CloseHandle(d_threads[index].handle);
        }
    }
    d_thread_count = 0;
}

/* Enumerates the process' threads, skipping the agent's own. A handle that
   cannot be opened is not debugged, exactly like a ptrace target that refuses
   PTRACE_SEIZE. Called again before every interrupt stop, it keeps each known
   thread's handle, Linux tid and suspend state and only adds or drops the
   difference, so a re-scan never forgets a mapping mid-session. */
static d_thread d_scan[D_MAX_THREADS];

static void d_enumerate_threads(void)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }
    uint32_t scanned = 0;
    THREADENTRY32 entry;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize        = sizeof(entry);
    const DWORD own_pid = GetCurrentProcessId();
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != own_pid || d_is_agent_tid(entry.th32ThreadID))
            {
                continue;
            }
            if (scanned >= D_MAX_THREADS)
            {
                break;
            }
            const uint32_t win_tid  = entry.th32ThreadID;
            uint32_t       existing = 0xFFFFFFFFu;
            for (uint32_t index = 0; index < d_thread_count; ++index)
            {
                if (d_threads[index].win_tid == win_tid)
                {
                    existing = index;
                    break;
                }
            }
            d_thread* const target = &d_scan[scanned];
            if (existing != 0xFFFFFFFFu)
            {
                *target = d_threads[existing];
            }
            else
            {
                target->handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT
                                                | THREAD_QUERY_INFORMATION,
                                            FALSE,
                                            win_tid);
                if (target->handle == NULL)
                {
                    continue;
                }
                target->win_tid   = win_tid;
                target->linux_tid = 0;
                target->suspended = 0;
                target->reserved  = 0;
            }
            ++scanned;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);

    /* Close the handles of threads that are gone; a live thread keeps its
       handle and its mapping. */
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        int kept = 0;
        for (uint32_t scan_index = 0; scan_index < scanned; ++scan_index)
        {
            if (d_scan[scan_index].win_tid == d_threads[index].win_tid)
            {
                kept = 1;
                break;
            }
        }
        if (!kept && d_threads[index].handle != NULL)
        {
            CloseHandle(d_threads[index].handle);
        }
    }

    memcpy(d_threads, d_scan, sizeof(d_thread) * scanned);
    d_thread_count = scanned;
}

/* The body a hijacked thread runs. It reports its own Linux tid, flags the
   result, then spins until the agent suspends it and puts the original context
   back. It must not return: the thread was resumed at an arbitrary stack
   position, so there is no caller to return to. */
static volatile uint32_t g_probe_linux_tid;
static volatile LONG     g_probe_ready;

static void d_probe_body(void)
{
    g_probe_linux_tid = d_self_linux_tid();
    InterlockedExchange(&g_probe_ready, 1);
    for (;;)
    {
        d_pause();
    }
}

/* Runs d_probe_body on a suspended thread and restores it. The thread is left
   suspended, as the caller found it. */
static void d_probe_thread(d_thread* thread)
{
    if (thread->handle == NULL || thread->linux_tid != 0)
    {
        return;
    }
    CONTEXT saved;
    memset(&saved, 0, sizeof(saved));
    saved.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread->handle, &saved))
    {
        return;
    }

    InterlockedExchange(&g_probe_ready, 0);
    g_probe_linux_tid = 0;

    CONTEXT hijack     = saved;
    D_CTX_RIP(&hijack) = (D_CTX_WORD)d_ptr(&d_probe_body);

    if (!SetThreadContext(thread->handle, &hijack))
    {
        return;
    }
    if (ResumeThread(thread->handle) == (DWORD)-1)
    {
        SetThreadContext(thread->handle, &saved);
        return;
    }
    for (uint32_t spin = 0; spin < 500 && InterlockedCompareExchange(&g_probe_ready, 0, 0) == 0; ++spin)
    {
        Sleep(1);
    }
    SuspendThread(thread->handle);
    SetThreadContext(thread->handle, &saved);

    if (InterlockedCompareExchange(&g_probe_ready, 0, 0) != 0)
    {
        thread->linux_tid = g_probe_linux_tid;
    }
}

static void d_suspend_thread(d_thread* thread)
{
    if (thread->handle == NULL || thread->suspended != 0)
    {
        return;
    }
    if (SuspendThread(thread->handle) != (DWORD)-1)
    {
        thread->suspended = 1;
    }
}

static void d_freeze_all(uint32_t except_win_tid)
{
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        if (d_threads[index].win_tid != except_win_tid
            && !d_is_excluded_linux_tid(d_threads[index].linux_tid))
        {
            d_suspend_thread(&d_threads[index]);
        }
    }
}

static void d_thaw_all(void)
{
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        if (d_threads[index].suspended != 0 && d_threads[index].handle != NULL)
        {
            ResumeThread(d_threads[index].handle);
            d_threads[index].suspended = 0;
        }
    }
}

/* Resumes the leader exactly once: the freeze release and the drain must not
   both decrement its suspend count. */
static void d_resume_leader(void)
{
    if (d_leader_handle == NULL)
    {
        return;
    }
    ResumeThread(d_leader_handle);
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        if (d_threads[index].handle == d_leader_handle)
        {
            d_threads[index].suspended = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Software traps                                                      */
/* ------------------------------------------------------------------ */

static int d_find_trap_slot(uint32_t slot)
{
    for (uint32_t index = 0; index < d_block->trap_count; ++index)
    {
        if (d_block->trap[index].slot == slot)
        {
            return (int)index;
        }
    }
    return -1;
}

static int d_find_trap_address(uint64_t address)
{
    for (uint32_t index = 0; index < d_block->trap_count; ++index)
    {
        if (d_block->trap[index].armed != 0 && d_block->trap[index].address == address)
        {
            return (int)index;
        }
    }
    return -1;
}

static int d_trap_arm(uint64_t address, uint32_t slot)
{
    if (address == 0)
    {
        return 0;
    }
    int index = d_find_trap_slot(slot);
    if (index < 0)
    {
        if (d_block->trap_count >= SLOPKIT_AGENT_DEBUG_MAX_TRAPS)
        {
            return 0;
        }
        index                    = (int)d_block->trap_count;
        d_block->trap[index].slot = slot;
        d_block->trap[index].armed = 0;
        ++d_block->trap_count;
    }
    slopkit_agent_debug_trap* const trap = &d_block->trap[index];
    trap->address                        = address;
    if (trap->armed == 0)
    {
        trap->saved_byte = d_read_byte(address);
        if (!d_write_byte(address, 0xCC))
        {
            return 0;
        }
        trap->armed = 1;
    }
    return 1;
}

static int d_trap_disarm(uint32_t slot)
{
    const int index = d_find_trap_slot(slot);
    if (index < 0)
    {
        return 1;
    }
    slopkit_agent_debug_trap* const trap = &d_block->trap[index];
    if (trap->armed != 0)
    {
        d_write_byte(trap->address, trap->saved_byte);
        trap->armed = 0;
    }
    d_block->trap[index] = d_block->trap[d_block->trap_count - 1];
    --d_block->trap_count;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Hardware slots                                                      */
/* ------------------------------------------------------------------ */

static int d_find_hardware_slot(uint32_t slot)
{
    for (uint32_t index = 0; index < d_block->hardware_count; ++index)
    {
        if (d_block->hardware[index].slot == slot)
        {
            return (int)index;
        }
    }
    return -1;
}

/* x86 DR7 encodes the access type as 00 execute, 01 write, 11 read/write. */
static DWORD d_hw_rw(uint32_t kind)
{
    switch (kind)
    {
    case SLOPKIT_AGENT_DBG_HW_WRITE:
        return 1u;
    case SLOPKIT_AGENT_DBG_HW_READ_WRITE:
        return 3u;
    default:
        return 0u;
    }
}

static DWORD d_hw_len(uint32_t kind, uint32_t size)
{
    if (kind == SLOPKIT_AGENT_DBG_HW_EXECUTE)
    {
        return 0u;
    }
    switch (size)
    {
    case 2:
        return 1u;
    case 8:
        return 2u;
    case 4:
        return 3u;
    default:
        return 0u;
    }
}

static uint32_t d_slot_from_dr6(uint64_t dr6)
{
    for (uint32_t slot = 0; slot < SLOPKIT_AGENT_DEBUG_DR_SLOTS; ++slot)
    {
        if ((dr6 & (1ull << slot)) != 0)
        {
            return slot;
        }
    }
    return 0xFFFFFFFFu;
}

/* Programs DR0..DR3/DR7 on one suspended thread from the hardware table. */
static void d_program_thread(HANDLE handle)
{
    if (handle == NULL)
    {
        return;
    }
    CONTEXT context;
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(handle, &context))
    {
        return;
    }
    context.Dr0 = 0;
    context.Dr1 = 0;
    context.Dr2 = 0;
    context.Dr3 = 0;
    DWORD64 dr7 = 0;
    for (uint32_t index = 0; index < d_block->hardware_count; ++index)
    {
        const slopkit_agent_debug_hardware* const entry = &d_block->hardware[index];
        if (entry->enabled == 0 || entry->slot >= SLOPKIT_AGENT_DEBUG_DR_SLOTS)
        {
            continue;
        }
        switch (entry->slot)
        {
        case 0:
            context.Dr0 = (DWORD64)entry->address;
            break;
        case 1:
            context.Dr1 = (DWORD64)entry->address;
            break;
        case 2:
            context.Dr2 = (DWORD64)entry->address;
            break;
        default:
            context.Dr3 = (DWORD64)entry->address;
            break;
        }
        dr7 |= 1ull << (2u * entry->slot);
        dr7 |= (DWORD64)d_hw_rw(entry->kind) << (16u + 4u * entry->slot);
        dr7 |= (DWORD64)d_hw_len(entry->kind, entry->size) << (18u + 4u * entry->slot);
    }
    context.Dr7 = dr7;
    context.Dr6 = 0;
    SetThreadContext(handle, &context);
}

/* Mirrors the hardware table into the VEH's own outgoing context, so the
   trapping thread leaves the stop with the watch armed too. */
static void d_program_context(CONTEXT* context)
{
    context->Dr0 = 0;
    context->Dr1 = 0;
    context->Dr2 = 0;
    context->Dr3 = 0;
    DWORD64 dr7 = 0;
    for (uint32_t index = 0; index < d_block->hardware_count; ++index)
    {
        const slopkit_agent_debug_hardware* const entry = &d_block->hardware[index];
        if (entry->enabled == 0 || entry->slot >= SLOPKIT_AGENT_DEBUG_DR_SLOTS)
        {
            continue;
        }
        switch (entry->slot)
        {
        case 0:
            context->Dr0 = (DWORD64)entry->address;
            break;
        case 1:
            context->Dr1 = (DWORD64)entry->address;
            break;
        case 2:
            context->Dr2 = (DWORD64)entry->address;
            break;
        default:
            context->Dr3 = (DWORD64)entry->address;
            break;
        }
        dr7 |= 1ull << (2u * entry->slot);
        dr7 |= (DWORD64)d_hw_rw(entry->kind) << (16u + 4u * entry->slot);
        dr7 |= (DWORD64)d_hw_len(entry->kind, entry->size) << (18u + 4u * entry->slot);
    }
    context->Dr7 = dr7;
    context->Dr6 = 0;
}

/* Re-programs every suspended target thread. The caller must hold them all. */
static void d_reprogram_frozen(void)
{
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        if (d_threads[index].suspended != 0)
        {
            d_program_thread(d_threads[index].handle);
        }
    }
}

static int d_hardware_arm(uint64_t address, uint32_t slot, uint32_t kind, uint32_t size)
{
    if (slot >= SLOPKIT_AGENT_DEBUG_DR_SLOTS)
    {
        return 0;
    }
    int index = d_find_hardware_slot(slot);
    if (index < 0)
    {
        if (d_block->hardware_count >= SLOPKIT_AGENT_DEBUG_DR_SLOTS)
        {
            return 0;
        }
        index                          = (int)d_block->hardware_count;
        d_block->hardware[index].slot  = slot;
        ++d_block->hardware_count;
    }
    d_block->hardware[index].address = address;
    d_block->hardware[index].kind    = kind;
    d_block->hardware[index].size    = size;
    d_block->hardware[index].enabled = 1;
    return 1;
}

static int d_hardware_disarm(uint32_t slot)
{
    const int index = d_find_hardware_slot(slot);
    if (index < 0)
    {
        return 1;
    }
    d_block->hardware[index].enabled = 0;
    d_block->hardware[index]         = d_block->hardware[d_block->hardware_count - 1];
    --d_block->hardware_count;
    return 1;
}

/* Re-applies the hardware table to the running process: every other thread is
   frozen for the edit, so a thread started in between cannot keep stale DRs. */
static void d_reprogram_running(void)
{
    d_freeze_all(0);
    d_reprogram_frozen();
    d_thaw_all();
}

/* ------------------------------------------------------------------ */
/* Stops and command serving                                           */
/* ------------------------------------------------------------------ */

static volatile uint32_t d_served_seq;

static void d_publish(uint32_t win_tid,
                      uint32_t linux_tid,
                      uint32_t reason,
                      uint32_t slot,
                      uint64_t address,
                      uint64_t trap_address,
                      const CONTEXT* context)
{
    d_block->held_win_tid     = win_tid;
    d_block->held_linux_tid   = linux_tid;
    d_block->stop_reason      = reason;
    d_block->stop_slot        = slot;
    d_block->stop_signal      = 0;
    d_block->stop_address     = address;
    d_block->stop_trap_address = trap_address;
    if (context != NULL)
    {
        d_fill_registers(&d_block->registers, context);
        d_block->register_mask = 0xFFFFFFFFu;
    }
    else
    {
        memset(&d_block->registers, 0, sizeof(d_block->registers));
        d_block->register_mask = 0;
    }
    d_block->stage_mask = 0;
    memset(d_block->stage_value, 0, sizeof(d_block->stage_value));
    d_served_seq = d_load(&d_block->request);
    d_store(&d_block->flag, 1);
}

static void d_answer(uint32_t sequence)
{
    d_store(&d_block->answer, sequence);
    d_store((volatile uint32_t*)&d_block->cmd, 0);
}

/* Applies the staged register writes and the resume lane to a context about to
   be released, and arms the trap dance the single-step handler will finish. */
static void d_prepare_release(CONTEXT* context, uint64_t resume_address, uint32_t want_step)
{
    d_rearm_address = 0;
    d_step_publish  = 0;
    d_tf_owner      = 0;

    if (want_step != 0)
    {
        /* Stepping over a breakpoint means running the patched instruction
           once with its real byte, then re-arming it. */
        const int index = d_find_trap_address(D_CTX_RIP(context));
        if (index >= 0 && d_block->trap[index].armed == 0)
        {
            d_rearm_address = d_block->trap[index].address;
        }
        d_step_publish = 1;
        d_tf_owner     = 1;
    }
    else if (resume_address != 0 && resume_address != 0xFFFFFFFFFFFFFFFFull)
    {
        const int index = d_find_trap_address(resume_address);
        if (index >= 0)
        {
            if (D_CTX_RIP(context) == resume_address && d_block->trap[index].armed == 0)
            {
                d_rearm_address = resume_address;
                d_tf_owner      = 1;
            }
            else if (d_block->trap[index].armed == 0)
            {
                d_write_byte(resume_address, 0xCC);
                d_block->trap[index].armed = 1;
            }
        }
    }

    if (d_tf_owner != 0)
    {
        D_CTX_FLAGS(context) |= 0x100u;
    }
    d_program_context(context);
}

static int d_handle_get_registers(CONTEXT* context, const slopkit_agent_debug* block)
{
    const uint32_t want = (uint32_t)block->arg[0];
    if (want == 0 || want == d_block->held_linux_tid)
    {
        d_fill_registers(&d_block->registers, context);
        d_block->register_mask = 0xFFFFFFFFu;
        return 1;
    }
    d_thread* const thread = d_find_linux(want);
    if (thread == NULL || thread->handle == NULL)
    {
        return 0;
    }
    CONTEXT other;
    memset(&other, 0, sizeof(other));
    other.ContextFlags = CONTEXT_FULL;
    SuspendThread(thread->handle);
    const int ok = GetThreadContext(thread->handle, &other);
    ResumeThread(thread->handle);
    if (!ok)
    {
        return 0;
    }
    d_fill_registers(&d_block->registers, &other);
    d_block->register_mask = 0xFFFFFFFFu;
    return 1;
}

/* Serves one command while a stop is held. Returns D_RELEASE_SERVE to keep
   waiting, or a release action. */
static int d_handle_stopped(CONTEXT* context)
{
    const uint32_t sequence = d_load(&d_block->request);
    const int32_t  command  = (int32_t)d_load((volatile uint32_t*)&d_block->cmd);
    int            result   = D_RELEASE_SERVE;
    d_block->status = SLOPKIT_AGENT_STATUS_OK;

    switch (command)
    {
    case SLOPKIT_AGENT_DBG_CMD_GET_REGISTERS:
        if (!d_handle_get_registers(context, d_block))
        {
            d_block->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        }
        break;
    case SLOPKIT_AGENT_DBG_CMD_SET_REGISTER:
    {
        const uint32_t index = (uint32_t)d_block->arg[1];
        if (index < SLOPKIT_AGENT_DEBUG_REG_COUNT)
        {
            d_reg_set(context, index, d_block->arg[2]);
            d_block->stage_mask = 1u << index;
            d_block->stage_value[index] = d_block->arg[2];
        }
        break;
    }
    case SLOPKIT_AGENT_DBG_CMD_ARM_SOFTWARE:
        if (!d_trap_arm(d_block->arg[0], (uint32_t)d_block->arg[1]))
        {
            d_block->status = SLOPKIT_AGENT_STATUS_UNSUPPORTED;
        }
        break;
    case SLOPKIT_AGENT_DBG_CMD_DISARM_SOFTWARE:
        (void)d_trap_disarm((uint32_t)d_block->arg[0]);
        break;
    case SLOPKIT_AGENT_DBG_CMD_ARM_HARDWARE:
        if (!d_hardware_arm(d_block->arg[0], (uint32_t)d_block->arg[1], (uint32_t)d_block->arg[2], (uint32_t)d_block->arg[3]))
        {
            d_block->status = SLOPKIT_AGENT_STATUS_UNSUPPORTED;
        }
        else
        {
            d_reprogram_frozen();
        }
        break;
    case SLOPKIT_AGENT_DBG_CMD_DISARM_HARDWARE:
        (void)d_hardware_disarm((uint32_t)d_block->arg[0]);
        d_reprogram_frozen();
        break;
    case SLOPKIT_AGENT_DBG_CMD_BACKTRACE:
        /* The host walks the frame-pointer chain itself; it already has the
           snapshot and reads the target's memory directly. */
        break;
    case SLOPKIT_AGENT_DBG_CMD_RESUME:
        d_prepare_release(context, d_block->resume_address, 0);
        result = D_RELEASE_RESUME;
        break;
    case SLOPKIT_AGENT_DBG_CMD_STEP:
        d_prepare_release(context, 0, 1);
        result = D_RELEASE_STEP;
        break;
    default:
        d_block->status = SLOPKIT_AGENT_STATUS_UNKNOWN_OP;
        break;
    }

    d_answer(sequence);
    return result;
}

/* Serves commands until the stop is released. For a watchdog-owned stop the
   release also restores the leader's context and thaws the process; the VEH
   path leaves that to the handler's return value. */
static int d_serve(CONTEXT* context, int from_veh)
{
    for (;;)
    {
        const uint32_t sequence = d_load(&d_block->request);
        if (sequence == d_served_seq)
        {
            Sleep(1);
            continue;
        }
        d_served_seq = sequence;
        EnterCriticalSection(&d_lock);
        const int result = d_handle_stopped(context);
        LeaveCriticalSection(&d_lock);
        if (result == D_RELEASE_SERVE)
        {
            continue;
        }

        if (result == D_RELEASE_DETACH)
        {
            return result;
        }
        if (from_veh == 0)
        {
            SetThreadContext(d_leader_handle, context);
            d_resume_leader();
            d_thaw_all();
            InterlockedExchange(&d_owner, D_OWNER_NONE);
            d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_RUNNING);
            d_store(&d_block->flag, 0);
        }
        return result;
    }
}

/* A freeze stop, published by the watchdog: the whole process is suspended and
   the watchdog serves the commands itself. */
static uint32_t d_pick_leader(void)
{
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        if (d_threads[index].linux_tid != 0 && d_threads[index].suspended != 0
            && !d_is_excluded_linux_tid(d_threads[index].linux_tid))
        {
            return index;
        }
    }
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        if (d_threads[index].suspended != 0)
        {
            return index;
        }
    }
    return 0xFFFFFFFFu;
}

static void d_freeze_stop(uint32_t reason, uint32_t sequence)
{
    d_freeze_all(0);
    const uint32_t leader = d_pick_leader();
    if (leader == 0xFFFFFFFFu)
    {
        d_answer(sequence);
        return;
    }
    memset(&d_stop_context, 0, sizeof(d_stop_context));
    d_stop_context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(d_threads[leader].handle, &d_stop_context))
    {
        d_thaw_all();
        d_answer(sequence);
        return;
    }
    d_leader_handle    = d_threads[leader].handle;
    d_leader_win_tid   = d_threads[leader].win_tid;
    d_leader_linux_tid = d_threads[leader].linux_tid;

    InterlockedExchange(&d_owner, D_OWNER_WATCHDOG);
    d_publish(d_leader_win_tid,
              d_leader_linux_tid,
              reason,
              0xFFFFFFFFu,
              (uint64_t)D_CTX_RIP(&d_stop_context),
              (uint64_t)D_CTX_RIP(&d_stop_context),
              &d_stop_context);
    d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_STOPPED);
    /* The command that triggered the stop answers as soon as the stop is
       visible, so the host call returns. */
    d_answer(sequence);
    d_served_seq = sequence;

    (void)d_serve(&d_stop_context, 0);
    d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_RUNNING);
    d_store(&d_block->flag, 0);
    InterlockedExchange(&d_owner, D_OWNER_NONE);
}

/* Serves a command received while the process runs. */
static void d_handle_running(void)
{
    const uint32_t sequence = d_load(&d_block->request);
    const int32_t  command  = (int32_t)d_load((volatile uint32_t*)&d_block->cmd);
    d_served_seq = sequence;
    d_block->status = SLOPKIT_AGENT_STATUS_OK;

    switch (command)
    {
    case SLOPKIT_AGENT_DBG_CMD_STOP:
        d_enumerate_threads();
        d_freeze_stop(SLOPKIT_AGENT_DBG_STOP_INTERRUPT, sequence);
        return;
    case SLOPKIT_AGENT_DBG_CMD_ARM_SOFTWARE:
        EnterCriticalSection(&d_lock);
        if (!d_trap_arm(d_block->arg[0], (uint32_t)d_block->arg[1]))
        {
            d_block->status = SLOPKIT_AGENT_STATUS_UNSUPPORTED;
        }
        LeaveCriticalSection(&d_lock);
        break;
    case SLOPKIT_AGENT_DBG_CMD_DISARM_SOFTWARE:
        EnterCriticalSection(&d_lock);
        (void)d_trap_disarm((uint32_t)d_block->arg[0]);
        LeaveCriticalSection(&d_lock);
        break;
    case SLOPKIT_AGENT_DBG_CMD_ARM_HARDWARE:
        EnterCriticalSection(&d_lock);
        if (!d_hardware_arm(d_block->arg[0], (uint32_t)d_block->arg[1], (uint32_t)d_block->arg[2], (uint32_t)d_block->arg[3]))
        {
            d_block->status = SLOPKIT_AGENT_STATUS_UNSUPPORTED;
        }
        else
        {
            d_reprogram_running();
        }
        LeaveCriticalSection(&d_lock);
        break;
    case SLOPKIT_AGENT_DBG_CMD_DISARM_HARDWARE:
        EnterCriticalSection(&d_lock);
        (void)d_hardware_disarm((uint32_t)d_block->arg[0]);
        d_reprogram_running();
        LeaveCriticalSection(&d_lock);
        break;
    default:
        d_block->status = SLOPKIT_AGENT_STATUS_UNKNOWN_OP;
        break;
    }
    d_answer(sequence);
}

/* ------------------------------------------------------------------ */
/* Detach                                                              */
/* ------------------------------------------------------------------ */

static void d_unpatch_all(void)
{
    for (uint32_t index = 0; index < d_block->trap_count; ++index)
    {
        if (d_block->trap[index].armed != 0)
        {
            d_write_byte(d_block->trap[index].address, d_block->trap[index].saved_byte);
            d_block->trap[index].armed = 0;
        }
    }
}

static int d_detach_internal(int from_veh)
{
    if (from_veh == 0)
    {
        d_freeze_all(0);
    }
    d_unpatch_all();
    d_block->hardware_count = 0;
    if (from_veh == 0)
    {
        d_reprogram_frozen();
        d_thaw_all();
    }
    else
    {
        for (uint32_t index = 0; index < d_thread_count; ++index)
        {
            if (d_threads[index].suspended != 0)
            {
                d_program_thread(d_threads[index].handle);
            }
        }
    }
    d_thaw_all();
    if (d_veh != NULL)
    {
        RemoveVectoredExceptionHandler(d_veh);
        d_veh = NULL;
    }
    InterlockedExchange(&d_watchdog_stop, 1);
    InterlockedExchange(&d_owner, D_OWNER_NONE);
    d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_DETACHED);
    d_store(&d_block->flag, 0);
    d_close_threads();
    return 0;
}

/* ------------------------------------------------------------------ */
/* The vectored exception handler                                      */
/* ------------------------------------------------------------------ */

static LONG CALLBACK slopkit_debug_veh(EXCEPTION_POINTERS* info)
{
    if (d_block == NULL)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* const  context = info->ContextRecord;
    const DWORD     code    = info->ExceptionRecord->ExceptionCode;

    if (code == EXCEPTION_BREAKPOINT)
    {
        const uint64_t address = d_ptr(info->ExceptionRecord->ExceptionAddress);
        const int      index   = d_find_trap_address(address);
        if (index >= 0)
        {
            /* Our trap: rewind past the 0xCC and restore the real byte so the
               stopped process reads as the target sees it. */
            D_CTX_RIP(context) = (D_CTX_WORD)address;
            d_write_byte(address, d_block->trap[index].saved_byte);
            d_block->trap[index].armed = 0;

            InterlockedExchange(&d_owner, D_OWNER_VEH);
            d_publish((uint32_t)GetCurrentThreadId(),
                      d_self_linux_tid_cached(),
                      SLOPKIT_AGENT_DBG_STOP_TRAP,
                      d_block->trap[index].slot,
                      address,
                      address,
                      context);
            d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_STOPPED);
            (void)d_serve(context, 1);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (d_load(&d_block->state) == SLOPKIT_AGENT_DBG_STATE_STOPPED)
        {
            /* A stopped target must not swallow the debugger with its own
               first-chance churn. */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (code == EXCEPTION_SINGLE_STEP)
    {
        if (InterlockedCompareExchange(&d_tf_owner, 0, 0) != 0)
        {
            InterlockedExchange(&d_tf_owner, 0);
            if (d_rearm_address != 0)
            {
                d_write_byte(d_rearm_address, 0xCC);
                const int index = d_find_trap_address(d_rearm_address);
                if (index >= 0)
                {
                    d_block->trap[index].armed = 1;
                }
                d_rearm_address = 0;
            }
            D_CTX_FLAGS(context) &= ~0x100u;
            context->Dr6 = 0;

            if (InterlockedCompareExchange(&d_step_publish, 0, 0) != 0)
            {
                InterlockedExchange(&d_step_publish, 0);
                InterlockedExchange(&d_owner, D_OWNER_VEH);
                d_publish((uint32_t)GetCurrentThreadId(),
                          d_self_linux_tid_cached(),
                          SLOPKIT_AGENT_DBG_STOP_STEP,
                          0xFFFFFFFFu,
                          (uint64_t)D_CTX_RIP(context),
                          (uint64_t)D_CTX_RIP(context),
                          context);
                d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_STOPPED);
                (void)d_serve(context, 1);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (context->Dr6 != 0)
        {
            const uint32_t slot = d_slot_from_dr6(context->Dr6);
            uint64_t       address = 0;
            const int      index   = slot < SLOPKIT_AGENT_DEBUG_DR_SLOTS ? d_find_hardware_slot(slot) : -1;
            if (index >= 0)
            {
                address = d_block->hardware[index].address;
            }
            context->Dr6 = 0;
            InterlockedExchange(&d_owner, D_OWNER_VEH);
            d_publish((uint32_t)GetCurrentThreadId(),
                      d_self_linux_tid_cached(),
                      SLOPKIT_AGENT_DBG_STOP_TRAP,
                      slot,
                      address,
                      address,
                      context);
            d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_STOPPED);
            (void)d_serve(context, 1);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (d_load(&d_block->state) == SLOPKIT_AGENT_DBG_STATE_STOPPED)
        {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedCompareExchange(&d_owner, 0, 0) != D_OWNER_NONE
        || d_load(&d_block->state) == SLOPKIT_AGENT_DBG_STATE_STOPPED)
    {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------------ */
/* The watchdog                                                        */
/* ------------------------------------------------------------------ */

static DWORD WINAPI d_watchdog_main(LPVOID parameter)
{
    (void)parameter;
    slopkit_debug_note_agent_thread();
    InterlockedExchange(&d_watchdog_noted, 1);
    for (;;)
    {
        if (InterlockedCompareExchange(&d_watchdog_stop, 0, 0) != 0)
        {
            return 0;
        }
        const uint32_t state   = d_load(&d_block->state);
        const uint32_t request = d_load(&d_block->request);
        const int      owner   = (int)InterlockedCompareExchange(&d_owner, 0, 0);

        if (state == SLOPKIT_AGENT_DBG_STATE_RUNNING && request != d_served_seq)
        {
            EnterCriticalSection(&d_lock);
            d_handle_running();
            LeaveCriticalSection(&d_lock);
            continue;
        }
        if (state == SLOPKIT_AGENT_DBG_STATE_STOPPED && owner == D_OWNER_WATCHDOG && request != d_served_seq)
        {
            d_served_seq = request;
            EnterCriticalSection(&d_lock);
            const int result = d_handle_stopped(&d_stop_context);
            LeaveCriticalSection(&d_lock);
            if (result == D_RELEASE_DETACH)
            {
                (void)d_detach_internal(0);
            }
            else if (result != D_RELEASE_SERVE)
            {
                SetThreadContext(d_leader_handle, &d_stop_context);
                d_resume_leader();
                d_thaw_all();
                InterlockedExchange(&d_owner, D_OWNER_NONE);
                d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_RUNNING);
                d_store(&d_block->flag, 0);
            }
            continue;
        }
        Sleep(1);
    }
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

void slopkit_debug_reset(slopkit_agent_region* region)
{
    d_region             = region;
    d_block              = region != NULL ? &region->debug : NULL;
    d_veh                = NULL;
    d_watchdog           = NULL;
    d_agent_tid_count    = 0;
    d_watchdog_stop      = 1;
    d_owner              = D_OWNER_NONE;
    d_thread_count       = 0;
    d_leader_handle      = NULL;
    d_leader_win_tid     = 0;
    d_leader_linux_tid   = 0;
    d_served_seq         = 0;
    d_rearm_address      = 0;
    d_step_publish       = 0;
    d_tf_owner           = 0;
    d_lock_ready         = 0;
    if (d_block != NULL)
    {
        d_block->state    = SLOPKIT_AGENT_DBG_STATE_IDLE;
        d_block->flag     = 0;
        d_block->request  = 0;
        d_block->answer   = 0;
        d_block->cmd      = 0;
        d_block->status   = SLOPKIT_AGENT_STATUS_OK;
        d_block->trap_count     = 0;
        d_block->hardware_count = 0;
    }
}

int slopkit_debug_attach(slopkit_agent_region* region)
{
    if (region == NULL)
    {
        return 1;
    }
    d_region = region;
    d_block  = &region->debug;
    if (d_block->size != (uint32_t)sizeof(slopkit_agent_debug)
        || d_block->offset != (uint32_t)offsetof(slopkit_agent_region, debug))
    {
        return 1;
    }

    if (d_lock_ready == 0)
    {
        InitializeCriticalSection(&d_lock);
        d_lock_ready = 1;
    }
    d_watchdog_stop    = 0;
    d_owner            = D_OWNER_NONE;
    d_served_seq       = d_load(&d_block->request);
    d_rearm_address    = 0;
    d_step_publish     = 0;
    d_tf_owner         = 0;
    d_block->status    = SLOPKIT_AGENT_STATUS_OK;
    d_block->win32_error = 0;
    d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_STOPPING);

    /* The host may have pre-armed software breakpoints before attach. */
    for (uint32_t index = 0; index < d_block->trap_count; ++index)
    {
        slopkit_agent_debug_trap* const trap = &d_block->trap[index];
        trap->armed                          = 0;
        trap->saved_byte                     = d_read_byte(trap->address);
        if (!d_write_byte(trap->address, 0xCC))
        {
            trap->armed = 0;
        }
        else
        {
            trap->armed = 1;
        }
    }

    d_veh = AddVectoredExceptionHandler(1, slopkit_debug_veh);
    if (d_veh == NULL)
    {
        d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_DETACHED);
        return 1;
    }
    d_watchdog = CreateThread(NULL, 0, d_watchdog_main, NULL, 0, NULL);
    /* Wait for the watchdog to register itself, so the enumeration below does
       not freeze the very thread that serves the stop. */
    for (uint32_t spin = 0; spin < 2000 && InterlockedCompareExchange(&d_watchdog_noted, 0, 0) == 0; ++spin)
    {
        Sleep(1);
    }

    slopkit_debug_note_agent_thread();
    d_enumerate_threads();
    d_freeze_all(0);
    for (uint32_t index = 0; index < d_thread_count; ++index)
    {
        d_probe_thread(&d_threads[index]);
    }
    d_reprogram_frozen();

    const uint32_t leader = d_pick_leader();
    if (leader == 0xFFFFFFFFu)
    {
        /* Nothing to stop (the process has no other threads): report an empty
           interrupt stop and let the host drive from here. */
        InterlockedExchange(&d_owner, D_OWNER_WATCHDOG);
        d_leader_handle = NULL;
        d_publish((uint32_t)GetCurrentThreadId(),
                  d_self_linux_tid_cached(),
                  SLOPKIT_AGENT_DBG_STOP_INTERRUPT,
                  0xFFFFFFFFu,
                  0,
                  0,
                  NULL);
        d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_STOPPED);
        d_answer(d_load(&d_block->request));
        return 0;
    }

    memset(&d_stop_context, 0, sizeof(d_stop_context));
    d_stop_context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(d_threads[leader].handle, &d_stop_context))
    {
        d_thaw_all();
        d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_DETACHED);
        return 1;
    }
    d_leader_handle    = d_threads[leader].handle;
    d_leader_win_tid   = d_threads[leader].win_tid;
    d_leader_linux_tid = d_threads[leader].linux_tid;

    InterlockedExchange(&d_owner, D_OWNER_WATCHDOG);
    d_publish(d_leader_win_tid,
              d_leader_linux_tid,
              SLOPKIT_AGENT_DBG_STOP_INTERRUPT,
              0xFFFFFFFFu,
              (uint64_t)D_CTX_RIP(&d_stop_context),
              (uint64_t)D_CTX_RIP(&d_stop_context),
              &d_stop_context);
    d_store(&d_block->state, SLOPKIT_AGENT_DBG_STATE_STOPPED);
    /* The attach request itself needs no answer: the host waits for the flag.
       Still, publishing an answer sequence keeps the block consistent. */
    d_answer(d_load(&d_block->request));
    return 0;
}

int slopkit_debug_detach(slopkit_agent_region* region)
{
    if (region == NULL || d_block == NULL)
    {
        return 1;
    }
    return d_detach_internal(0);
}
