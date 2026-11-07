#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "platform/linux/memory.hpp"
#include "plugin/plugin_api.h"

namespace slopkit::plugins::support
{
    struct AccessFault
    {
        std::int32_t code {0};
        std::string  message;
    };

    struct MemoryResult
    {
        std::size_t   bytes {0};
        std::uint32_t method {0};
    };

    inline std::int32_t status_for(platform::MemoryError error)
    {
        switch (error)
        {
        case platform::MemoryError::process_not_found:
            return SLOPKIT_ERR_NOT_FOUND;
        case platform::MemoryError::permission_denied:
            return SLOPKIT_ERR_PERMISSION_DENIED;
        case platform::MemoryError::invalid_argument:
            return SLOPKIT_ERR_INVALID_ARGUMENT;
        case platform::MemoryError::unmapped:
            return SLOPKIT_ERR_NOT_FOUND;
        case platform::MemoryError::io_error:
            return SLOPKIT_ERR_IO;
        }
        return SLOPKIT_ERR_INTERNAL;
    }

    inline const char* message_for(platform::MemoryError error)
    {
        switch (error)
        {
        case platform::MemoryError::process_not_found:
            return "no such process";
        case platform::MemoryError::permission_denied:
            return "permission denied";
        case platform::MemoryError::invalid_argument:
            return "invalid argument";
        case platform::MemoryError::unmapped:
            return "address range is not mapped";
        case platform::MemoryError::io_error:
            return "memory I/O error";
        }
        return "unknown memory error";
    }

    inline std::uint32_t method_flag(platform::MemoryPrimitive primitive)
    {
        switch (primitive)
        {
        case platform::MemoryPrimitive::process_vm:
            return SLOPKIT_ACCESS_PROCESS_VM;
        case platform::MemoryPrimitive::procfs_mem:
            return SLOPKIT_ACCESS_PROCFS_MEM;
        case platform::MemoryPrimitive::none:
            return 0;
        }
        return 0;
    }

}
