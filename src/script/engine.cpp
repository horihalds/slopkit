#include "script/engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "disasm/assembler.hpp"
#include "expr/expression.hpp"
#include "expr/resolver.hpp"
#include "scan/pattern.hpp"
#include "script/codec.hpp"
#include "script/hook.hpp"
#include "script/symbols.hpp"

namespace slopkit::script
{
    namespace
    {
        // The VM-instruction hook fires every this many instructions; it is what
        // turns the instruction budget and the wall-clock deadline into an abort.
        constexpr int kHookInstructionCount = 1000;

        // How many bytes `mem.read_string` fetches per target read while it looks
        // for the NUL terminator, and the default upper bound when the script
        // passes none.
        constexpr std::size_t kStringChunk      = 64;
        constexpr std::size_t kStringDefaultMax = 256;

        // How many bytes `aobscan` reads per target read. Consecutive chunks
        // overlap by the pattern size so a match on a boundary is still found,
        // and a huge region never becomes one allocation.
        constexpr std::size_t kAobChunk = 64 * 1024;

        constexpr const char* kBudgetError   = "script exceeded its instruction budget";
        constexpr const char* kDeadlineError = "script exceeded its time budget";

        bool equals_case_insensitive(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                const auto lower = [](char value)
                {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
                };
                if (lower(left[i]) != lower(right[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // Pops the top of the stack and returns it as text, exactly like Lua's
        // `tostring` (which is what `print` uses for each of its arguments).
        std::string to_text(lua_State* state, int index)
        {
            std::size_t length      = 0;
            const char* text        = luaL_tolstring(state, index, &length);
            std::string description = text != nullptr ? std::string(text, length) : std::string {};
            lua_pop(state, 1); // luaL_tolstring always pushes its result
            return description;
        }

        // The identity of the global `update` in `state`, or nullptr when it is
        // absent or not a function. `run_update` compares it across the chunk
        // run, so only the hook the chunk itself defines counts -- a global left
        // behind by another script never does.
        const void* update_identity(lua_State* state)
        {
            const std::string name {kUpdateHook};
            lua_getglobal(state, name.c_str());
            const void* identity = lua_isfunction(state, -1) != 0 ? lua_topointer(state, -1) : nullptr;
            lua_pop(state, 1);
            return identity;
        }

        // The verdict a hook call returns: `ok` is false only for an explicit
        // `false` first value, and `message` is the second return value when
        // there is one. Shared by `run_lifecycle` and `run_update` so the two
        // cannot drift apart.
        struct HookVerdict
        {
            bool        ok {true};
            std::string message;
        };

        HookVerdict read_verdict(sol::protected_function_result& call, lua_State* state)
        {
            HookVerdict verdict;
            if (call.return_count() > 0)
            {
                const sol::object value = call.get<sol::object>(0);
                if (value.get_type() == sol::type::boolean && !value.as<bool>())
                {
                    verdict.ok = false;
                }
            }
            if (call.return_count() > 1)
            {
                const sol::object reason = call.get<sol::object>(1);
                reason.push(state);
                verdict.message = to_text(state, -1);
                lua_pop(state, 1); // the value pushed above
            }
            return verdict;
        }
    } // namespace

    struct Engine::Impl
    {
        MemoryApi    api;
        SymbolApi    symbols;
        EngineConfig config;
        sol::state   lua;

        // The script-local registry behind `rlabel`/`slabel`/`ulabel`, plus the
        // chunk it belongs to: a run of a different chunk drops them, so labels
        // never leak between scripts while a script's own hooks share theirs.
        SymbolTable                labels;
        std::optional<std::string> labels_owner;

        std::vector<std::string>              output;
        std::size_t                           ticks {0};
        std::chrono::steady_clock::time_point deadline;

        Impl(MemoryApi memory_api, SymbolApi symbol_api, EngineConfig engine_config)
            : api(std::move(memory_api)), symbols(std::move(symbol_api)), config(engine_config)
        {
            lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);
            // The instruction hook finds its Impl through the state's extra space.
            *static_cast<Impl**>(lua_getextraspace(lua.lua_state())) = this;
            install_print();
            install_memory();
            install_typed_access();
            install_symbols();
            install_labels();
            install_resolution();
            install_allocation();
            install_validation();
            install_aobscan();
            install_assembly();
            install_hook();
        }

        // The VM-instruction hook that turns the instruction budget and the
        // wall-clock deadline into an abort.
        static void budget_hook(lua_State* hooked, lua_Debug*)
        {
            auto* self = *static_cast<Impl**>(lua_getextraspace(hooked));
            self->ticks += 1;
            if (self->ticks * kHookInstructionCount > self->config.instruction_budget)
            {
                luaL_error(hooked, "%s", kBudgetError);
            }
            if (std::chrono::steady_clock::now() > self->deadline)
            {
                luaL_error(hooked, "%s", kDeadlineError);
            }
        }

        // Runs `body` (which makes exactly one protected call) under a fresh
        // instruction/time budget and always removes the hook afterwards. The
        // call's result is returned; a failing call leaves `valid()` false and
        // its error on the stack.
        template<typename Body>
        sol::protected_function_result call_guarded(Body&& body)
        {
            ticks            = 0;
            deadline         = std::chrono::steady_clock::now()
                             + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double>(config.timeout_seconds));
            lua_State* state = lua.lua_state();
            lua_sethook(state, &Impl::budget_hook, LUA_MASKCOUNT, kHookInstructionCount);
            sol::protected_function_result result = body();
            lua_sethook(state, nullptr, 0, 0);
            return result;
        }

        // Scopes the label table to the chunk currently running: a different
        // chunk starts with an empty registry while the same chunk keeps its
        // labels, so a script's `activate`/`deactivate` see what it registered.
        void begin_chunk(std::string_view chunk)
        {
            if (labels_owner && *labels_owner == chunk)
            {
                return;
            }
            labels.clear();
            labels_owner = std::string(chunk);
        }

        // `print` appends one line per call to the run's output instead of
        // writing to stdout.
        void install_print()
        {
            lua.set_function("print",
                             [this](sol::variadic_args arguments)
                             {
                                 lua_State*  state = lua.lua_state();
                                 std::string line;
                                 bool        first = true;
                                 for (sol::object argument : arguments)
                                 {
                                     if (!first)
                                     {
                                         line.push_back('\t');
                                     }
                                     first = false;
                                     argument.push(state);
                                     line += to_text(state, -1);
                                     lua_pop(state, 1);
                                 }
                                 if (output.size() >= kMaxOutputLines)
                                 {
                                     return; // the cap: further lines are dropped
                                 }
                                 if (line.size() > kMaxOutputLine)
                                 {
                                     line.resize(kMaxOutputLine);
                                     line += "\xE2\x80\xA6";
                                 }
                                 output.push_back(std::move(line));
                             });
        }

        void install_memory()
        {
            sol::table mem = lua.create_named_table("mem");

            mem.set_function("pointer_size",
                             [this]() -> std::size_t
                             {
                                 return api.pointer_size ? api.pointer_size() : 0;
                             });

            mem.set_function("read",
                             [this](std::uint64_t address, std::string token) -> sol::object
                             {
                                 const std::optional<std::size_t> width = type_width(token);
                                 if (!width)
                                 {
                                     throw std::runtime_error("mem.read: unknown value type '" + token + "'");
                                 }
                                 const double value = read_number(address, *width, token);
                                 // Integer tokens come back as Lua integers so a
                                 // value prints as `42`, not `42.0`.
                                 if (is_integer_token(token))
                                 {
                                     return sol::make_object(lua.lua_state(), static_cast<std::int64_t>(value));
                                 }
                                 return sol::make_object(lua.lua_state(), value);
                             });

            mem.set_function("read_bytes",
                             [this](std::uint64_t address, std::size_t size) -> std::string
                             {
                                 const std::vector<std::byte> bytes = read_raw(address, size);
                                 return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                             });

            mem.set_function("read_string",
                             [this](std::uint64_t address, sol::optional<std::size_t> limit) -> std::string
                             {
                                 return read_string(address, limit.value_or(kStringDefaultMax));
                             });

            mem.set_function("write",
                             [this](std::uint64_t address, std::string token, double value)
                             {
                                 const std::expected<std::vector<std::byte>, std::string> encoded =
                                     encode_number(token, value);
                                 if (!encoded)
                                 {
                                     throw std::runtime_error("mem.write: " + encoded.error());
                                 }
                                 write_raw(address, *encoded);
                             });

            mem.set_function("write_bytes",
                             [this](std::uint64_t address, std::string data)
                             {
                                 const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
                                 write_raw(address, std::span<const std::byte>(bytes, data.size()));
                             });
        }

        // The ten typed reads and the inferring `write`: the one-line
        // counterparts of `mem.read`/`mem.write` that name the type in the
        // function, over the same `script::codec` value vocabulary.
        void install_typed_access()
        {
            struct ReadBinding
            {
                const char* name;
                const char* token;
            };

            static constexpr ReadBinding kReads[] = {
                { "read_u8",  "u8"},
                { "read_i8",  "i8"},
                {"read_u16", "u16"},
                {"read_i16", "i16"},
                {"read_u32", "u32"},
                {"read_i32", "i32"},
                {"read_u64", "u64"},
                {"read_i64", "i64"},
                {"read_f32", "f32"},
                {"read_f64", "f64"},
            };
            for (const ReadBinding& binding : kReads)
            {
                const std::string name {binding.name};
                const std::string token {binding.token};
                lua.set_function(binding.name,
                                 [this, name, token](std::uint64_t address) -> sol::object
                                 {
                                     const std::size_t                        width   = *type_width(token);
                                     const std::vector<std::byte>             bytes   = read_for(name, address, width);
                                     const std::expected<double, std::string> decoded = decode_number(token, bytes);
                                     if (!decoded)
                                     {
                                         throw std::runtime_error(name + ": " + decoded.error());
                                     }
                                     // Integer tokens come back as Lua integers so a
                                     // value prints as `42`, not `42.0`.
                                     if (is_integer_token(token))
                                     {
                                         return sol::make_object(lua.lua_state(), static_cast<std::int64_t>(*decoded));
                                     }
                                     return sol::make_object(lua.lua_state(), *decoded);
                                 });
            }

            lua.set_function("write",
                             [this](std::uint64_t address, sol::object value, sol::optional<sol::object> type)
                             {
                                 if (value.get_type() != sol::type::number)
                                 {
                                     throw std::runtime_error("write: the value must be a number");
                                 }
                                 std::string token;
                                 if (type)
                                 {
                                     if (type->get_type() != sol::type::string)
                                     {
                                         throw std::runtime_error("write: the value type must be a string");
                                     }
                                     token = type->as<std::string>();
                                     if (!type_width(token))
                                     {
                                         throw std::runtime_error("write: unknown value type '" + token + "'");
                                     }
                                 }
                                 else
                                 {
                                     token = infer_token(value);
                                 }

                                 const std::expected<std::vector<std::byte>, std::string> encoded =
                                     encode_value(token, value);
                                 if (!encoded)
                                 {
                                     throw std::runtime_error("write: " + encoded.error());
                                 }
                                 write_for("write", address, *encoded);
                             });
        }

        // Turns a Lua value into a symbol value: `rsymbol` with no argument means
        // 0, and anything else must be an integral number in `0 .. 2^64-1`.
        std::uint64_t value_argument(const std::string& function, const sol::optional<sol::object>& value)
        {
            if (!value)
            {
                return 0;
            }
            if (value->get_type() != sol::type::number)
            {
                throw std::runtime_error(function + ": the value must be a 64-bit unsigned integer");
            }

            lua_State* state = lua.lua_state();
            value->push(state);
            bool          valid  = false;
            std::uint64_t result = 0;
            if (lua_isinteger(state, -1))
            {
                const lua_Integer integer = lua_tointeger(state, -1);
                valid                     = integer >= 0;
                result                    = static_cast<std::uint64_t>(integer);
            }
            else
            {
                const double number = lua_tonumber(state, -1);
                valid               = std::isfinite(number) && number >= 0.0 && number < 18446744073709551616.0
                                   && std::floor(number) == number;
                result              = static_cast<std::uint64_t>(number);
            }
            lua_pop(state, 1);
            if (!valid)
            {
                throw std::runtime_error(function + ": the value must be a 64-bit unsigned integer");
            }
            return result;
        }

        static std::string name_argument(const std::string& function, const sol::object& name)
        {
            if (name.get_type() != sol::type::string)
            {
                throw std::runtime_error(function + ": the name must be a string");
            }
            return name.as<std::string>();
        }

        void store_symbol(const std::string& function, std::string_view name, std::uint64_t value)
        {
            if (!symbols.set)
            {
                throw std::runtime_error(function + ": no symbol store is attached");
            }
            const std::expected<void, std::string> stored = symbols.set(name, value);
            if (!stored)
            {
                throw std::runtime_error(function + ": " + stored.error());
            }
        }

        // The value a name resolves to, and whether the script-local table
        // answered (`local`) or the process-wide symbols did.
        struct NameHit
        {
            std::uint64_t value {};
            bool          local {};
        };

        // Labels first, then the shared table; a name can live in either.
        [[nodiscard]] std::optional<NameHit> resolve_name(std::string_view name) const
        {
            if (const std::optional<std::uint64_t> local = labels.lookup(name))
            {
                return NameHit {*local, true};
            }
            if (symbols.lookup)
            {
                if (const std::optional<std::uint64_t> global = symbols.lookup(name))
                {
                    return NameHit {*global, false};
                }
            }
            return std::nullopt;
        }

        // Stores a label, mirroring `store_symbol`: a rejected name throws with
        // the function that asked for it.
        void store_label(const std::string& function, std::string_view name, std::uint64_t value)
        {
            const std::expected<void, std::string> stored = labels.set(name, value);
            if (!stored)
            {
                throw std::runtime_error(function + ": " + stored.error());
            }
        }

        // Binds `rsymbol`/`ssymbol`/`usymbol`; a bad argument or a rejected name
        // throws, which sol2 turns into a Lua error carrying the function name,
        // exactly like a failed `mem` access.
        void install_symbols()
        {
            lua.set_function("rsymbol",
                             [this](sol::object name, sol::optional<sol::object> value)
                             {
                                 store_symbol(
                                     "rsymbol", name_argument("rsymbol", name), value_argument("rsymbol", value));
                             });

            lua.set_function("ssymbol",
                             [this](sol::object name, sol::optional<sol::object> value)
                             {
                                 if (!value)
                                 {
                                     throw std::runtime_error("ssymbol: the value must be a 64-bit unsigned integer");
                                 }
                                 store_symbol(
                                     "ssymbol", name_argument("ssymbol", name), value_argument("ssymbol", value));
                             });

            lua.set_function("usymbol",
                             [this](sol::object name)
                             {
                                 if (!symbols.remove)
                                 {
                                     throw std::runtime_error("usymbol: no symbol store is attached");
                                 }
                                 const std::expected<void, std::string> removed =
                                     symbols.remove(name_argument("usymbol", name));
                                 if (!removed)
                                 {
                                     throw std::runtime_error("usymbol: " + removed.error());
                                 }
                             });
        }

        // Binds `rlabel`/`slabel`/`ulabel`, the script-local twin of the symbol
        // triple, with the same argument checks and error wording.
        void install_labels()
        {
            lua.set_function("rlabel",
                             [this](sol::object name, sol::optional<sol::object> value)
                             {
                                 store_label("rlabel", name_argument("rlabel", name), value_argument("rlabel", value));
                             });

            lua.set_function("slabel",
                             [this](sol::object name, sol::optional<sol::object> value)
                             {
                                 if (!value)
                                 {
                                     throw std::runtime_error("slabel: the value must be a 64-bit unsigned integer");
                                 }
                                 store_label("slabel", name_argument("slabel", name), value_argument("slabel", value));
                             });

            lua.set_function("ulabel",
                             [this](sol::object name)
                             {
                                 labels.remove(name_argument("ulabel", name));
                             });
        }

        // Binds `label(name)` (the script's own labels), `symbol(name)` (labels
        // first, then the process-wide table) and `expression(text)` (a full
        // address expression). An unknown name is the value 0, not an error; a
        // malformed name or an unresolvable expression throws under the
        // function's name.
        void install_resolution()
        {
            lua.set_function("label",
                             [this](sol::object name) -> std::uint64_t
                             {
                                 const std::string resolved_name = name_argument("label", name);
                                 if (const std::optional<std::string> rejected = validate_symbol_name(resolved_name))
                                 {
                                     throw std::runtime_error("label: " + *rejected);
                                 }
                                 return labels.lookup(resolved_name).value_or(0);
                             });

            lua.set_function("symbol",
                             [this](sol::object name) -> std::uint64_t
                             {
                                 const std::string resolved_name = name_argument("symbol", name);
                                 if (const std::optional<std::string> rejected = validate_symbol_name(resolved_name))
                                 {
                                     throw std::runtime_error("symbol: " + *rejected);
                                 }
                                 if (const std::optional<NameHit> hit = resolve_name(resolved_name))
                                 {
                                     return hit->value;
                                 }
                                 return 0;
                             });

            lua.set_function(
                "expression",
                [this](sol::object text) -> std::uint64_t
                {
                    if (text.get_type() != sol::type::string)
                    {
                        throw std::runtime_error("expression: the expression must be a string");
                    }
                    const std::string source = text.as<std::string>();

                    const std::expected<expr::Expression, expr::Error> parsed = expr::parse(source);
                    if (!parsed)
                    {
                        throw std::runtime_error("expression: " + parsed.error().message);
                    }

                    // The base is a script label first, then a
                    // process-wide symbol, then (via the evaluator)
                    // a module name and a literal. Passing only the
                    // name that answered makes the script's own name
                    // beat a module with the same spelling.
                    std::vector<expr::SymbolRef> symbol_entries;
                    std::vector<expr::ModuleRef> module_entries;
                    if (const std::optional<NameHit> hit = resolve_name(parsed->base))
                    {
                        symbol_entries.push_back(expr::SymbolRef {parsed->base, hit->value});
                    }
                    else if (api.modules)
                    {
                        if (std::expected<std::vector<expr::ModuleRef>, std::string> listed = api.modules())
                        {
                            module_entries = std::move(*listed);
                        }
                    }

                    const std::size_t pointer_size = api.pointer_size ? api.pointer_size() : sizeof(std::uint64_t);
                    const auto        reader =
                        [this, pointer_size](std::uint64_t address) -> std::expected<std::uint64_t, std::string>
                    {
                        if (!api.read)
                        {
                            return std::unexpected(std::string {"no target is attached"});
                        }
                        std::expected<std::vector<std::byte>, std::string> bytes = api.read(address, pointer_size);
                        if (!bytes)
                        {
                            return std::unexpected(bytes.error());
                        }
                        return decode_pointer_value(*bytes);
                    };
                    // Validate each dereference through the same seam, so a
                    // script-level chain names the failing level.
                    const auto validator = [this](std::uint64_t address,
                                                  std::size_t   size) -> std::expected<bool, std::string>
                    {
                        if (!api.validate)
                        {
                            return std::unexpected(std::string {"no target is attached"});
                        }
                        return api.validate(address, size);
                    };

                    const std::expected<std::uint64_t, expr::ResolveError> resolved =
                        expr::evaluate(*parsed,
                                       module_entries,
                                       symbol_entries,
                                       reader,
                                       expr::Options {.pointer_size = pointer_size},
                                       validator);
                    if (!resolved)
                    {
                        throw std::runtime_error("expression: " + resolved.error().message);
                    }
                    return *resolved;
                });
        }

        // Binds `alloc(name, size[, near])` and `dealloc(name)`: map and unmap
        // memory in the target from a script. `alloc` publishes the address with
        // `aobscan`'s rule, so a label or symbol of that name sees it too.
        void install_allocation()
        {
            lua.set_function(
                "alloc",
                [this](sol::object name, sol::object size, sol::optional<sol::object> near) -> std::uint64_t
                {
                    const std::string label_name = name_argument("alloc", name);
                    if (const std::optional<std::string> rejected = validate_symbol_name(label_name))
                    {
                        throw std::runtime_error("alloc: " + *rejected);
                    }

                    if (size.get_type() != sol::type::number)
                    {
                        throw std::runtime_error("alloc: the size must be a number");
                    }
                    lua_State* state = lua.lua_state();
                    size.push(state);
                    const bool        is_integer = lua_isinteger(state, -1) != 0;
                    const lua_Integer integer    = is_integer ? lua_tointeger(state, -1) : 0;
                    const double      number     = is_integer ? 0.0 : lua_tonumber(state, -1);
                    lua_pop(state, 1);
                    const double checked_size = is_integer ? static_cast<double>(integer) : number;
                    if (!std::isfinite(checked_size) || checked_size <= 0.0)
                    {
                        throw std::runtime_error("alloc: the size must be larger than 0");
                    }
                    const std::size_t size_bytes =
                        is_integer ? static_cast<std::size_t>(integer) : static_cast<std::size_t>(number);

                    std::uint64_t near_address = 0;
                    if (near)
                    {
                        near_address = value_argument("alloc", near);
                    }

                    if (!api.allocate)
                    {
                        throw std::runtime_error("alloc: the target's plugin cannot allocate memory");
                    }
                    const std::expected<std::uint64_t, std::string> address = api.allocate(size_bytes, near_address);
                    if (!address)
                    {
                        throw std::runtime_error("alloc: " + address.error());
                    }
                    store_hit("alloc", label_name, *address);
                    return *address;
                });

            lua.set_function(
                "dealloc",
                [this](sol::object name)
                {
                    const std::string label_name = name_argument("dealloc", name);
                    if (const std::optional<std::string> rejected = validate_symbol_name(label_name))
                    {
                        throw std::runtime_error("dealloc: " + *rejected);
                    }
                    if (!api.deallocate)
                    {
                        throw std::runtime_error("dealloc: the target's plugin cannot allocate memory");
                    }

                    const std::optional<NameHit> hit = resolve_name(label_name);
                    if (!hit)
                    {
                        throw std::runtime_error("dealloc: " + label_name + " is not this session's allocation");
                    }

                    const std::expected<void, std::string> freed = api.deallocate(hit->value);
                    if (!freed)
                    {
                        if (freed.error() == "not this session's allocation")
                        {
                            throw std::runtime_error("dealloc: " + label_name + " is not this session's allocation");
                        }
                        throw std::runtime_error("dealloc: " + freed.error());
                    }
                });
        }

        // Binds `validate(address[, size])`: true when [address, address + size)
        // is mapped and readable in the target, false otherwise. `size` defaults
        // to 1 and is probed in bounded chunks. An unmapped or unreadable range
        // is `false`, not an error; a zero size, a bad argument or a missing
        // target raises.
        void install_validation()
        {
            lua.set_function(
                "validate",
                [this](sol::object address_object, sol::optional<sol::object> size_object) -> bool
                {
                    const std::uint64_t address = value_argument("validate", sol::make_optional(address_object));

                    std::size_t size = 1;
                    if (size_object)
                    {
                        if (size_object->get_type() != sol::type::number)
                        {
                            throw std::runtime_error("validate: the size must be a number");
                        }
                        lua_State* state = lua.lua_state();
                        size_object->push(state);
                        const bool        is_integer = lua_isinteger(state, -1) != 0;
                        const lua_Integer integer    = is_integer ? lua_tointeger(state, -1) : 0;
                        const double      number     = is_integer ? 0.0 : lua_tonumber(state, -1);
                        lua_pop(state, 1);
                        const double checked_size = is_integer ? static_cast<double>(integer) : number;
                        if (!std::isfinite(checked_size) || checked_size <= 0.0)
                        {
                            throw std::runtime_error("validate: the size must be larger than 0");
                        }
                        size = is_integer ? static_cast<std::size_t>(integer) : static_cast<std::size_t>(number);
                    }

                    if (!api.validate)
                    {
                        throw std::runtime_error("validate: no target is attached");
                    }
                    const std::expected<bool, std::string> valid = api.validate(address, size);
                    if (!valid)
                    {
                        throw std::runtime_error("validate: " + valid.error());
                    }
                    return *valid;
                });
        }

        // Binds `assemble(address, text[, ...])`: turns a block of the listing's
        // instructions into bytes with `disasm::assemble_block` and writes them
        // into the target at `address`. Extra arguments expand the text with
        // Lua's `string.format` first. A wrong argument type raises; everything
        // else is a `false, reason` pair, so a typo writes nothing.
        void install_assembly()
        {
            lua.set_function(
                "assemble",
                [this](sol::object        address_object,
                       sol::object        text_object,
                       sol::variadic_args format) -> sol::variadic_results
                {
                    const std::uint64_t address = value_argument("assemble", sol::make_optional(address_object));
                    if (text_object.get_type() != sol::type::string)
                    {
                        throw std::runtime_error("assemble: the text must be a string");
                    }

                    const auto failed = [this](std::string reason) -> sol::variadic_results
                    {
                        sol::variadic_results results;
                        results.push_back(sol::make_object(lua.lua_state(), false));
                        results.push_back(sol::make_object(lua.lua_state(), std::move(reason)));
                        return results;
                    };

                    lua_State*  state = lua.lua_state();
                    std::string text  = text_object.as<std::string>();
                    if (format.size() > 0)
                    {
                        // The expansion runs on the Lua stack, so every extra
                        // argument is passed to `string.format` as it is.
                        const int stack_top = lua_gettop(state);
                        lua_getglobal(state, "string");
                        lua_getfield(state, -1, "format");
                        lua_remove(state, -2);
                        lua_pushlstring(state, text.data(), text.size());
                        for (sol::object argument : format)
                        {
                            argument.push(state);
                        }
                        if (lua_pcall(state, static_cast<int>(format.size()) + 1, 1, 0) != LUA_OK)
                        {
                            const std::string reason = to_text(state, -1);
                            lua_settop(state, stack_top);
                            return failed(reason);
                        }
                        std::size_t length   = 0;
                        const char* expanded = lua_tolstring(state, -1, &length);
                        text                 = expanded != nullptr ? std::string(expanded, length) : std::string {};
                        lua_settop(state, stack_top);
                    }

                    const std::size_t         pointer_size = api.pointer_size ? api.pointer_size() : 8;
                    const disasm::MachineMode mode =
                        pointer_size == 4 ? disasm::MachineMode::legacy_32 : disasm::MachineMode::long_64;

                    const std::expected<std::vector<std::byte>, std::string> bytes =
                        disasm::assemble_block(text, disasm::AssembleBlockContext {.base = address, .mode = mode});
                    if (!bytes)
                    {
                        return failed(bytes.error());
                    }
                    if (!api.write)
                    {
                        return failed("no target is attached");
                    }
                    const std::expected<void, std::string> written = api.write(address, *bytes);
                    if (!written)
                    {
                        return failed(written.error());
                    }

                    sol::variadic_results results;
                    results.push_back(sol::make_object(state, true));
                    results.push_back(sol::make_object(state, static_cast<std::int64_t>(bytes->size())));
                    return results;
                });
        }

        // Stores the address of a successful scan under `name`: into the label
        // the script already owns, else into an existing global symbol (so an
        // address field can resolve it), else into a freshly registered label.
        void store_hit(const std::string& function, std::string_view name, std::uint64_t address)
        {
            if (const std::optional<NameHit> hit = resolve_name(name); hit && !hit->local)
            {
                const std::expected<void, std::string> stored = symbols.set(name, address);
                if (!stored)
                {
                    throw std::runtime_error(function + ": " + stored.error());
                }
                return;
            }
            store_label(function, name, address);
        }

        // Walks the readable regions in ascending address order and collects the
        // lowest `limit` addresses where `pattern` matches. `module`, when set,
        // restricts the walk to that module's regions. Every failure is thrown
        // with `function`'s name, never a `longjmp`, so the walk's containers are
        // destroyed.
        std::vector<std::uint64_t> scan_memory(const scan::BytePattern&          pattern,
                                               const std::optional<std::string>& module,
                                               std::size_t                       limit,
                                               std::string_view                  function)
        {
            if (!api.regions)
            {
                throw std::runtime_error(std::string {function} + ": no target is attached");
            }
            const std::expected<std::vector<MemoryRegion>, std::string> listed = api.regions();
            if (!listed)
            {
                throw std::runtime_error(std::string {function} + ": " + listed.error());
            }

            std::vector<MemoryRegion> regions;
            for (const MemoryRegion& region : *listed)
            {
                if (!region.readable || (module && !equals_case_insensitive(region.module, *module)))
                {
                    continue;
                }
                regions.push_back(region);
            }

            if (module && regions.empty())
            {
                throw std::runtime_error(std::string {function} + ": no region belongs to module '" + *module + "'");
            }
            if (regions.empty())
            {
                throw std::runtime_error(std::string {function} + ": the target reported no readable memory");
            }

            std::ranges::sort(regions, {}, &MemoryRegion::base);

            std::vector<std::uint64_t> matches;
            const std::size_t          pattern_size = pattern.size();
            const std::size_t          overlap      = pattern_size - 1;
            const std::size_t          step         = std::max(kAobChunk, pattern_size);
            for (const MemoryRegion& region : regions)
            {
                if (region.size < pattern_size)
                {
                    continue;
                }
                std::uint64_t       cursor = region.base;
                const std::uint64_t end    = region.base + region.size;
                while (cursor < end)
                {
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        throw std::runtime_error(kDeadlineError);
                    }
                    const std::uint64_t remaining = end - cursor;
                    const std::size_t   want      = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, step));
                    const std::expected<std::vector<std::byte>, std::string> bytes = api.read(cursor, want);
                    if (!bytes)
                    {
                        break; // abandon this region and continue with the next
                    }
                    for (std::size_t from = 0; matches.size() < limit;)
                    {
                        const std::optional<std::size_t> offset = pattern.find(*bytes, from);
                        if (!offset)
                        {
                            break;
                        }
                        const std::uint64_t address = cursor + *offset;
                        // Consecutive chunks overlap, so a match near a chunk's
                        // end can surface once more in the next one.
                        if (matches.empty() || address != matches.back())
                        {
                            matches.push_back(address);
                        }
                        from = *offset + 1;
                    }
                    if (matches.size() >= limit)
                    {
                        return matches;
                    }
                    if (want >= remaining)
                    {
                        break; // the chunk reached the region's end
                    }
                    cursor += want - overlap;
                }
            }
            return matches;
        }

