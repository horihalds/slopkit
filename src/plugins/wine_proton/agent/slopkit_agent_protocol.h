#pragma once

#include <stddef.h>
#include <stdint.h>

/* The four bytes "A" "K" "L" "S" read as a little-endian 32-bit word; a stale
   mapping from an unrelated file is rejected by the magic before anything else
   is trusted. */
#define SLOPKIT_AGENT_MAGIC                  0x534C4B41u

/* Bumped whenever the layout below changes. A mismatch is refused as a
   protocol error, never waited on until a timeout. */
#define SLOPKIT_AGENT_PROTOCOL_VERSION       4u

/* The ring of independent request slots one region carries: every slot has its
   own state word, request/response record and payload, so this many requests
   can be in flight at once. */
#define SLOPKIT_AGENT_SLOT_COUNT             4u

/* The cap on the agent's concurrent service threads. The dispatcher is one of
   them, so the agent runs at most 1 + (SLOPKIT_AGENT_MAX_WORKERS - 1) = 4
   threads and never more workers than there are slots. */
#define SLOPKIT_AGENT_MAX_WORKERS            4u

/* A live marker every agent thread keeps on its own stack while it serves, so
   the host can tell the agent's threads from the target's own without ptrace
   (see SLOPKIT_AGENT_STACK_SCAN_BYTES). */
#define SLOPKIT_AGENT_THREAD_MAGIC           0x534C4F504B495421ull
/* How far below a thread's stack top the host scans for the marker. */
#define SLOPKIT_AGENT_STACK_SCAN_BYTES       (256u * 1024u)

/* The thread description the agent puts on every one of its threads, so the
   host's /proc/<tid>/comm fast path can recognise them by name. */
#define SLOPKIT_AGENT_THREAD_NAME            "slopkit-agent"

/* The file names shared by both sides. The DLL is loaded from its own
   per-session directory and finds the shared region as the sibling file, so
   the path never has to be passed into the target. */
#define SLOPKIT_AGENT_SHM_FILE               "slopkit-agent.bin"
#define SLOPKIT_AGENT_SHM_FILE_W             L"slopkit-agent.bin"
#define SLOPKIT_AGENT_DLL_FILE               "slopkit-agent.dll"
#define SLOPKIT_AGENT_INJECTOR_FILE          "slopkit-injector.exe"

/* Request opcodes. */
#define SLOPKIT_AGENT_OP_HELLO               1u
#define SLOPKIT_AGENT_OP_ALLOC               2u
#define SLOPKIT_AGENT_OP_ALLOC_NEAR          3u
#define SLOPKIT_AGENT_OP_FREE                4u
#define SLOPKIT_AGENT_OP_PROTECT             5u
#define SLOPKIT_AGENT_OP_QUERY               6u
#define SLOPKIT_AGENT_OP_SHUTDOWN            7u
/* Version 2. */
#define SLOPKIT_AGENT_OP_ENUM                8u
#define SLOPKIT_AGENT_OP_READ                9u
#define SLOPKIT_AGENT_OP_WRITE               10u
/* Version 3. */
#define SLOPKIT_AGENT_OP_MODULES             11u

/* Module flags carried in slopkit_agent_module_entry.flags. */
#define SLOPKIT_AGENT_MODULE_FLAG_MAIN       0x1u

/* The payload area trailing the request and response records. It holds
   `SLOPKIT_AGENT_PAYLOAD_SIZE / sizeof(slopkit_agent_region_entry)` region
   records for ENUM, the bytes READ returns or the bytes WRITE sends. 64 KiB
   matches the chunk the host's read probe already uses. */
#define SLOPKIT_AGENT_PAYLOAD_SIZE           (64u * 1024u)

/* Response statuses. SLOPKIT_AGENT_STATUS_ERROR always carries a Win32 error
   code in win32_error; the others describe a request the agent refused before
   it reached a Win32 call. */
