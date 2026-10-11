/*
 * The resident agent DLL the wine-proton plugin injects into a Wine/Proton
 * target. Its only job is to call Wine's own VirtualAlloc/VirtualFree/
 * VirtualProtect/VirtualQuery from inside the target process, so the mappings
 * it creates are ones Wine's virtual-memory manager knows about (a raw mmap
 * planted by ptrace is invisible to it).
 *
 * The DLL carries no CRTF/C++ runtime: it is C, built with -nostdlib, and
 * talks to kernel32 only. DllMain does one thing — start the service thread —
 * so nothing heavy runs under the loader lock. The service thread maps the
 * shared region sitting next to the DLL, serves requests until SHUTDOWN and
 * then unloads itself.
 */

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
   SHUTDOWN can release everything. Only the service thread touches the list. */
typedef struct slopkit_agent_allocation
{
    uint64_t                         base;
    uint64_t                         size;
    struct slopkit_agent_allocation* next;
} slopkit_agent_allocation;

static HMODULE                   g_instance;
static HANDLE                    g_mapping;
static slopkit_agent_region*     g_region;
static slopkit_agent_allocation* g_allocations;

static slopkit_agent_allocation* agent_find_exact(uint64_t base)
{
    for (slopkit_agent_allocation* node = g_allocations; node != NULL; node = node->next)
    {
        if (node->base == base)
        {
            return node;
        }
    }
    return NULL;
}

/* The allocation that wholly contains [address, address + size), or NULL. */
static slopkit_agent_allocation* agent_find_containing(uint64_t address, uint64_t size)
{
    for (slopkit_agent_allocation* node = g_allocations; node != NULL; node = node->next)
    {
        if (address >= node->base && size <= node->size && address - node->base <= node->size - size)
        {
            return node;
        }
    }
    return NULL;
}

static int agent_track(uint64_t base, uint64_t size)
{
    slopkit_agent_allocation* node =
        (slopkit_agent_allocation*)HeapAlloc(GetProcessHeap(), 0, sizeof(slopkit_agent_allocation));
    if (node == NULL)
    {
        return 0;
    }
    node->base    = base;
    node->size    = size;
    node->next    = g_allocations;
    g_allocations = node;
    return 1;
}

static void agent_untrack(slopkit_agent_allocation* node)
{
    slopkit_agent_allocation** link = &g_allocations;
    while (*link != NULL)
    {
        if (*link == node)
        {
            *link = node->next;
            HeapFree(GetProcessHeap(), 0, node);
            return;
        }
        link = &(*link)->next;
    }
}