        // Binds `aobscan(name, pattern[, module])`: it walks the target's
        // readable memory for a wildcard byte pattern, stores the address of the
        // first match under `name` and returns whether it was found, with the
        // address as a second value.
        void install_aobscan()
        {
            lua.set_function(
                "aobscan",
                [this](
                    sol::object name, sol::object pattern, sol::optional<sol::object> module) -> sol::variadic_results
                {
                    const std::string scan_name = name_argument("aobscan", name);
                    if (const std::optional<std::string> rejected = validate_symbol_name(scan_name))
                    {
                        throw std::runtime_error("aobscan: " + *rejected);
                    }
                    if (pattern.get_type() != sol::type::string)
                    {
                        throw std::runtime_error("aobscan: the pattern must be a string");
                    }
                    std::optional<std::string> module_name;
                    if (module)
                    {
                        if (module->get_type() != sol::type::string)
                        {
                            throw std::runtime_error("aobscan: the module must be a string");
                        }
                        module_name = module->as<std::string>();
                    }

                    const std::expected<scan::BytePattern, std::string> compiled =
                        scan::BytePattern::parse(pattern.as<std::string>());
                    if (!compiled)
                    {
                        throw std::runtime_error("aobscan: " + compiled.error());
                    }

                    sol::variadic_results            results;
                    const std::vector<std::uint64_t> matches = scan_memory(*compiled, module_name, 1, "aobscan");
                    if (!matches.empty())
                    {
                        store_hit("aobscan", scan_name, matches.front());
                        results.push_back(sol::make_object(lua.lua_state(), true));
                        results.push_back(
                            sol::make_object(lua.lua_state(), static_cast<std::int64_t>(matches.front())));
                    }
                    else
                    {
                        results.push_back(sol::make_object(lua.lua_state(), false));
                    }
                    return results;
                });
        }