#define SLOPKIT_AGENT_STATUS_OK              0u
#define SLOPKIT_AGENT_STATUS_ERROR           1u
#define SLOPKIT_AGENT_STATUS_BAD_REQUEST     2u
#define SLOPKIT_AGENT_STATUS_UNKNOWN_OP      3u
#define SLOPKIT_AGENT_STATUS_NOT_OWNED       4u
#define SLOPKIT_AGENT_STATUS_UNSUPPORTED     5u

/* Header flags. */
#define SLOPKIT_AGENT_FLAG_NONE              0u
/* The agent's compiled header layout does not match the one the host wrote;
   it cannot serve, so the host must fall back instead of waiting for a reply
   that can never come. */
#define SLOPKIT_AGENT_FLAG_PROTOCOL_MISMATCH 1u
/* The agent has served SHUTDOWN and its service thread has stopped. */
#define SLOPKIT_AGENT_FLAG_STOPPED           2u

/* One slot's state word. 0 -> idle, 1 -> a request is published, 2 -> the
   response is published. Written with acquire/release ordering on both sides.
   RETIRED only exists on the host: a timed-out exchange marks its slot so a
   late answer can never be mistaken for the next request's answer. */
#define SLOPKIT_AGENT_SLOT_IDLE              0u
#define SLOPKIT_AGENT_SLOT_REQUEST           1u
#define SLOPKIT_AGENT_SLOT_RESPONSE          2u
#define SLOPKIT_AGENT_SLOT_RETIRED           3u

/* Page protections, mirroring the Win32 PAGE_* values the agent passes to
   VirtualAlloc/VirtualProtect. */
#define SLOPKIT_AGENT_PAGE_NOACCESS          0x01u
#define SLOPKIT_AGENT_PAGE_READONLY          0x02u
#define SLOPKIT_AGENT_PAGE_READWRITE         0x04u
#define SLOPKIT_AGENT_PAGE_WRITECOPY         0x08u
#define SLOPKIT_AGENT_PAGE_EXECUTE           0x10u
#define SLOPKIT_AGENT_PAGE_EXECUTE_READ      0x20u
#define SLOPKIT_AGENT_PAGE_EXECUTE_READWRITE 0x40u
#define SLOPKIT_AGENT_PAGE_EXECUTE_WRITECOPY 0x80u

/* Allocation types, mirroring the Win32 MEM_* values. */
#define SLOPKIT_AGENT_MEM_COMMIT             0x1000u
#define SLOPKIT_AGENT_MEM_RESERVE            0x2000u
#define SLOPKIT_AGENT_MEM_FREE               0x10000u

/* Mapping types, mirroring the Win32 MEM_* values; a range that is not private
   is shared with another view. */
#define SLOPKIT_AGENT_MEM_PRIVATE            0x20000u
#define SLOPKIT_AGENT_MEM_MAPPED             0x40000u
#define SLOPKIT_AGENT_MEM_IMAGE              0x1000000u

/* The request a host writes and the response the agent writes back, in one
   layout: a request leaves the response-only fields zero and a response leaves
   the request-only fields zero. All fields are little-endian.

   The structs are *not* packed: every member is naturally aligned on both
   x86 and x86_64, so the two compilers produce the same offsets while the
   fields stay addressable (a packed struct would trip
   -Waddress-of-packed-member when the host takes the state word's address).
   The static asserts below are the contract that keeps that true. */
typedef struct slopkit_agent_record
{
    /* Both directions. */
    uint32_t op;
    uint32_t status;
    uint32_t win32_error;
    uint32_t request_id;

    /* Request: the address a FREE/PROTECT/QUERY names. Response: the
       allocated base of ALLOC/ALLOC_NEAR. */
    uint64_t address;
    /* Request: the requested byte count. Response: the committed size. */
    uint64_t size;
    /* Request: the ALLOC hint. Unused otherwise. */
    uint64_t hint;
    /* Request: the PAGE_* protection to apply. Response: the previous one. */
    uint32_t protection;

    /* HELLO response. */
    uint32_t protocol_version;
    uint32_t record_size;
    uint32_t pointer_size;
    uint32_t windows_pid;
    uint32_t granularity;
    uint64_t agent_base;

    /* QUERY response, straight from MEMORY_BASIC_INFORMATION. */
    uint64_t region_base;
    uint64_t region_size;
    uint32_t region_state;
    uint32_t region_protect;

    /* HELLO response: the capacity the host sizes its slot pool from, 0 when
       the agent predates the slot ring and the host must degrade to one. */
    uint32_t slot_count;
    uint32_t worker_count;

    uint64_t reserved0;
} slopkit_agent_record;

