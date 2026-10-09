#include "script/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "script/codec.hpp"

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

        constexpr const char* kBudgetError   = "script exceeded its instruction budget";
        constexpr const char* kDeadlineError = "script exceeded its time budget";

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
    } // namespace

    struct Engine::Impl
    {
        MemoryApi    api;
        EngineConfig config;
        sol::state   lua;

        std::vector<std::string>              output;
        std::size_t                           ticks {0};
        std::chrono::steady_clock::time_point deadline;

        Impl(MemoryApi memory_api, EngineConfig engine_config) : api(std::move(memory_api)), config(engine_config)
        {
            lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);
            // The instruction hook finds its Impl through the state's extra space.
            *static_cast<Impl**>(lua_getextraspace(lua.lua_state())) = this;
            install_print();
            install_memory();
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

    Engine::Engine(MemoryApi api, EngineConfig config) : impl_(std::make_unique<Impl>(std::move(api), config)) {}

    Engine::~Engine()                            = default;
    Engine::Engine(Engine&&) noexcept            = default;
    Engine& Engine::operator=(Engine&&) noexcept = default;

    RunResult Engine::run(std::string_view chunk)
    {
        Impl&      impl  = *impl_;
        lua_State* state = impl.lua.lua_state();

        RunResult result;
        impl.output.clear();

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

    LifecycleResult Engine::run_lifecycle(std::string_view chunk, std::string_view function)
    {
        Impl&      impl  = *impl_;
        lua_State* state = impl.lua.lua_state();

        LifecycleResult result;
        impl.output.clear();

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
        result.ok = true;
        if (hook_call.return_count() > 0)
        {
            const sol::object verdict = hook_call.get<sol::object>(0);
            if (verdict.get_type() == sol::type::boolean && !verdict.as<bool>())
            {
                result.ok = false;
            }
        }
        if (hook_call.return_count() > 1)
        {
            const sol::object reason = hook_call.get<sol::object>(1);
            reason.push(state);
            result.message = to_text(state, -1);
            lua_pop(state, 1); // the value pushed above
        }

        return result;
    }

} // namespace slopkit::script