        // Reads a required string field of a hook facts table, raising under the
        // calling function's name when it is missing or not a string.
        std::string hook_string(sol::table table, std::string_view field, std::string_view function)
        {
            sol::object value = table[std::string {field}];
            if (value.get_type() != sol::type::string)
            {
                throw std::runtime_error(std::string {function} + ": the " + std::string {field}
                                         + " field must be a string");
            }
            return value.as<std::string>();
        }

        // Parses a generated hook's facts table into a `hook::Spec`, raising
        // under the calling function's name when a field has the wrong type.
        [[nodiscard]] hook::Spec parse_hook(sol::table table, std::string_view function)
        {
            hook::Spec spec;
            spec.site_name = hook_string(table, "site_name", function);
            spec.cave_name = hook_string(table, "cave_name", function);
            spec.pattern   = hook_string(table, "pattern", function);

            if (sol::object module = table[std::string {"module"}]; module.get_type() == sol::type::string)
            {
                spec.module = module.as<std::string>();
            }
            else if (module.get_type() != sol::type::nil && module.get_type() != sol::type::none)
            {
                throw std::runtime_error(std::string {function} + ": the module field must be a string");
            }

            if (sol::object offset = table[std::string {"offset"}]; offset.get_type() != sol::type::nil)
            {
                spec.offset =
                    static_cast<std::size_t>(value_argument(std::string {function}, sol::make_optional(offset)));
            }

            sol::object original = table[std::string {"original"}];
            if (original.get_type() != sol::type::string)
            {
                throw std::runtime_error(std::string {function} + ": the original field must be a string");
            }
            const std::string original_bytes = original.as<std::string>();
            spec.original.reserve(original_bytes.size());
            for (const char byte : original_bytes)
            {
                spec.original.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
            }

            spec.code_text       = hook_string(table, "code", function);
            spec.trampoline_text = hook_string(table, "trampoline", function);

            if (sol::object arguments = table[std::string {"trampoline_args"}];
                arguments.get_type() == sol::type::table)
            {
                const sol::table  list  = arguments.as<sol::table>();
                const std::size_t count = list.size();
                for (std::size_t index = 1; index <= count; ++index)
                {
                    sol::object entry = list[index];
                    if (entry.get_type() != sol::type::number)
                    {
                        throw std::runtime_error(std::string {function}
                                                 + ": the trampoline_args field must hold numbers");
                    }
                    const double number = entry.as<double>();
                    if (!std::isfinite(number) || std::floor(number) != number)
                    {
                        throw std::runtime_error(std::string {function}
                                                 + ": the trampoline_args field must hold whole numbers");
                    }
                    spec.address_args.push_back(static_cast<std::int64_t>(number));
                }
            }
            else if (arguments.get_type() != sol::type::nil && arguments.get_type() != sol::type::none)
            {
                throw std::runtime_error(std::string {function} + ": the trampoline_args field must be a table");
            }

            if (sol::object cave_size = table[std::string {"cave_size"}]; cave_size.get_type() != sol::type::nil)
            {
                spec.cave_size =
                    static_cast<std::size_t>(value_argument(std::string {function}, sol::make_optional(cave_size)));
            }
            return spec;
        }

