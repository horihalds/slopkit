#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "slopkit_agent_protocol.h"

/* Polling: a short spin for a host that is actively waiting, then a Sleep()
   backoff that grows to a ceiling, so an idle or abandoned agent costs almost
   no CPU. A heartbeat change from the host resets the backoff to the spin. */
#define SLOPKIT_AGENT_SPIN_LIMIT   64u
#define SLOPKIT_AGENT_SLEEP_MIN_MS 1u
#define SLOPKIT_AGENT_SLEEP_MAX_MS 64u

/* A pause hint for the spin phase; the agent never calls Rtl* directly so it
   needs no import beyond kernel32. */
static void agent_pause(void)
{
    YieldProcessor();
}

/* -nostdlib leaves no CRT, but the compiler still emits calls to these for a
   struct assignment or a zero initialiser, so they are provided here. */
void* memset(void* destination, int value, size_t count)
{
    unsigned char* bytes = (unsigned char*)destination;
    size_t         index = 0;
    for (; index < count; ++index)
    {
        bytes[index] = (unsigned char)value;
    }
    return destination;
}

void* memcpy(void* destination, const void* source, size_t count)
{
    unsigned char*       out   = (unsigned char*)destination;
    const unsigned char* in    = (const unsigned char*)source;
    size_t               index = 0;
    for (; index < count; ++index)
    {
        out[index] = in[index];
    }
    return destination;
}

void* memmove(void* destination, const void* source, size_t count)
{
    unsigned char*       out = (unsigned char*)destination;
    const unsigned char* in  = (const unsigned char*)source;
    if (out == in || count == 0)
    {
        return destination;
    }
    if (out < in)
    {
        size_t index = 0;
        for (; index < count; ++index)
        {
            out[index] = in[index];
        }
    }
    else
    {
        size_t index = count;
        while (index > 0)
        {
            --index;
            out[index] = in[index];
        }
    }
    return destination;
}

/* The wide-string helpers the CRT would otherwise provide. */
static size_t agent_wide_length(const wchar_t* text)
{
    size_t length = 0;
    while (text[length] != L'\0')
    {
        ++length;
    }
    return length;
}

static wchar_t* agent_wide_copy(wchar_t* destination, const wchar_t* source)
{
    wchar_t* cursor = destination;
    while ((*cursor = *source) != L'\0')
    {
        ++cursor;
        ++source;
    }
    return destination;
}

static wchar_t* agent_wide_last(wchar_t* text, wchar_t needle)
{
    wchar_t* found = NULL;
    for (; *text != L'\0'; ++text)
    {
        if (*text == needle)
        {
            found = text;
        }
    }
    return found;
}

/* What the agent allocated, so FREE/PROTECT can refuse a foreign address and
   SHUTDOWN can release everything. Several workers touch the list, so every
   walk and mutation runs under g_lock. */
typedef struct slopkit_agent_allocation
{
    uint64_t                         base;
    uint64_t                         size;
    struct slopkit_agent_allocation* next;
} slopkit_agent_allocation;

/* A live marker each agent thread keeps on its own stack while it serves. The
   host scans the stack window below a thread's stack top for `magic` to tell
   the agent's threads from the target's own without a ptrace window. */
typedef struct slopkit_agent_marker
{
    volatile uint64_t magic;
    volatile uint64_t tid;
} slopkit_agent_marker;

static HMODULE                   g_instance;
static HANDLE                    g_mapping;
static slopkit_agent_region*     g_region;
static slopkit_agent_allocation* g_allocations;

/* The dispatcher owns the region and the lifecycle; the workers only serve the
   slots it hands them. g_lock guards the allocation list, the slot queue and
   the claimed flags; the events wake a worker and end every loop. */
static CRITICAL_SECTION g_lock;
static uint32_t         g_claimed[SLOPKIT_AGENT_SLOT_COUNT];
static uint32_t         g_queue[SLOPKIT_AGENT_SLOT_COUNT];
static uint32_t         g_queue_count;
static HANDLE           g_work_event;
static HANDLE           g_stop_event;
static uint32_t         g_workers_started;
static HANDLE           g_workers[SLOPKIT_AGENT_MAX_WORKERS - 1];

/* Writes the marker each loop iteration so the compiler cannot fold the frame
   away and the host always finds a live value on this thread's stack. */
static void agent_mark(slopkit_agent_marker* marker)
{
    marker->magic = SLOPKIT_AGENT_THREAD_MAGIC;
    marker->tid   = (uint64_t)(uintptr_t)GetCurrentThreadId();
}

/* Best-effort thread name: SetThreadDescription is not on every Wine build, so
   it is resolved at runtime and its absence simply leaves the stack marker as
   the only identification path. Split writes keep -Wcast-function-type quiet. */
typedef HRESULT(WINAPI* agent_set_thread_description)(HANDLE, PCWSTR);

