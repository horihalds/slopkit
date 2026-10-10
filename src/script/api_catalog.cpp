#include "script/api_catalog.hpp"

#include <algorithm>
#include <cctype>

namespace slopkit::script
{
    namespace
    {
        // The engine's own vocabulary, in bind order: `engine.cpp`'s install_*
        // functions bind exactly these names. The guard test keeps the two in
        // step, so a new global has to be added here too.
        constexpr ApiEntry kCatalog[] = {
            {           "print","print(...)",                          "Writes its arguments to the script output.",ApiKind::global_function                                                                                                                              },
            {             "mem",                 "mem",             "The target's memory: typed reads, raw bytes and writes.",           ApiKind::table},
            {         "read_u8",    "read_u8(address)",                                    "Reads an unsigned 8-bit integer.", ApiKind::global_function},
            {         "read_i8",    "read_i8(address)",                                       "Reads a signed 8-bit integer.", ApiKind::global_function},
            {        "read_u16",   "read_u16(address)",                                   "Reads an unsigned 16-bit integer.", ApiKind::global_function},
            {        "read_i16",   "read_i16(address)",                                      "Reads a signed 16-bit integer.", ApiKind::global_function},
            {        "read_u32",   "read_u32(address)",                                   "Reads an unsigned 32-bit integer.", ApiKind::global_function},
            {        "read_i32",   "read_i32(address)",                                      "Reads a signed 32-bit integer.", ApiKind::global_function},
            {        "read_u64",   "read_u64(address)",                                   "Reads an unsigned 64-bit integer.", ApiKind::global_function},
            {        "read_i64",   "read_i64(address)",                                      "Reads a signed 64-bit integer.", ApiKind::global_function},
            {        "read_f32",   "read_f32(address)",                                               "Reads a 32-bit float.", ApiKind::global_function},
            {        "read_f64",   "read_f64(address)",                                               "Reads a 64-bit float.", ApiKind::global_function},
            {           "write",
             "write(address, value[, type])",              "Writes a value, inferring the type when it is omitted.",
             ApiKind::global_function                                                                                                                  },
            {         "rsymbol",
             "rsymbol(name[, value])",                "Registers a process-wide symbol with a value (or 0).",
             ApiKind::global_function                                                                                                                  },
            {         "ssymbol",
             "ssymbol(name, value)",    "Sets a process-wide symbol, registering the name when it is new.",
             ApiKind::global_function                                                                                                                  },
            {         "usymbol",       "usymbol(name)",                                      "Removes a process-wide symbol.", ApiKind::global_function},
            {          "rlabel",
             "rlabel(name[, value])",         "Registers a label owned by this script with a value (or 0).",
             ApiKind::global_function                                                                                                                  },
            {          "slabel",
             "slabel(name, value)",           "Sets a script label, registering the name when it is new.",
             ApiKind::global_function                                                                                                                  },
            {          "ulabel",        "ulabel(name)",                                             "Removes a script label.", ApiKind::global_function},
            {           "label",         "label(name)",                      "Resolves one of the script's own labels, or 0.", ApiKind::global_function},
            {          "symbol",        "symbol(name)",                    "Resolves a label or a process-wide symbol, or 0.", ApiKind::global_function},
            {      "expression",
             "expression(text)",          "Resolves a full address expression to an absolute address.",
             ApiKind::global_function                                                                                                                  },
            {           "alloc",
             "alloc(name, size_bytes[, near_address])",            "Maps target memory and publishes the address under name.",
             ApiKind::global_function                                                                                                                  },
            {         "dealloc",       "dealloc(name)",                      "Unmaps a mapping this session's alloc created.", ApiKind::global_function},
            {        "validate",
             "validate(address[, size_bytes])",           "True when the range is mapped and readable in the target.",
             ApiKind::global_function                                                                                                                  },
            {         "aobscan",
             "aobscan(name, pattern[, module])",             "Finds a byte pattern and stores its address under name.",
             ApiKind::global_function                                                                                                                  },
            {        "assemble",
             "assemble(address, text[, ...])",             "Assembles instructions and writes them into the target.",
             ApiKind::global_function                                                                                                                  },
            {            "hook",                "hook",               "Installs and removes inline hooks from a facts table.",           ApiKind::table},
            {"mem.pointer_size",  "mem.pointer_size()",                                "The target's pointer width in bytes.",  ApiKind::table_function},
            {        "mem.read",
             "mem.read(address, token)", "A typed read; tokens are u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 ptr.",
             ApiKind::table_function                                                                                                                   },
            {       "mem.write",
             "mem.write(address, token, value)",                    "A typed write, with the same tokens as mem.read.",
             ApiKind::table_function                                                                                                                   },
            {  "mem.read_bytes",
             "mem.read_bytes(address, size)",                             "Returns size raw bytes as a Lua string.",
             ApiKind::table_function                                                                                                                   },
            { "mem.read_string",
             "mem.read_string(address[, limit])",                                 "Reads bytes up to a NUL terminator.",
             ApiKind::table_function                                                                                                                   },
            { "mem.write_bytes",
             "mem.write_bytes(address, data)",                                        "Writes a Lua string's bytes.",
             ApiKind::table_function                                                                                                                   },
            {    "hook.install",
             "hook.install(spec)",                         "Installs an inline hook from a facts table.",
             ApiKind::table_function                                                                                                                   },
            {     "hook.remove",   "hook.remove(spec)",                                  "Removes a hook and frees its cave.",  ApiKind::table_function}
        };