        // Binds `hook.install(spec)` and `hook.remove(spec)`, the two calls a
        // generated hook script makes. `install` demands a unique pattern,
        // verifies the site, maps a cave near it and patches the site; `remove`
        // puts the originals back and frees the cave. Both return `true` alone on
        // success, or `false, reason` on a refusal.
        void install_hook()
        {
            sol::table hook_table = lua.create_named_table("hook");

            const auto failed = [this](std::string reason) -> sol::variadic_results
            {
                sol::variadic_results results;
                results.push_back(sol::make_object(lua.lua_state(), false));
                results.push_back(sol::make_object(lua.lua_state(), std::move(reason)));
                return results;
            };
            const auto succeeded = [this]() -> sol::variadic_results
            {
                sol::variadic_results results;
                results.push_back(sol::make_object(lua.lua_state(), true));
                return results;
            };

            hook_table.set_function(
                "install",
                [this, failed, succeeded](sol::object spec_object) -> sol::variadic_results
                {
                    if (spec_object.get_type() != sol::type::table)
                    {
                        throw std::runtime_error("hook.install: the argument must be a table");
                    }
                    const hook::Spec spec = parse_hook(spec_object.as<sol::table>(), "hook.install");

                    const std::expected<scan::BytePattern, std::string> compiled =
                        scan::BytePattern::parse(spec.pattern);
                    if (!compiled)
                    {
                        throw std::runtime_error("hook.install: " + compiled.error());
                    }

                    const std::optional<std::string> module =
                        spec.module.empty() ? std::nullopt : std::make_optional(spec.module);
                    const std::vector<std::uint64_t> matches = scan_memory(*compiled, module, 2, "hook.install");
                    if (matches.empty())
                    {
                        return failed("the hook pattern was not found");
                    }
                    if (matches.size() > 1)
                    {
                        return failed("the hook pattern matches more than one place");
                    }

                    const std::uint64_t                             site      = matches.front() + spec.offset;
                    const std::expected<std::uint64_t, std::string> installed = hook::install(spec, api, site);
                    if (!installed)
                    {
                        return failed(installed.error());
                    }

                    store_hit("hook.install", spec.site_name, site);
                    store_hit("hook.install", spec.cave_name, *installed);
                    return succeeded();
                });

            hook_table.set_function("remove",
                                    [this, failed, succeeded](sol::object spec_object) -> sol::variadic_results
                                    {
                                        if (spec_object.get_type() != sol::type::table)
                                        {
                                            throw std::runtime_error("hook.remove: the argument must be a table");
                                        }
                                        const hook::Spec spec = parse_hook(spec_object.as<sol::table>(), "hook.remove");

                                        const std::optional<NameHit> site = resolve_name(spec.site_name);
                                        if (!site)
                                        {
                                            return failed("this hook's site is not known anymore");
                                        }
                                        const std::optional<NameHit> cave = resolve_name(spec.cave_name);

                                        const std::expected<void, std::string> removed =
                                            hook::remove(spec, api, site->value, cave ? cave->value : 0);
                                        if (!removed)
                                        {
                                            return failed(removed.error());
                                        }
                                        labels.remove(spec.site_name);
                                        labels.remove(spec.cave_name);
                                        return succeeded();
                                    });
        }