/* The fixed header every mapping starts with. */
typedef struct slopkit_agent_header
{
    uint32_t magic;
    uint32_t version;
    uint32_t record_size;
    uint32_t flags;
    /* The slot ring's capacity, published once the agent has mapped; the host
       compares it before it trusts the slot array. */
    uint32_t slot_count;
    /* How many worker threads the agent may run; informational. */
    uint32_t worker_count;
    /* Bumped by the host on every request so an idle agent can back off
       without losing the ability to notice the host is still alive. */
    uint32_t heartbeat;
    /* The agent's own Windows process id, published once it has mapped. */
    uint32_t agent_pid;
    /* How many requests the agent has served; informational. */
    uint32_t requests;
    /* Set once the agent's dispatcher is ready to serve slots. */
    uint32_t ready;

    uint64_t reserved0;
} slopkit_agent_header;

/* One region record written into the payload by ENUM: the same fields
   MEMORY_BASIC_INFORMATION reports, widened to fixed-width integers. */
typedef struct slopkit_agent_region_entry
{
    uint64_t base;
    uint64_t size;
    uint32_t state;
    uint32_t protection;
    uint32_t type;
    uint32_t reserved;
} slopkit_agent_region_entry;

/* Version 3. One loaded image from the target's own loader list, written into
   the payload by MODULES: the fixed-width fields of one loader entry plus a
   reference to its name. Records sit at the start of payload[] in loader
   order; each name's UTF-16 bytes are appended from the end of the payload
   downwards and referenced by name_offset/name_length. */
typedef struct slopkit_agent_module_entry
{
    uint64_t base;        /* DllBase */
    uint64_t size;        /* SizeOfImage */
    uint64_t entry;       /* DllBase + AddressOfEntryPoint, 0 when none */
    uint32_t name_offset; /* byte offset of the name in payload[] */
    uint32_t name_length; /* bytes, no terminator */
    uint32_t flags;       /* SLOPKIT_AGENT_MODULE_FLAG_MAIN */
    uint32_t reserved;
} slopkit_agent_module_entry;

/* One request slot: its own state word, request/response records and payload,
   so one it never overlaps another slot's exchange. */
typedef struct slopkit_agent_slot
{
    /* The atomic state word (SLOPKIT_AGENT_SLOT_*). */
    uint32_t                    state;
    uint32_t                    reserved;
    struct slopkit_agent_record request;
    struct slopkit_agent_record response;
    uint8_t                     payload[SLOPKIT_AGENT_PAYLOAD_SIZE];
} slopkit_agent_slot;

/* One mapping, mapped by the host and the agent alike.

   Field reuse across the payload-carrying ops:
     - ENUM request:  address = the address-space cursor to resume from,
                      size    = the record capacity (0 = the whole payload).
       ENUM response: size    = records written, address = the next cursor,
                      or 0 when the walk reached the end of the user address
                      space. The records land in payload[].
     - READ/WRITE request:  address = the target address, size = the byte
                      count. READ fills payload[]; WRITE sends it.
       READ/WRITE response: size = the bytes actually moved (a short transfer
                      is a success).
     - MODULES request:  address = the starting loader-index (0 = the first
                      loader entry), size = the record capacity (0 = the whole
                      payload).
       MODULES response: size = records written, address = the index to resume
                      from, or 0 when the loader list ended. The records land
                      in payload[], followed by the names growing down from the
                      end of the payload.
     - HELLO response: reserved0 = SLOPKIT_AGENT_PAYLOAD_SIZE, which the host
                      compares before trusting a payload. */