        // The base globals `lua.open_libraries(base, string, table, math)` binds,
        // minus `print` (the engine rebinds it). The guard test checks every name
        // here against a live engine, so an entry for a library the engine does
        // not open cannot creep in.
        constexpr std::string_view kLuaStandardGlobals[] = {
            "_G",     "_VERSION", "assert",   "collectgarbage", "dofile",       "error",    "getmetatable",
            "ipairs", "load",     "loadfile", "next",           "pairs",        "pcall",    "rawequal",
            "rawget", "rawlen",   "rawset",   "select",         "setmetatable", "tonumber", "tostring",
            "type",   "warn",     "xpcall",   "string",         "table",        "math"};

        constexpr std::string_view kLuaLibraryMembers[] = {
            "string.byte",   "string.char",     "string.dump",     "string.find",    "string.format",
            "string.gmatch", "string.gsub",     "string.len",      "string.lower",   "string.match",
            "string.pack",   "string.packsize", "string.rep",      "string.reverse", "string.sub",
            "string.unpack", "string.upper",    "table.concat",    "table.insert",   "table.move",
            "table.pack",    "table.remove",    "table.sort",      "table.unpack",   "math.abs",
            "math.acos",     "math.asin",       "math.atan",       "math.ceil",      "math.cos",
            "math.deg",      "math.exp",        "math.floor",      "math.fmod",      "math.huge",
            "math.log",      "math.max",        "math.maxinteger", "math.min",       "math.mininteger",
            "math.modf",     "math.pi",         "math.rad",        "math.random",    "math.randomseed",
            "math.sin",      "math.sqrt",       "math.tan",        "math.tointeger", "math.type",
            "math.ult"};

        [[nodiscard]] bool starts_with_ci(std::string_view text, std::string_view prefix)
        {
            if (prefix.size() > text.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < prefix.size(); ++index)
            {
                const unsigned char left  = static_cast<unsigned char>(text[index]);
                const unsigned char right = static_cast<unsigned char>(prefix[index]);
                if (std::tolower(left) != std::tolower(right))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool has_dot(std::string_view name)
        {
            return name.find('.') != std::string_view::npos;
        }
    } // namespace

    std::span<const ApiEntry> api_catalog()
    {
        return kCatalog;
    }

    std::span<const std::string_view> lua_standard_globals()
    {
        return kLuaStandardGlobals;
    }

    std::span<const std::string_view> lua_library_members()
    {
        return kLuaLibraryMembers;
    }

    const ApiEntry* find(std::string_view name)
    {
        const auto found = std::ranges::find(api_catalog(), name, &ApiEntry::name);
        return found == api_catalog().end() ? nullptr : &*found;
    }

    std::vector<const ApiEntry*> members_of(std::string_view table)
    {
        std::vector<const ApiEntry*> members;
        for (const ApiEntry& entry : kCatalog)
        {
            const std::string_view name = entry.name;
            if (name.size() > table.size() && name[table.size()] == '.' && name.substr(0, table.size()) == table)
            {
                members.push_back(&entry);
            }
        }
        return members;
    }

    std::vector<const ApiEntry*> matches(std::string_view receiver, std::string_view prefix)
    {
        std::vector<const ApiEntry*> selected;
        for (const ApiEntry& entry : kCatalog)
        {
            const std::string_view name = entry.name;
            if (receiver.empty())
            {
                if (has_dot(name) || !starts_with_ci(name, prefix))
                {
                    continue;
                }
            }
            else
            {
                if (name.size() <= receiver.size() + 1 || name[receiver.size()] != '.'
                    || name.substr(0, receiver.size()) != receiver)
                {
                    continue;
                }
                if (!starts_with_ci(name.substr(receiver.size() + 1), prefix))
                {
                    continue;
                }
            }
            selected.push_back(&entry);
        }

        std::ranges::sort(selected, {}, &ApiEntry::name);
        return selected;
    }

    std::size_t parameter_index(std::string_view signature, std::size_t typed)
    {
        const std::size_t open = signature.find('(');
        if (open == std::string_view::npos)
        {
            return 0;
        }
        const std::size_t close = signature.find(')', open);

        std::size_t parameters = 0;
        int         depth      = 0;
        for (std::size_t index = open + 1;
             index < signature.size() && (close == std::string_view::npos || index < close);
             ++index)
        {
            const char character = signature[index];
            if (character == '(')
            {
                ++depth;
            }
            else if (character == ')')
            {
                --depth;
            }
            else if (character == ',' && depth == 0)
            {
                ++parameters; // `[optional]` brackets are notation, not nesting
            }
        }
        if (close != std::string_view::npos && close > open + 1)
        {
            ++parameters; // one more parameter than there are top-level commas
        }

        if (parameters == 0)
        {
            return 0;
        }
        return typed < parameters ? typed : parameters - 1;
    }
} // namespace slopkit::script