        std::vector<std::byte> read_raw(std::uint64_t address, std::size_t size)
        {
            if (!api.read)
            {
                throw std::runtime_error("mem.read: no target is attached");
            }
            std::expected<std::vector<std::byte>, std::string> bytes = api.read(address, size);
            if (!bytes)
            {
                throw std::runtime_error("mem.read: " + bytes.error());
            }
            return std::move(*bytes);
        }

        double read_number(std::uint64_t address, std::size_t width, std::string_view token)
        {
            const std::vector<std::byte>             bytes   = read_raw(address, width);
            const std::expected<double, std::string> decoded = decode_number(token, bytes);
            if (!decoded)
            {
                throw std::runtime_error("mem.read: " + decoded.error());
            }
            return *decoded;
        }

        void write_raw(std::uint64_t address, std::span<const std::byte> bytes)
        {
            if (!api.write)
            {
                throw std::runtime_error("mem.write: no target is attached");
            }
            const std::expected<void, std::string> written = api.write(address, bytes);
            if (!written)
            {
                throw std::runtime_error("mem.write: " + written.error());
            }
        }

        // Reads `size` bytes for one of the typed globals: the same seam as
        // `mem.read`, but every failure names the function the script called.
        std::vector<std::byte> read_for(const std::string& function, std::uint64_t address, std::size_t size)
        {
            if (!api.read)
            {
                throw std::runtime_error(function + ": no target is attached");
            }
            std::expected<std::vector<std::byte>, std::string> bytes = api.read(address, size);
            if (!bytes)
            {
                throw std::runtime_error(function + ": " + bytes.error());
            }
            return std::move(*bytes);
        }