typedef struct slopkit_agent_region
{
    slopkit_agent_header header;
    slopkit_agent_slot   slots[SLOPKIT_AGENT_SLOT_COUNT];
} slopkit_agent_region;

#if defined(__cplusplus)
#define SLOPKIT_AGENT_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define SLOPKIT_AGENT_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

SLOPKIT_AGENT_STATIC_ASSERT(sizeof(slopkit_agent_record) == 112, "slopkit_agent_record layout changed");
SLOPKIT_AGENT_STATIC_ASSERT(sizeof(slopkit_agent_header) == 48, "slopkit_agent_header layout changed");
SLOPKIT_AGENT_STATIC_ASSERT(sizeof(slopkit_agent_slot) == 232 + SLOPKIT_AGENT_PAYLOAD_SIZE,
                            "slopkit_agent_slot layout changed");
SLOPKIT_AGENT_STATIC_ASSERT(sizeof(slopkit_agent_region_entry) == 32, "slopkit_agent_region_entry layout changed");
SLOPKIT_AGENT_STATIC_ASSERT(sizeof(slopkit_agent_module_entry) == 40, "slopkit_agent_module_entry layout changed");
SLOPKIT_AGENT_STATIC_ASSERT(sizeof(slopkit_agent_region)
                                == 48 + SLOPKIT_AGENT_SLOT_COUNT * (232 + SLOPKIT_AGENT_PAYLOAD_SIZE),
                            "slopkit_agent_region layout changed");

SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, op) == 0, "record.op moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, status) == 4, "record.status moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, win32_error) == 8, "record.win32_error moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, request_id) == 12, "record.request_id moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, address) == 16, "record.address moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, size) == 24, "record.size moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, hint) == 32, "record.hint moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, protection) == 40, "record.protection moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, protocol_version) == 44, "record.protocol_version moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, record_size) == 48, "record.record_size moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, pointer_size) == 52, "record.pointer_size moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, windows_pid) == 56, "record.windows_pid moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, granularity) == 60, "record.granularity moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, agent_base) == 64, "record.agent_base moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, region_base) == 72, "record.region_base moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, region_size) == 80, "record.region_size moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, region_state) == 88, "record.region_state moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, region_protect) == 92, "record.region_protect moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, slot_count) == 96, "record.slot_count moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, worker_count) == 100, "record.worker_count moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_record, reserved0) == 104, "record.reserved0 moved");

SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, magic) == 0, "header.magic moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, version) == 4, "header.version moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, record_size) == 8, "header.record_size moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, flags) == 12, "header.flags moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, slot_count) == 16, "header.slot_count moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, worker_count) == 20, "header.worker_count moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, heartbeat) == 24, "header.heartbeat moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, agent_pid) == 28, "header.agent_pid moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, requests) == 32, "header.requests moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_header, ready) == 36, "header.ready moved");

SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_region_entry, base) == 0, "entry.base moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_region_entry, size) == 8, "entry.size moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_region_entry, state) == 16, "entry.state moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_region_entry, protection) == 20, "entry.protection moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_region_entry, type) == 24, "entry.type moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_region_entry, reserved) == 28, "entry.reserved moved");

SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_module_entry, base) == 0, "module.base moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_module_entry, size) == 8, "module.size moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_module_entry, entry) == 16, "module.entry moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_module_entry, name_offset) == 24, "module.name_offset moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_module_entry, name_length) == 28, "module.name_length moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_module_entry, flags) == 32, "module.flags moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_module_entry, reserved) == 36, "module.reserved moved");

SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_slot, state) == 0, "slot.state moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_slot, request) == 8, "slot.request moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_slot, response) == 120, "slot.response moved");
SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_slot, payload) == 232, "slot.payload moved");

SLOPKIT_AGENT_STATIC_ASSERT(offsetof(slopkit_agent_region, slots) == 48, "region.slots moved");

#undef SLOPKIT_AGENT_STATIC_ASSERT