static void agent_name_thread(void)
{
    static const wchar_t name[] = L"slopkit-agent";
    HMODULE              kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == NULL)
    {
        return;
    }
    FARPROC raw = GetProcAddress(kernel, "SetThreadDescription");
    union
    {
        FARPROC                     raw;
        agent_set_thread_description typed;
    } set;
    set.raw = raw;
    if (set.typed != NULL)
    {
        set.typed(GetCurrentThread(), name);
    }
}

static slopkit_agent_allocation* agent_find_exact(uint64_t base)
{
    slopkit_agent_allocation* found = NULL;
    EnterCriticalSection(&g_lock);
    for (slopkit_agent_allocation* node = g_allocations; node != NULL; node = node->next)
    {
        if (node->base == base)
        {
            found = node;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

/* The allocation that wholly contains [address, address + size), or NULL. */
static slopkit_agent_allocation* agent_find_containing(uint64_t address, uint64_t size)
{
    slopkit_agent_allocation* found = NULL;
    EnterCriticalSection(&g_lock);
    for (slopkit_agent_allocation* node = g_allocations; node != NULL; node = node->next)
    {
        if (address >= node->base && size <= node->size && address - node->base <= node->size - size)
        {
            found = node;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

static int agent_track(uint64_t base, uint64_t size)
{
    slopkit_agent_allocation* node =
        (slopkit_agent_allocation*)HeapAlloc(GetProcessHeap(), 0, sizeof(slopkit_agent_allocation));
    if (node == NULL)
    {
        return 0;
    }
    node->base = base;
    node->size = size;
    EnterCriticalSection(&g_lock);
    node->next    = g_allocations;
    g_allocations = node;
    LeaveCriticalSection(&g_lock);
    return 1;
}

static void agent_untrack(slopkit_agent_allocation* node)
{
    EnterCriticalSection(&g_lock);
    slopkit_agent_allocation** link = &g_allocations;
    while (*link != NULL)
    {
        if (*link == node)
        {
            *link = node->next;
            LeaveCriticalSection(&g_lock);
            HeapFree(GetProcessHeap(), 0, node);
            return;
        }
        link = &(*link)->next;
    }
    LeaveCriticalSection(&g_lock);
}

/* Only the dispatcher runs this, after every worker has stopped, so no slot is
   being served and no other thread touches the list. */
static void agent_free_all(void)
{
    for (;;)
    {
        slopkit_agent_allocation* node = NULL;
        EnterCriticalSection(&g_lock);
        node          = g_allocations;
        g_allocations = node != NULL ? node->next : NULL;
        LeaveCriticalSection(&g_lock);
        if (node == NULL)
        {
            return;
        }
        VirtualFree((LPVOID)(uintptr_t)node->base, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, node);
    }
}

/* Open the shared region as the sibling of this DLL: <dir>/slopkit-agent.bin.
   Returns 1 on success and leaves a usable g_region with a matching magic. */
static int agent_open_region(void)
{
    wchar_t module_path[MAX_PATH];
    if (GetModuleFileNameW(g_instance, module_path, MAX_PATH) == 0)
    {
        return 0;
    }
    wchar_t* separator = agent_wide_last(module_path, L'\\');
    if (separator == NULL)
    {
        return 0;
    }
    separator[1]      = L'\0';
    const size_t used = agent_wide_length(module_path);
    const size_t name = agent_wide_length(SLOPKIT_AGENT_SHM_FILE_W);
    if (used + name + 1 > MAX_PATH)
    {
        return 0;
    }
    agent_wide_copy(module_path + used, SLOPKIT_AGENT_SHM_FILE_W);

    HANDLE file = CreateFileW(module_path,
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    g_mapping = CreateFileMappingW(file, NULL, PAGE_READWRITE, 0, 0, NULL);
    CloseHandle(file);
    if (g_mapping == NULL)
    {
        return 0;
    }
    g_region = (slopkit_agent_region*)MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(slopkit_agent_region));
    if (g_region == NULL)
    {
        CloseHandle(g_mapping);
        g_mapping = NULL;
        return 0;
    }
    return 1;
}

static void agent_close_region(void)
{
    if (g_region != NULL)
    {
        UnmapViewOfFile(g_region);
        g_region = NULL;
    }
    if (g_mapping != NULL)
    {
        CloseHandle(g_mapping);
        g_mapping = NULL;
    }
}

/* <windows.h> spells the state/flag atomics as LONG, which is 32 bits. */
static uint32_t agent_load_slot_state(uint32_t index)
{
    return (uint32_t)InterlockedCompareExchange((volatile LONG*)&g_region->slots[index].state,
                                                (LONG)SLOPKIT_AGENT_SLOT_IDLE,
                                                (LONG)SLOPKIT_AGENT_SLOT_IDLE);
}

static uint64_t agent_granularity(UINT* pointer_size, uint64_t* minimum, uint64_t* maximum)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (pointer_size != NULL)
    {
        *pointer_size = info.dwPageSize;
    }
    if (minimum != NULL)
    {
        *minimum = (uint64_t)(uintptr_t)info.lpMinimumApplicationAddress;
    }
    if (maximum != NULL)
    {
        *maximum = (uint64_t)(uintptr_t)info.lpMaximumApplicationAddress;
    }
    return info.dwAllocationGranularity != 0 ? info.dwAllocationGranularity : 0x10000u;
}

static void* agent_try_commit(uint64_t address, uint64_t size)
{
    if (address == 0)
    {
        return NULL;
    }
    return VirtualAlloc((LPVOID)(uintptr_t)address, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

/* The nearest usable base at or below `from` inside [lowest, from]. */
static void* agent_search_down(uint64_t from, uint64_t size, uint64_t lowest, uint64_t granularity)
{
    uint64_t cursor = from & ~(granularity - 1);
    while (cursor >= lowest && cursor != 0)
    {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((LPCVOID)(uintptr_t)cursor, &region, sizeof(region)) != sizeof(region))
        {
            return NULL;
        }
        const uint64_t base = (uint64_t)(uintptr_t)region.BaseAddress;
        const uint64_t end  = base + (uint64_t)region.RegionSize;
        if (end > base && region.State == MEM_FREE)
        {
            /* The highest granularity-aligned base in this free region that
               still leaves room for `size` and does not overshoot `cursor`. */
            uint64_t candidate = (end >= size ? end - size : 0) & ~(granularity - 1);
            if (candidate > cursor)
            {
                candidate = cursor;
            }
            if (candidate >= base && candidate >= lowest && candidate != 0)
            {
                void* committed = agent_try_commit(candidate, size);
                if (committed != NULL)
                {
                    return committed;
                }
            }
        }
        if (base == 0 || base <= lowest)
        {
            return NULL;
        }
        cursor = (base - 1) & ~(granularity - 1);
    }
    return NULL;
}

/* The nearest usable base at or above `from` inside [from, highest]. */
static void* agent_search_up(uint64_t from, uint64_t size, uint64_t highest, uint64_t granularity)
{
    uint64_t cursor = from;
    while (cursor <= highest && cursor != 0)
    {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((LPCVOID)(uintptr_t)cursor, &region, sizeof(region)) != sizeof(region))
        {
            return NULL;
        }
        const uint64_t base = (uint64_t)(uintptr_t)region.BaseAddress;
        const uint64_t end  = base + (uint64_t)region.RegionSize;
        if (end <= cursor)
        {
            return NULL;
        }
        if (region.State == MEM_FREE)
        {
            uint64_t candidate = (base > cursor ? base : cursor);
            candidate          = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate >= base && candidate <= highest && end - candidate >= size)
            {
                void* committed = agent_try_commit(candidate, size);
                if (committed != NULL)
                {
                    return committed;
                }
            }
        }
        cursor = end;
    }
    return NULL;
}

/* The ±2 GB search the ptrace path performs, in the same order: the hinted
   page, then the closest free gap below it, then above it. */
static void* agent_alloc_near(uint64_t size, uint64_t target)
{
    uint64_t       minimum     = 0;
    uint64_t       maximum     = 0;
    const uint64_t granularity = agent_granularity(NULL, &minimum, &maximum);
    if (granularity == 0 || target == 0)
    {
        return NULL;
    }
    const uint64_t span  = 0x80000000ull;
    const uint64_t start = target & ~(granularity - 1);

    if (start >= minimum && start <= maximum)
    {
        void* committed = agent_try_commit(start, size);
        if (committed != NULL)
        {
            return committed;
        }
    }

    uint64_t lowest = target > span ? target - span : minimum;
    if (lowest < minimum)
    {
        lowest = minimum;
    }
    if (start > granularity)
    {
        void* committed = agent_search_down(start - granularity, size, lowest, granularity);
        if (committed != NULL)
        {
            return committed;
        }
    }

    uint64_t highest = target > (UINT64_MAX - span) ? maximum : target + span;
    if (highest > maximum)
    {
        highest = maximum;
    }
    return agent_search_up(start + granularity, size, highest, granularity);
}

static void agent_fill_error(slopkit_agent_record* response, DWORD win32_error)
{
    response->status      = SLOPKIT_AGENT_STATUS_ERROR;
    response->win32_error = (uint32_t)win32_error;
}

static void agent_handle_hello(slopkit_agent_record* response)
{
    uint32_t       page        = 0;
    uint64_t       ignored_min = 0;
    uint64_t       ignored_max = 0;
    const uint64_t granularity = agent_granularity(&page, &ignored_min, &ignored_max);
    response->protocol_version = SLOPKIT_AGENT_PROTOCOL_VERSION;
    response->record_size      = (uint32_t)sizeof(slopkit_agent_record);
    response->pointer_size     = (uint32_t)sizeof(void*);
    response->windows_pid      = (uint32_t)GetCurrentProcessId();
    response->granularity      = (uint32_t)granularity;
    response->agent_base       = (uint64_t)(uintptr_t)g_instance;
    /* The host sizes its slot pool from these before it serves anything. */
    response->slot_count       = SLOPKIT_AGENT_SLOT_COUNT;
    response->worker_count     = SLOPKIT_AGENT_MAX_WORKERS;
    /* The host refuses a different payload capacity before trusting the area. */
    response->reserved0        = SLOPKIT_AGENT_PAYLOAD_SIZE;
}

/* The Windows loader structures the MODULES walk needs, declared locally
   rather than through <winternl.h>: the DLL includes <windows.h> only, and the
   member spelling of mingw-w64's winternl.h is not stable across releases.
   Only the fields the walk reads are declared, in their documented order; the
   pointer and natural alignment of each puts every field at its real offset on
   both x86_64 and i686. */
typedef struct slopkit_agent_plist_entry
{
    struct slopkit_agent_plist_entry* flink;
    struct slopkit_agent_plist_entry* blink;
} slopkit_agent_plist_entry;

/* PEB_LDR_DATA up to and including InLoadOrderModuleList. */
typedef struct slopkit_agent_ploader_data
{
    uint32_t                  length;
    uint32_t                  initialized;
    void*                     ss_handle;
    slopkit_agent_plist_entry in_load_order_module_list;
} slopkit_agent_ploader_data;

/* UNICODE_STRING: Length and MaximumLength are byte counts. */
typedef struct slopkit_agent_punicode_string
{
    uint16_t length;
    uint16_t maximum_length;
    void*    buffer;
} slopkit_agent_punicode_string;

/* LDR_DATA_TABLE_ENTRY up to and including BaseDllName. */
typedef struct slopkit_agent_ploader_module_entry
{
    slopkit_agent_plist_entry     in_load_order_links;
    slopkit_agent_plist_entry     in_memory_order_links;
    slopkit_agent_plist_entry     in_initialization_order_links;
    void*                         dll_base;
    void*                         entry_point;
    uint32_t                      size_of_image;
    slopkit_agent_punicode_string full_dll_name;
    slopkit_agent_punicode_string base_dll_name;
} slopkit_agent_ploader_module_entry;

/* PEB up to and including Ldr; the natural alignment of the pointer members
   keeps the offsets right on both bitnesses. */
typedef struct slopkit_agent_ppeb
{
    uint8_t                     inherited_address_space;
    uint8_t                     read_image_file_exec_options;
    uint8_t                     being_debugged;
    uint8_t                     bit_field;
    void*                       mutant;
    void*                       image_base_address;
    slopkit_agent_ploader_data* ldr;
} slopkit_agent_ppeb;

/* TEB up to and including ProcessEnvironmentBlock: the reserved pointers stand
   in for the members the walk does not read, so the field lands at 0x60 on
   x86_64 and 0x30 on i686. */
typedef struct slopkit_agent_pteb
{
    void*               reserved[12];
    slopkit_agent_ppeb* process_environment_block;
} slopkit_agent_pteb;

/* A hard ceiling on how many loader entries one MODULES request visits past
   its resume point, so a corrupt or cyclic list cannot spin the service
   thread. */
#define SLOPKIT_AGENT_MODULE_LIMIT 4096u

static slopkit_agent_ploader_data* agent_loader_data(void)
{
    const slopkit_agent_pteb* teb = (const slopkit_agent_pteb*)NtCurrentTeb();
    if (teb == NULL || teb->process_environment_block == NULL)
    {
        return NULL;
    }
    return teb->process_environment_block->ldr;
}

/* The bytes the downward name cursor needs for a name of `length` bytes,
   keeping the UTF-16 name on a two-byte boundary. */
static uint32_t agent_name_span(uint32_t length)
{
    return (length + 1u) & ~1u;
}

/* MODULES: walk the target loader's InLoadOrderModuleList from the request
   index and report every loaded PE image. Records are written at the start of
   payload[] in loader order and each name's UTF-16 bytes are appended from the
   end of the payload downwards, so no fixed name budget is needed. The walk is
   bounded by a fixed ceiling and by the payload, and it stops early rather
   than faulting on a malformed or concurrently-changing list. */
static void agent_handle_modules(const slopkit_agent_record* request, slopkit_agent_record* response, uint8_t* payload)
{
    const uint32_t capacity = (uint32_t)(SLOPKIT_AGENT_PAYLOAD_SIZE / sizeof(slopkit_agent_module_entry));
    if (request->size > capacity || request->address > (uint64_t)-1 - SLOPKIT_AGENT_MODULE_LIMIT)
    {
        response->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        return;
    }
    const uint32_t want  = request->size == 0 ? capacity : (uint32_t)request->size;
    const uint64_t start = request->address;

    slopkit_agent_ploader_data* ldr = agent_loader_data();
    if (ldr == NULL || ldr->in_load_order_module_list.flink == &ldr->in_load_order_module_list)
    {
        /* No loader list to walk: an empty, finished listing. */
        response->size    = 0;
        response->address = 0;
        return;
    }

    slopkit_agent_plist_entry*  head        = &ldr->in_load_order_module_list;
    slopkit_agent_module_entry* records     = (slopkit_agent_module_entry*)payload;
    uint32_t                    name_cursor = SLOPKIT_AGENT_PAYLOAD_SIZE;
    uint64_t                    index       = 0;
    uint32_t                    written     = 0;
    uint64_t                    visited     = 0;
    const uint64_t              visit_limit = start + SLOPKIT_AGENT_MODULE_LIMIT;

    slopkit_agent_plist_entry* node = head->flink;
    while (node != head && node != NULL && visited < visit_limit)
    {
        ++visited;
        const slopkit_agent_ploader_module_entry* module = (const slopkit_agent_ploader_module_entry*)node;
        node                                            = node->flink;

        if (module->dll_base == NULL)
        {
            /* The list also carries entries without an image base, which a
               module view has nothing to say about. */
            continue;
        }
        if (index < start)
        {
            ++index;
            continue;
        }
        if (written == want)
        {
            break;
        }

        const uint32_t name_bytes  = module->base_dll_name.buffer == NULL ? 0u : (uint32_t)module->base_dll_name.length;
        uint32_t       name_offset = 0;
        if (name_bytes != 0)
        {
            const uint32_t span = agent_name_span(name_bytes);
            if (span > name_cursor
                || name_cursor - span < (written + 1u) * (uint32_t)sizeof(slopkit_agent_module_entry))
            {
                /* The record array and the downward name area would collide. */
                break;
            }
            name_offset = name_cursor - span;
            name_cursor = name_offset;
            memcpy(payload + name_offset, module->base_dll_name.buffer, name_bytes);
        }

        records[written].base        = (uint64_t)(uintptr_t)module->dll_base;
        records[written].size        = (uint64_t)module->size_of_image;
        records[written].entry       = (uint64_t)(uintptr_t)module->entry_point;
        records[written].name_offset = name_offset;
        records[written].name_length = name_bytes;
        records[written].flags       = index == 0 ? SLOPKIT_AGENT_MODULE_FLAG_MAIN : 0u;
        records[written].reserved    = 0;
        ++written;
        ++index;
    }

    response->size    = written;
    response->address = node == head ? 0 : index;
}

/* ENUM: walk the target's own VirtualQuery chain from the request cursor,
   writing up to `size` (0 = the whole payload) region records into payload[].
   Progress stops at the capacity, at the end of the user address space, or on
   a failed VirtualQuery; the next cursor is reported in address, 0 when the
   walk reached the end. */
static void agent_handle_enum(const slopkit_agent_record* request, slopkit_agent_record* response, uint8_t* payload)
{
    const uint32_t capacity = (uint32_t)(SLOPKIT_AGENT_PAYLOAD_SIZE / sizeof(slopkit_agent_region_entry));
    if (request->size > capacity)
    {
        response->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        return;
    }
    const uint32_t want = request->size == 0 ? capacity : (uint32_t)request->size;

    uint64_t minimum = 0;
    uint64_t maximum = 0;
    agent_granularity(NULL, &minimum, &maximum);

    slopkit_agent_region_entry* entries = (slopkit_agent_region_entry*)payload;
    uint64_t                    cursor  = request->address;
    if (cursor == 0)
    {
        /* A zero cursor means "start from the beginning": the response reports
           0 only once the walk has reached the end of the address space. */
        cursor = minimum;
    }
    uint32_t written = 0;
    while (written < want && cursor != 0 && cursor <= maximum)
    {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((LPCVOID)(uintptr_t)cursor, &region, sizeof(region)) != sizeof(region))
        {
            agent_fill_error(response, GetLastError());
            return;
        }
        const uint64_t base = (uint64_t)(uintptr_t)region.BaseAddress;
        const uint64_t size = (uint64_t)region.RegionSize;
        const uint64_t end  = base + size;
        if (size == 0 || end <= cursor)
        {
            /* A zero-size or backwards region would never advance the cursor,
               so the walk stops as if the address space had ended. */
            cursor = 0;
            break;
        }
        entries[written].base       = base;
        entries[written].size       = size;
        entries[written].state      = (uint32_t)region.State;
        entries[written].protection = (uint32_t)region.Protect;
        entries[written].type       = (uint32_t)region.Type;
        entries[written].reserved   = 0;
        ++written;
        cursor = end > maximum ? 0 : end;
    }
    response->size    = written;
    response->address = cursor;
}

static void agent_handle_alloc(const slopkit_agent_record* request, slopkit_agent_record* response)
{
    if (request->size == 0 || request->size > (uint64_t)(SIZE_T)-1)
    {
        response->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        return;
    }
    void* base = NULL;
    if (request->hint != 0)
    {
        base = agent_try_commit(request->hint, request->size);
    }
    if (base == NULL)
    {
        base = VirtualAlloc(NULL, (SIZE_T)request->size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (base == NULL)
    {
        agent_fill_error(response, GetLastError());
        return;
    }
    if (!agent_track((uint64_t)(uintptr_t)base, request->size))
    {
        VirtualFree(base, 0, MEM_RELEASE);
        agent_fill_error(response, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    response->address = (uint64_t)(uintptr_t)base;
    response->size    = request->size;
}

static void agent_handle_alloc_near(const slopkit_agent_record* request, slopkit_agent_record* response)
{
    if (request->size == 0 || request->size > (uint64_t)(SIZE_T)-1)
    {
        response->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        return;
    }
    if (request->hint == 0)
    {
        response->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        return;
    }
    void* base = agent_alloc_near(request->size, request->hint);
    if (base == NULL)
    {
        /* A near miss is not a hard error: the caller's own contract allows a
           hint to be dropped when no gap exists, and the host reports "no
           address in range" from the status. */
        response->status      = SLOPKIT_AGENT_STATUS_UNSUPPORTED;
        response->win32_error = (uint32_t)GetLastError();
        return;
    }
    if (!agent_track((uint64_t)(uintptr_t)base, request->size))
    {
        VirtualFree(base, 0, MEM_RELEASE);
        agent_fill_error(response, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    response->address = (uint64_t)(uintptr_t)base;
    response->size    = request->size;
}

static void agent_handle_free(const slopkit_agent_record* request, slopkit_agent_record* response)
{
    slopkit_agent_allocation* node = agent_find_exact(request->address);
    if (request->address == 0 || node == NULL)
    {
        response->status      = SLOPKIT_AGENT_STATUS_NOT_OWNED;
        response->win32_error = (uint32_t)ERROR_INVALID_ADDRESS;
        return;
    }
    if (!VirtualFree((LPVOID)(uintptr_t)node->base, 0, MEM_RELEASE))
    {
        agent_fill_error(response, GetLastError());
        return;
    }
    agent_untrack(node);
}

static int agent_is_page_protection(uint32_t protection)
{
    switch (protection)
    {
    case SLOPKIT_AGENT_PAGE_NOACCESS:
    case SLOPKIT_AGENT_PAGE_READONLY:
    case SLOPKIT_AGENT_PAGE_READWRITE:
    case SLOPKIT_AGENT_PAGE_WRITECOPY:
    case SLOPKIT_AGENT_PAGE_EXECUTE:
    case SLOPKIT_AGENT_PAGE_EXECUTE_READ:
    case SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE:
    case SLOPKIT_AGENT_PAGE_EXECUTE_WRITECOPY:
        return 1;
    default:
        return 0;
    }
}

static void agent_handle_protect(const slopkit_agent_record* request, slopkit_agent_record* response)
{
    if (request->size == 0 || !agent_is_page_protection(request->protection))
    {
        response->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        return;
    }
    if (agent_find_containing(request->address, request->size) == NULL)
    {
        response->status      = SLOPKIT_AGENT_STATUS_NOT_OWNED;
        response->win32_error = (uint32_t)ERROR_INVALID_ADDRESS;
        return;
    }
    DWORD previous = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)request->address, (SIZE_T)request->size, request->protection, &previous))
    {
        agent_fill_error(response, GetLastError());
        return;
    }
    response->protection = (uint32_t)previous;
}

static void agent_handle_query(const slopkit_agent_record* request, slopkit_agent_record* response)
{
    if (request->address == 0)
    {
        response->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        return;
    }
    MEMORY_BASIC_INFORMATION region;
    if (VirtualQuery((LPCVOID)(uintptr_t)request->address, &region, sizeof(region)) != sizeof(region))
    {
        agent_fill_error(response, GetLastError());
        return;
    }
    response->region_base    = (uint64_t)(uintptr_t)region.BaseAddress;
    response->region_size    = (uint64_t)region.RegionSize;
    response->region_state   = (uint32_t)region.State;
    response->region_protect = (uint32_t)region.Protect;
}

/* Answers one slot's request into its response record and publishes the slot's
   response with a release store. Returns 0 when the request was SHUTDOWN, which
   the dispatcher handles itself: no worker may free allocations under another
   worker's feet. */
static int agent_dispatch(uint32_t index)
{
    slopkit_agent_slot*  slot = &g_region->slots[index];
    slopkit_agent_record request;
    slopkit_agent_record response;
    memcpy(&request, &slot->request, sizeof(request));
    memset(&response, 0, sizeof(response));
    response.op         = request.op;
    response.request_id = request.request_id;
    response.status     = SLOPKIT_AGENT_STATUS_OK;

    int keep_serving = 1;
    switch (request.op)
    {
    case SLOPKIT_AGENT_OP_HELLO:
        agent_handle_hello(&response);
        break;
    case SLOPKIT_AGENT_OP_ALLOC:
        agent_handle_alloc(&request, &response);
        break;
    case SLOPKIT_AGENT_OP_ALLOC_NEAR:
        agent_handle_alloc_near(&request, &response);
        break;
    case SLOPKIT_AGENT_OP_FREE:
        agent_handle_free(&request, &response);
        break;
    case SLOPKIT_AGENT_OP_PROTECT:
        agent_handle_protect(&request, &response);
        break;
    case SLOPKIT_AGENT_OP_QUERY:
        agent_handle_query(&request, &response);
        break;
    case SLOPKIT_AGENT_OP_ENUM:
        agent_handle_enum(&request, &response, slot->payload);
        break;
    case SLOPKIT_AGENT_OP_MODULES:
        agent_handle_modules(&request, &response, slot->payload);
        break;
    case SLOPKIT_AGENT_OP_SHUTDOWN:
        keep_serving = 0;
        break;
    default:
        response.status = SLOPKIT_AGENT_STATUS_UNKNOWN_OP;
        break;
    }

    slot->response = response;
    InterlockedIncrement((volatile LONG*)&g_region->header.requests);
    InterlockedExchange((volatile LONG*)&slot->state, (LONG)SLOPKIT_AGENT_SLOT_RESPONSE);
    return keep_serving;
}

static DWORD WINAPI agent_worker_thread(LPVOID parameter);

/* Serves a claimed slot and releases the claim, so the dispatcher sees it again
   once the host republishes a new request on the same slot. */
static void agent_serve_slot(uint32_t index)
{
    (void)agent_dispatch(index);
    EnterCriticalSection(&g_lock);
    g_claimed[index] = 0;
    LeaveCriticalSection(&g_lock);
}

/* The oldest queued slot index, or SLOPKIT_AGENT_SLOT_COUNT when the queue is
   empty. */
static uint32_t agent_take_queued(void)
{
    uint32_t index = SLOPKIT_AGENT_SLOT_COUNT;
    EnterCriticalSection(&g_lock);
    if (g_queue_count > 0)
    {
        index = g_queue[0];
        for (uint32_t i = 1; i < g_queue_count; ++i)
        {
            g_queue[i - 1] = g_queue[i];
        }
        --g_queue_count;
    }
    LeaveCriticalSection(&g_lock);
    return index;
}

/* Creates workers on demand: never more than one fewer than the worker cap (the
   dispatcher serves one request itself) and never more than there is work for. */
static void agent_ensure_workers(uint32_t pending)
{
    const uint32_t cap = SLOPKIT_AGENT_MAX_WORKERS - 1;
    for (;;)
    {
        EnterCriticalSection(&g_lock);
        const uint32_t started = g_workers_started;
        LeaveCriticalSection(&g_lock);
        if (started >= cap || pending <= started + 1)
        {
            return;
        }

        EnterCriticalSection(&g_lock);
        const uint32_t index = g_workers_started < cap ? g_workers_started : cap;
        if (index < cap)
        {
            g_workers_started = index + 1;
        }
        LeaveCriticalSection(&g_lock);
        if (index >= cap)
        {
            return;
        }

        HANDLE worker = CreateThread(NULL, 0, agent_worker_thread, NULL, 0, NULL);
        if (worker == NULL)
        {
            EnterCriticalSection(&g_lock);
            g_workers_started = index;
            LeaveCriticalSection(&g_lock);
            return;
        }
        g_workers[index] = worker;
    }
}

/* One queued request at a time until the stop event is set. Created by the
   dispatcher, never under Wine's loader lock. The queue is polled before every
   wait: an auto-reset event collapses several signals into one, so a worker that
   took one item must keep taking until the queue is empty. */
static DWORD WINAPI agent_worker_thread(LPVOID parameter)
{
    slopkit_agent_marker marker;
    (void)parameter;
    agent_name_thread();
    for (;;)
    {
        agent_mark(&marker);
        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0)
        {
            return 0;
        }
        const uint32_t index = agent_take_queued();
        if (index < SLOPKIT_AGENT_SLOT_COUNT)
        {
            agent_serve_slot(index);
            continue;
        }
        (void)WaitForSingleObject(g_work_event, 20);
    }
}

/* Waits until no worker holds a claimed slot, so the region is quiet before the
   dispatcher frees allocations and unmaps it. */
static void agent_drain_slots(void)
{
    for (;;)
    {
        int busy = 0;
        EnterCriticalSection(&g_lock);
        for (uint32_t index = 0; index < SLOPKIT_AGENT_SLOT_COUNT; ++index)
        {
            if (g_claimed[index] != 0)
            {
                busy = 1;
            }
        }
        LeaveCriticalSection(&g_lock);
        if (!busy)
        {
            return;
        }
        Sleep(SLOPKIT_AGENT_SLEEP_MIN_MS);
    }
}

static DWORD WINAPI agent_service_thread(LPVOID parameter)
{
    slopkit_agent_marker marker;
    g_instance = (HMODULE)parameter;
    if (!agent_open_region())
    {
        return 0;
    }
    if (g_region->header.magic != SLOPKIT_AGENT_MAGIC)
    {
        agent_close_region();
        return 0;
    }
    if (g_region->header.version != SLOPKIT_AGENT_PROTOCOL_VERSION
        || g_region->header.record_size != (uint32_t)sizeof(slopkit_agent_record))
    {
        /* The host compiled a different layout; tell it so instead of leaving
           it to time out on a reply that will never match. */
        InterlockedOr((volatile LONG*)&g_region->header.flags, (LONG)SLOPKIT_AGENT_FLAG_PROTOCOL_MISMATCH);
        agent_close_region();
        return 0;
    }

    /* The loader lock is already gone by the time this thread runs, so the
       dispatcher may create the lock, the events and every worker it needs. */
    InitializeCriticalSection(&g_lock);
    g_work_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    agent_name_thread();
    g_region->header.agent_pid    = (uint32_t)GetCurrentProcessId();
    g_region->header.slot_count   = SLOPKIT_AGENT_SLOT_COUNT;
    g_region->header.worker_count = SLOPKIT_AGENT_MAX_WORKERS;
    g_region->header.ready        = 1;

    int      stopping       = 0;
    uint32_t last_heartbeat = g_region->header.heartbeat;
    uint32_t spins          = 0;
    DWORD    sleep_ms       = SLOPKIT_AGENT_SLEEP_MIN_MS;
    while (!stopping)
    {
        agent_mark(&marker);

        for (uint32_t index = 0; index < SLOPKIT_AGENT_SLOT_COUNT; ++index)
        {
            if (agent_load_slot_state(index) != SLOPKIT_AGENT_SLOT_REQUEST)
            {
                continue;
            }

            int claimed = 0;
            EnterCriticalSection(&g_lock);
            if (g_claimed[index] == 0)
            {
                g_claimed[index] = 1;
                claimed          = 1;
            }
            LeaveCriticalSection(&g_lock);
            if (!claimed)
            {
                continue;
            }

            if (g_region->slots[index].request.op == SLOPKIT_AGENT_OP_SHUTDOWN)
            {
                /* Answer SHUTDOWN inline, then drain every other in-flight slot
                   before anything is freed or the region is unmapped. */
                (void)agent_dispatch(index);
                EnterCriticalSection(&g_lock);
                g_claimed[index] = 0;
                LeaveCriticalSection(&g_lock);
                stopping = 1;
                break;
            }

            EnterCriticalSection(&g_lock);
            g_queue[g_queue_count] = index;
            ++g_queue_count;
            LeaveCriticalSection(&g_lock);
            SetEvent(g_work_event);
        }

        if (stopping)
        {
            break;
        }

        /* Keep draining while anything is queued: the workers poll too, and the
           dispatcher serving one itself guarantees progress even if a signal was
           collapsed. */
        uint32_t queued = 0;
        EnterCriticalSection(&g_lock);
        queued = g_queue_count;
        LeaveCriticalSection(&g_lock);
        if (queued > 0)
        {
            spins    = 0;
            sleep_ms = SLOPKIT_AGENT_SLEEP_MIN_MS;
            agent_ensure_workers(queued);
            const uint32_t index = agent_take_queued();
            if (index < SLOPKIT_AGENT_SLOT_COUNT)
            {
                agent_serve_slot(index);
            }
            continue;
        }

        const uint32_t heartbeat = g_region->header.heartbeat;
        if (heartbeat != last_heartbeat)
        {
            last_heartbeat = heartbeat;
            spins          = 0;
            sleep_ms       = SLOPKIT_AGENT_SLEEP_MIN_MS;
            continue;
        }
        if (spins < SLOPKIT_AGENT_SPIN_LIMIT)
        {
            ++spins;
            agent_pause();
        }
        else
        {
            Sleep(sleep_ms);
            if (sleep_ms < SLOPKIT_AGENT_SLEEP_MAX_MS)
            {
                sleep_ms *= 2;
                if (sleep_ms > SLOPKIT_AGENT_SLEEP_MAX_MS)
                {
                    sleep_ms = SLOPKIT_AGENT_SLEEP_MAX_MS;
                }
            }
        }
    }

    /* Stop every worker and wait for each to finish the slot it holds, so no
       request is served out of a region that is about to disappear. */
    SetEvent(g_stop_event);
    agent_drain_slots();
    for (uint32_t index = 0; index < g_workers_started; ++index)
    {
        if (g_workers[index] != NULL)
        {
            WaitForSingleObject(g_workers[index], INFINITE);
            CloseHandle(g_workers[index]);
            g_workers[index] = NULL;
        }
    }

    agent_free_all();
    InterlockedOr((volatile LONG*)&g_region->header.flags, (LONG)SLOPKIT_AGENT_FLAG_STOPPED);
    agent_close_region();
    FreeLibraryAndExitThread(g_instance, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        /* The only work the loader lock sees: hand the module handle to the
           service thread and let it do everything else. */
        CloseHandle(CreateThread(NULL, 0, agent_service_thread, (LPVOID)instance, 0, NULL));
    }
    else if (reason == DLL_PROCESS_DETACH && g_stop_event != NULL)
    {
        /* A process that exits without SHUTDOWN: end the loops best-effort so
           no worker keeps touching a region on the way down. */
        SetEvent(g_stop_event);
    }
    return TRUE;
}

/* -nostdlib leaves the PE entry point to us; the loader calls it with the
   same (instance, reason, reserved) signature as DllMain, so it stays WINAPI.
   The i686 stdcall decoration would rename the symbol to _name@12, which the
   linker's --entry cannot spell portably; an asm label pins the plain symbol
   instead. */
#if defined(__i386__) && defined(__GNUC__)
int WINAPI slopkit_agent_entry(HINSTANCE instance, DWORD reason, LPVOID reserved) __asm__("_slopkit_agent_entry");
#endif

int WINAPI slopkit_agent_entry(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    return DllMain(instance, reason, reserved) ? 1 : 0;
}