        void write_for(const std::string& function, std::uint64_t address, std::span<const std::byte> bytes)
        {
            if (!api.write)
            {
                throw std::runtime_error(function + ": no target is attached");
            }
            const std::expected<void, std::string> written = api.write(address, bytes);
            if (!written)
            {
                throw std::runtime_error(function + ": " + written.error());
            }
        }

        // The narrowest type that holds a plain Lua value, so `write(addr, v)`
        // needs no token for the common cases; a float writes `f32`.
        std::string infer_token(const sol::object& value)
        {
            lua_State* state = lua.lua_state();
            value.push(state);
            const bool        is_integer = lua_isinteger(state, -1) != 0;
            const lua_Integer integer    = is_integer ? lua_tointeger(state, -1) : 0;
            lua_pop(state, 1);

            if (!is_integer)
            {
                return "f32";
            }
            if (integer >= 0)
            {
                const std::uint64_t unsigned_value = static_cast<std::uint64_t>(integer);
                if (unsigned_value <= 0xFF)
                {
                    return "u8";
                }
                if (unsigned_value <= 0xFFFF)
                {
                    return "u16";
                }
                if (unsigned_value <= 0xFFFFFFFF)
                {
                    return "u32";
                }
                return "u64";
            }
            if (integer >= -128)
            {
                return "i8";
            }
            if (integer >= -0x8000)
            {
                return "i16";
            }
            if (integer >= -0x80000000LL)
            {
                return "i32";
            }
            return "i64";
        }