static void agent_free_all(void)
{
    while (g_allocations != NULL)
    {
        slopkit_agent_allocation* node = g_allocations;
        g_allocations                  = node->next;
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
static uint32_t agent_load_state(void)
{
    return (uint32_t)InterlockedCompareExchange(
        (volatile LONG*)&g_region->header.state, (LONG)SLOPKIT_AGENT_STATE_IDLE, (LONG)SLOPKIT_AGENT_STATE_IDLE);
}

static void agent_publish_response(const slopkit_agent_record* response)
{
    g_region->response = *response;
    InterlockedIncrement((volatile LONG*)&g_region->header.requests);
    InterlockedExchange((volatile LONG*)&g_region->header.state, (LONG)SLOPKIT_AGENT_STATE_RESPONSE);
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
    /* The host refuses a different payload capacity before trusting the area. */
    response->reserved0        = SLOPKIT_AGENT_PAYLOAD_SIZE;
}

/* Whether a protection permits a plain in-target read. PAGE_GUARD is a
   one-shot trap, not readable memory, so it is excluded; the low byte is the
   base protection and the PAGE_NOCACHE/PAGE_WRITECOMBINE modifiers above it do
   not change readability. */
static int agent_protection_readable(uint32_t protection)
{
    if ((protection & 0x100u) != 0)
    {
        return 0;
    }
    switch (protection & 0xFFu)
    {
    case SLOPKIT_AGENT_PAGE_READONLY:
    case SLOPKIT_AGENT_PAGE_READWRITE:
    case SLOPKIT_AGENT_PAGE_WRITECOPY:
    case SLOPKIT_AGENT_PAGE_EXECUTE_READ:
    case SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE:
    case SLOPKIT_AGENT_PAGE_EXECUTE_WRITECOPY:
        return 1;
    default:
        return 0;
    }
}

/* Whether a protection permits a plain in-target write; a read-only or
   execute-only page would fault the target, not merely fail the request. */
static int agent_protection_writable(uint32_t protection)
{
    if ((protection & 0x100u) != 0)
    {
        return 0;
    }
    switch (protection & 0xFFu)
    {
    case SLOPKIT_AGENT_PAGE_READWRITE:
    case SLOPKIT_AGENT_PAGE_WRITECOPY:
    case SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE:
    case SLOPKIT_AGENT_PAGE_EXECUTE_WRITECOPY:
        return 1;
    default:
        return 0;
    }
}

/* ENUM: walk the target's own VirtualQuery chain from the request cursor,
   writing up to `size` (0 = the whole payload) region records into payload[].
   Progress stops at the capacity, at the end of the user address space, or on
   a failed VirtualQuery; the next cursor is reported in address, 0 when the
   walk reached the end. */
static void agent_handle_enum(const slopkit_agent_record* request, slopkit_agent_record* response)
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

    slopkit_agent_region_entry* entries = (slopkit_agent_region_entry*)g_region->payload;
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

/* The bytes one READ/WRITE may move: no more than the payload, and never past
   the end of the committed region containing `address`. Returns 0 when nothing
   is transferable, which the callers report as an error. */
static uint64_t agent_transfer_span(uint64_t address, uint64_t size, int for_write)
{
    if (size > SLOPKIT_AGENT_PAYLOAD_SIZE)
    {
        size = SLOPKIT_AGENT_PAYLOAD_SIZE;
    }
    MEMORY_BASIC_INFORMATION region;
    if (VirtualQuery((LPCVOID)(uintptr_t)address, &region, sizeof(region)) != sizeof(region))
    {
        return 0;
    }
    if (region.State != MEM_COMMIT)
    {
        return 0;
    }
    const uint32_t protection = (uint32_t)region.Protect;
    if ((for_write && !agent_protection_writable(protection)) || (!for_write && !agent_protection_readable(protection)))
    {
        return 0;
    }
    const uint64_t base = (uint64_t)(uintptr_t)region.BaseAddress;
    const uint64_t end  = base + (uint64_t)region.RegionSize;
    if (end <= address)
    {
        return 0;
    }
    return size < end - address ? size : end - address;
}

/* READ: copy the target's bytes into payload[], at most min(size, payload)
   bytes and never across the containing region's end. A short copy is a
   success; a range with nothing readable is an error. */
static void agent_handle_read(const slopkit_agent_record* request, slopkit_agent_record* response)
{
    if (request->size == 0 || request->size > SLOPKIT_AGENT_PAYLOAD_SIZE)
    {
        response->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        return;
    }
    const uint64_t span = agent_transfer_span(request->address, request->size, 0);
    if (span == 0)
    {
        agent_fill_error(response, ERROR_INVALID_ADDRESS);
        return;
    }
    memcpy(g_region->payload, (const void*)(uintptr_t)request->address, (size_t)span);
    response->size = span;
}

/* WRITE: copy payload[] into the target, bounded the same way as READ. */
static void agent_handle_write(const slopkit_agent_record* request, slopkit_agent_record* response)
{
    if (request->size == 0 || request->size > SLOPKIT_AGENT_PAYLOAD_SIZE)
    {
        response->status = SLOPKIT_AGENT_STATUS_BAD_REQUEST;
        return;
    }
    const uint64_t span = agent_transfer_span(request->address, request->size, 1);
    if (span == 0)
    {
        agent_fill_error(response, ERROR_INVALID_ADDRESS);
        return;
    }
    memcpy((void*)(uintptr_t)request->address, g_region->payload, (size_t)span);
    response->size = span;
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

/* Returns 0 when the service thread must stop. */
static int agent_handle_request(void)
{
    slopkit_agent_record request;
    slopkit_agent_record response;
    memcpy(&request, &g_region->request, sizeof(request));
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
        agent_handle_enum(&request, &response);
        break;
    case SLOPKIT_AGENT_OP_READ:
        agent_handle_read(&request, &response);
        break;
    case SLOPKIT_AGENT_OP_WRITE:
        agent_handle_write(&request, &response);
        break;
    case SLOPKIT_AGENT_OP_SHUTDOWN:
        agent_free_all();
        keep_serving = 0;
        break;
    default:
        response.status = SLOPKIT_AGENT_STATUS_UNKNOWN_OP;
        break;
    }

    agent_publish_response(&response);
    if (!keep_serving)
    {
        InterlockedOr((volatile LONG*)&g_region->header.flags, (LONG)SLOPKIT_AGENT_FLAG_STOPPED);
    }
    return keep_serving;
}

static DWORD WINAPI agent_service_thread(LPVOID parameter)
{
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
    g_region->header.agent_pid = (uint32_t)GetCurrentProcessId();

    uint32_t last_heartbeat = g_region->header.heartbeat;
    uint32_t spins          = 0;
    DWORD    sleep_ms       = SLOPKIT_AGENT_SLEEP_MIN_MS;
    for (;;)
    {
        if (agent_load_state() == SLOPKIT_AGENT_STATE_REQUEST)
        {
            spins    = 0;
            sleep_ms = SLOPKIT_AGENT_SLEEP_MIN_MS;
            if (!agent_handle_request())
            {
                break;
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

    agent_free_all();
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
