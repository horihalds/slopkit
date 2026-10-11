#include <catch2/catch.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>

#include "plugins/wine_proton/agent/slopkit_agent_protocol.h"

// The resident agent DLL and the Linux plugin build from this one header with
// different compilers, so the layout is the contract. The static asserts in the
// header already fail the build on a mismatch; these checks also pin the magic,
// the version and the opcodes, so an accidental change is caught here instead of
// as a timeout against a live target.
TEST_CASE("the resident agent protocol layout is fixed", "[wine-proton][agent]")
{
    CHECK(sizeof(slopkit_agent_record) == 112);
    CHECK(sizeof(slopkit_agent_header) == 48);
    CHECK(sizeof(slopkit_agent_region_entry) == 32);
    CHECK(sizeof(slopkit_agent_module_entry) == 40);
    CHECK(sizeof(slopkit_agent_slot) == 232 + SLOPKIT_AGENT_PAYLOAD_SIZE);
    CHECK(sizeof(slopkit_agent_region) == 48 + SLOPKIT_AGENT_SLOT_COUNT * (232 + SLOPKIT_AGENT_PAYLOAD_SIZE));

    CHECK(offsetof(slopkit_agent_region, slots) == 48);
    CHECK(offsetof(slopkit_agent_slot, state) == 0);
    CHECK(offsetof(slopkit_agent_slot, request) == 8);
    CHECK(offsetof(slopkit_agent_slot, response) == 120);
    CHECK(offsetof(slopkit_agent_slot, payload) == 232);

    CHECK(offsetof(slopkit_agent_header, magic) == 0);
    CHECK(offsetof(slopkit_agent_header, version) == 4);
    CHECK(offsetof(slopkit_agent_header, record_size) == 8);
    CHECK(offsetof(slopkit_agent_header, flags) == 12);
    CHECK(offsetof(slopkit_agent_header, slot_count) == 16);
    CHECK(offsetof(slopkit_agent_header, worker_count) == 20);
    CHECK(offsetof(slopkit_agent_header, heartbeat) == 24);
    CHECK(offsetof(slopkit_agent_header, agent_pid) == 28);
    CHECK(offsetof(slopkit_agent_header, requests) == 32);
    CHECK(offsetof(slopkit_agent_header, ready) == 36);

    CHECK(offsetof(slopkit_agent_record, op) == 0);
    CHECK(offsetof(slopkit_agent_record, status) == 4);
    CHECK(offsetof(slopkit_agent_record, win32_error) == 8);
    CHECK(offsetof(slopkit_agent_record, request_id) == 12);
    CHECK(offsetof(slopkit_agent_record, address) == 16);
    CHECK(offsetof(slopkit_agent_record, size) == 24);
    CHECK(offsetof(slopkit_agent_record, hint) == 32);
    CHECK(offsetof(slopkit_agent_record, protection) == 40);
    CHECK(offsetof(slopkit_agent_record, protocol_version) == 44);
    CHECK(offsetof(slopkit_agent_record, record_size) == 48);
    CHECK(offsetof(slopkit_agent_record, pointer_size) == 52);
    CHECK(offsetof(slopkit_agent_record, windows_pid) == 56);
    CHECK(offsetof(slopkit_agent_record, granularity) == 60);
    CHECK(offsetof(slopkit_agent_record, agent_base) == 64);
    CHECK(offsetof(slopkit_agent_record, region_base) == 72);
    CHECK(offsetof(slopkit_agent_record, region_protect) == 92);
    CHECK(offsetof(slopkit_agent_record, slot_count) == 96);
    CHECK(offsetof(slopkit_agent_record, worker_count) == 100);
    CHECK(offsetof(slopkit_agent_record, reserved0) == 104);

    CHECK(offsetof(slopkit_agent_region_entry, base) == 0);
    CHECK(offsetof(slopkit_agent_region_entry, size) == 8);
    CHECK(offsetof(slopkit_agent_region_entry, state) == 16);
    CHECK(offsetof(slopkit_agent_region_entry, protection) == 20);
    CHECK(offsetof(slopkit_agent_region_entry, type) == 24);
    CHECK(offsetof(slopkit_agent_region_entry, reserved) == 28);

    CHECK(offsetof(slopkit_agent_module_entry, base) == 0);
    CHECK(offsetof(slopkit_agent_module_entry, size) == 8);
    CHECK(offsetof(slopkit_agent_module_entry, entry) == 16);
    CHECK(offsetof(slopkit_agent_module_entry, name_offset) == 24);
    CHECK(offsetof(slopkit_agent_module_entry, name_length) == 28);
    CHECK(offsetof(slopkit_agent_module_entry, flags) == 32);
    CHECK(offsetof(slopkit_agent_module_entry, reserved) == 36);

    CHECK(SLOPKIT_AGENT_MAGIC == 0x534C4B41u);
    CHECK(SLOPKIT_AGENT_PROTOCOL_VERSION == 5u);
    CHECK(SLOPKIT_AGENT_SLOT_COUNT == 4u);
    CHECK(SLOPKIT_AGENT_MAX_WORKERS == 4u);
    CHECK(SLOPKIT_AGENT_THREAD_MAGIC == 0x534C4F504B495421ull);
    CHECK(SLOPKIT_AGENT_PAYLOAD_SIZE == 64u * 1024u);
    CHECK(SLOPKIT_AGENT_OP_HELLO == 1u);
    CHECK(SLOPKIT_AGENT_OP_ALLOC == 2u);
    CHECK(SLOPKIT_AGENT_OP_ALLOC_NEAR == 3u);
    CHECK(SLOPKIT_AGENT_OP_FREE == 4u);
    CHECK(SLOPKIT_AGENT_OP_PROTECT == 5u);
    CHECK(SLOPKIT_AGENT_OP_QUERY == 6u);
    CHECK(SLOPKIT_AGENT_OP_SHUTDOWN == 7u);
    CHECK(SLOPKIT_AGENT_OP_ENUM == 8u);
    CHECK(SLOPKIT_AGENT_OP_MODULES == 11u);
    CHECK(SLOPKIT_AGENT_MODULE_FLAG_MAIN == 0x1u);
    CHECK(SLOPKIT_AGENT_SLOT_IDLE == 0u);
    CHECK(SLOPKIT_AGENT_SLOT_REQUEST == 1u);
    CHECK(SLOPKIT_AGENT_SLOT_RESPONSE == 2u);
    CHECK(SLOPKIT_AGENT_SLOT_RETIRED == 3u);
    CHECK(SLOPKIT_AGENT_FLAG_PROTOCOL_MISMATCH == 1u);
    CHECK(SLOPKIT_AGENT_FLAG_STOPPED == 2u);
}

TEST_CASE("the resident agent protocol encodes fixed-width little-endian fields", "[wine-proton][agent]")
{
    if (std::endian::native != std::endian::little)
    {
        WARN("the protocol is little-endian only; this host is not, so the byte check is skipped");
        return;
    }

    slopkit_agent_record record {};
    record.op         = 0x04030201u;
    record.size       = 0x1122334455667788ull;
    record.protection = 0x0A0B0C0Du;

    const auto* bytes = reinterpret_cast<const unsigned char*>(&record);
    CHECK(bytes[0] == 0x01);
    CHECK(bytes[1] == 0x02);
    CHECK(bytes[2] == 0x03);
    CHECK(bytes[3] == 0x04);

    CHECK(bytes[24] == 0x88);
    CHECK(bytes[31] == 0x11);
    CHECK(bytes[40] == 0x0D);
    CHECK(bytes[43] == 0x0A);
}