        // Encodes a Lua value for `write`: an integer token keeps the exact 64
        // bits of a Lua integer, everything else goes through `encode_number`.
        std::expected<std::vector<std::byte>, std::string> encode_value(const std::string& token,
                                                                        const sol::object& value)
        {
            lua_State* state = lua.lua_state();
            value.push(state);
            const bool        is_integer = lua_isinteger(state, -1) != 0;
            const lua_Integer integer    = is_integer ? lua_tointeger(state, -1) : 0;
            const double      number     = is_integer ? 0.0 : lua_tonumber(state, -1);
            lua_pop(state, 1);

            if (is_integer && is_integer_token(token))
            {
                return encode_integer(token, static_cast<std::uint64_t>(integer));
            }
            return encode_number(token, is_integer ? static_cast<double>(integer) : number);
        }

        // Little-endian pointer for an `expression` dereference.
        static std::expected<std::uint64_t, std::string> decode_pointer_value(std::span<const std::byte> bytes)
        {
            if (bytes.empty() || bytes.size() > sizeof(std::uint64_t))
            {
                return std::unexpected(std::string {"the target returned a bad pointer width"});
            }
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[i])) << (8 * i);
            }
            return value;
        }

        std::string read_string(std::uint64_t address, std::size_t limit)
        {
            std::string text;
            std::size_t offset = 0;
            while (text.size() < limit)
            {
                const std::size_t            chunk  = std::min(kStringChunk, limit - text.size());
                const std::vector<std::byte> bytes  = read_raw(address + offset, chunk);
                std::size_t                  length = 0;
                while (length < bytes.size() && bytes[length] != std::byte {0})
                {
                    ++length;
                }
                text.append(reinterpret_cast<const char*>(bytes.data()), length);
                if (length < bytes.size())
                {
                    break; // the NUL terminator
                }
                offset += chunk;
            }
            return text;
        }
    };

    Engine::Engine(MemoryApi api, SymbolApi symbols, EngineConfig config)
        : impl_(std::make_unique<Impl>(std::move(api), std::move(symbols), config))
    {
    }

    Engine::~Engine()                            = default;
    Engine::Engine(Engine&&) noexcept            = default;
    Engine& Engine::operator=(Engine&&) noexcept = default;

    RunResult Engine::run(std::string_view chunk)
    {
        Impl&      impl  = *impl_;
        lua_State* state = impl.lua.lua_state();

        RunResult result;
        impl.output.clear();
        impl.begin_chunk(chunk);

        sol::load_result loaded = impl.lua.load(chunk, "@script");
        if (!loaded.valid())
        {
            const sol::error error = loaded;
            result.error           = error.what();
            return result;
        }

        sol::protected_function        function = loaded;
        sol::protected_function_result call     = impl.call_guarded(
            [&function]
            {
                return function();
            });

        result.output = std::move(impl.output);

        if (!call.valid())
        {
            const sol::error error = call;
            result.error           = error.what();
            return result;
        }

        if (call.return_count() > 0)
        {
            sol::object value = call.get<sol::object>(0);
            value.push(state);
            const int type = lua_type(state, -1);
            if (type == LUA_TBOOLEAN || type == LUA_TNUMBER || type == LUA_TSTRING)
            {
                result.returned = to_text(state, -1);
            }
            lua_pop(state, 1); // the value pushed above
        }

        result.ok = true;
        return result;
    }

    std::expected<void, std::string> check_syntax(std::string_view chunk)
    {
        lua_State* state = luaL_newstate();
        if (state == nullptr)
        {
            return std::unexpected(std::string {"the Lua state could not be created"});
        }

        const char* data   = chunk.empty() ? "" : chunk.data();
        const int   status = luaL_loadbufferx(state, data, chunk.size(), "@script", nullptr);

        std::expected<void, std::string> result;
        if (status != LUA_OK)
        {
            result = std::unexpected(to_text(state, -1));
        }
        lua_close(state); // the loaded function (or the error) dies with the state
        return result;
    }

    LifecycleResult Engine::run_lifecycle(std::string_view chunk, std::string_view function)
    {
        Impl&      impl  = *impl_;
        lua_State* state = impl.lua.lua_state();

        LifecycleResult result;
        impl.output.clear();
        impl.begin_chunk(chunk);

        sol::load_result loaded = impl.lua.load(chunk, "@script");
        if (!loaded.valid())
        {
            const sol::error error = loaded;
            result.error           = error.what();
            result.output          = std::move(impl.output);
            return result;
        }

        // The chunk runs first so its globals -- the hooks among them -- are
        // defined; a chunk that raises never reaches its hook.
        sol::protected_function        chunk_function = loaded;
        sol::protected_function_result chunk_call     = impl.call_guarded(
            [&chunk_function]
            {
                return chunk_function();
            });
        if (!chunk_call.valid())
        {
            const sol::error error = chunk_call;
            result.error           = error.what();
            result.output          = std::move(impl.output);
            return result;
        }

        sol::object hook = impl.lua[std::string(function)];
        if (hook.get_type() != sol::type::function)
        {
            result.error  = std::string(function) + "() is not defined";
            result.output = std::move(impl.output);
            return result;
        }

        sol::protected_function        hook_function = hook;
        sol::protected_function_result hook_call     = impl.call_guarded(
            [&hook_function]
            {
                return hook_function();
            });

        result.output = std::move(impl.output);

        if (!hook_call.valid())
        {
            const sol::error error = hook_call;
            result.error           = error.what();
            return result;
        }

        // Nothing, or a truthy first value, is a success; an explicit `false` is
        // a refusal. A second return value is the message either way.
        const HookVerdict verdict = read_verdict(hook_call, state);
        result.ok                 = verdict.ok;
        result.message            = verdict.message;

        return result;
    }

    UpdateResult Engine::run_update(std::string_view chunk)
    {
        Impl&      impl  = *impl_;
        lua_State* state = impl.lua.lua_state();

        UpdateResult result;
        impl.output.clear();
        impl.begin_chunk(chunk);

        // Another script may have left an `update` global behind, so remember
        // what the chunk starts with: only a function this very chunk defines is
        // its own hook, which is what makes a hook-less chunk a silent skip.
        const void* before = update_identity(state);

        sol::load_result loaded = impl.lua.load(chunk, "@script");
        if (!loaded.valid())
        {
            const sol::error error = loaded;
            result.error           = error.what();
            result.ok              = false;
            result.output          = std::move(impl.output);
            return result;
        }

        sol::protected_function        chunk_function = loaded;
        sol::protected_function_result chunk_call     = impl.call_guarded(
            [&chunk_function]
            {
                return chunk_function();
            });
        if (!chunk_call.valid())
        {
            const sol::error error = chunk_call;
            result.error           = error.what();
            result.ok              = false;
            result.output          = std::move(impl.output);
            return result;
        }

        const void* after = update_identity(state);
        if (after == nullptr || after == before)
        {
            // The chunk defines no hook of its own: skipped silently and the
            // tick counts as a success.
            result.output = std::move(impl.output);
            return result;
        }

        result.ran = true;

        sol::object                    hook          = impl.lua[std::string(kUpdateHook)];
        sol::protected_function        hook_function = hook;
        sol::protected_function_result hook_call     = impl.call_guarded(
            [&hook_function]
            {
                return hook_function();
            });

        result.output = std::move(impl.output);

        if (!hook_call.valid())
        {
            const sol::error error = hook_call;
            result.error           = error.what();
            result.ok              = false;
            return result;
        }

        const HookVerdict verdict = read_verdict(hook_call, state);
        result.ok                 = verdict.ok;
        result.message            = verdict.message;

        return result;
    }

} // namespace slopkit::script
