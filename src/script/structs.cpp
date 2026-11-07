#include "script/structs.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
}

#include "script/structs_view.hpp"

namespace slopkit::script
{
    namespace
    {
        constexpr const char* kClassMeta    = "slopkit.structs.class";
        constexpr const char* kInstanceMeta = "slopkit.structs.instance";
        constexpr const char* kArrayMeta    = "slopkit.structs.array";

        const char kContextKey = 0;

        StructsContext* context_of(lua_State* state)
        {
            lua_rawgetp(state, LUA_REGISTRYINDEX, &kContextKey);
            auto* context = static_cast<StructsContext*>(lua_touserdata(state, -1));
            lua_pop(state, 1);
            return context;
        }

        // Raises an ordinary Lua error. The message buffer is released before the
        // longjmp, so the one allocation the error needed does not leak.
        int fail(lua_State* state, std::string message)
        {
            lua_pushlstring(state, message.data(), message.size());
            std::string {}.swap(message);
            lua_error(state);
            return 0;
        }

        template<typename T>
        void push_user(lua_State* state, const char* metatable, T value)
        {
            void* block = lua_newuserdatauv(state, sizeof(T), 0);
            new (block) T(std::move(value));
            luaL_getmetatable(state, metatable);
            lua_setmetatable(state, -2);
        }

        template<typename T>
        T* check_user(lua_State* state, int index, const char* metatable)
        {
            return static_cast<T*>(luaL_checkudata(state, index, metatable));
        }

        template<typename T>
        T* test_user(lua_State* state, int index, const char* metatable)
        {
            void* block = luaL_testudata(state, index, metatable);
            return block != nullptr ? static_cast<T*>(block) : nullptr;
        }

        // A class handle: the view looks the class up by name on every access, so
        // renaming or deleting it in the editor shows up on the next access.
        struct ClassRef
        {
            std::string name;
        };

        // The bytes a snapshot read once, and the address they start at.
        struct Snapshot
        {
            std::shared_ptr<const std::vector<std::byte>> bytes;
            std::uint64_t                                 origin {};
        };

        struct InstanceRef
        {
            std::string             name;
            std::uint64_t           address {};
            std::optional<Snapshot> snapshot;
        };

        struct ArrayRef
        {
            std::string             name;  // the class the field belongs to
            std::string             field; // the field the proxy came from
            std::uint64_t           address {};
            std::size_t             count {};
            std::size_t             stride {};
            ElementDef              element;
            std::optional<Snapshot> snapshot;
        };

        // The bytes one view reads and writes through: live target memory, or a
        // snapshot's buffer, which still reads its pointers live when followed.
        class ViewSource : public ByteSource
        {
        public:
            ViewSource(const StructsContext& context, const std::optional<Snapshot>& snapshot)
                : context_(context),
                  buffer_(snapshot ? std::optional<BufferSource> {std::in_place,
                                                                  std::span<const std::byte>(*snapshot->bytes),
                                                                  snapshot->origin,
                                                                  pointer_size(context)}
                                   : std::nullopt)
            {
            }

            [[nodiscard]] std::expected<void, std::string>
            load(std::uint64_t base, std::size_t offset, std::span<std::byte> out) const override
            {
                if (buffer_)
                {
                    return buffer_->load(base, offset, out);
                }
                if (context_.memory == nullptr || !context_.memory->read)
                {
                    return std::unexpected(std::string {"no target is attached"});
                }
                auto bytes = context_.memory->read(base + offset, out.size());
                if (!bytes)
                {
                    return std::unexpected(std::move(bytes.error()));
                }
                if (bytes->size() < out.size())
                {
                    return std::unexpected(std::string {"the target returned too few bytes"});
                }
                if (!out.empty())
                {
                    std::memcpy(out.data(), bytes->data(), out.size());
                }
                return {};
            }

            [[nodiscard]] std::expected<void, std::string>
            store(std::uint64_t base, std::size_t offset, std::span<const std::byte> data) override
            {
                if (buffer_)
                {
                    return buffer_->store(base, offset, data);
                }
                if (context_.memory == nullptr || !context_.memory->write)
                {
                    return std::unexpected(std::string {"no target is attached"});
                }
                return context_.memory->write(base + offset, data);
            }

            [[nodiscard]] std::size_t pointer_size() const noexcept override
            {
                return pointer_size(context_);
            }

        private:
            static std::size_t pointer_size(const StructsContext& context) noexcept
            {
                return context.memory != nullptr && context.memory->pointer_size ? context.memory->pointer_size()
                                                                                 : sizeof(void*);
            }

            const StructsContext&       context_;
            std::optional<BufferSource> buffer_;
        };

        const ClassDef* class_of(const StructsContext& context, std::string_view name)
        {
            return context.classes != nullptr ? context.classes->find(name) : nullptr;
        }

        void push_instance(lua_State* state, const FieldValue& value, const std::optional<Snapshot>& snapshot)
        {
            InstanceRef instance;
            instance.name     = value.target;
            instance.address  = value.address;
            instance.snapshot = snapshot;
            push_user(state, kInstanceMeta, std::move(instance));
        }

        void push_array(lua_State*                     state,
                        const FieldValue&              value,
                        const std::optional<Snapshot>& snapshot,
                        std::string_view               owner,
                        std::string_view               field)
        {
            ArrayRef array;
            array.name     = owner;
            array.field    = field;
            array.address  = value.address;
            array.count    = value.count;
            array.stride   = value.stride;
            array.element  = value.element;
            array.snapshot = snapshot;
            push_user(state, kArrayMeta, std::move(array));
        }

        // Pushes the Lua view of a value the plain layer read. `owner` and
        // `field` only feed the labels of the instances and proxies it builds.
        void push_value(lua_State*                     state,
                        const FieldValue&              value,
                        const std::optional<Snapshot>& snapshot,
                        std::string_view               owner,
                        std::string_view               field)
        {
            switch (value.kind)
            {
            case FieldValue::Kind::number:
                if (value.integral && value.number >= -9223372036854775808.0 && value.number < 9223372036854775808.0)
                {
                    lua_pushinteger(state, static_cast<lua_Integer>(value.number));
                    return;
                }
                lua_pushnumber(state, value.number);
                return;
            case FieldValue::Kind::boolean:
                lua_pushboolean(state, value.boolean ? 1 : 0);
                return;
            case FieldValue::Kind::text:
                lua_pushlstring(state, value.text.data(), value.text.size());
                return;
            case FieldValue::Kind::bytes:
                lua_pushlstring(state, reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size());
                return;
            case FieldValue::Kind::address:
                lua_pushinteger(state, static_cast<lua_Integer>(value.address));
                return;
            case FieldValue::Kind::nothing:
                lua_pushnil(state);
                return;
            case FieldValue::Kind::instance:
                // A pointer followed out of a snapshot reaches live memory; only
                // a class inlined where the snapshot read stays in its buffer.
                push_instance(state, value, value.in_place ? snapshot : std::nullopt);
                return;
            case FieldValue::Kind::array:
                push_array(state, value, value.in_place ? snapshot : std::nullopt, owner, field);
                return;
            }
        }

        // Reads `field` of `instance` and pushes its Lua view. A `pairs` walk is
        // lenient: a field the view cannot read, such as a union, reports nil
        // rather than ending the loop.
        int push_field(lua_State*            state,
                       const StructsContext& context,
                       const InstanceRef&    instance,
                       std::string_view      field_name,
                       bool                  lenient = false)
        {
            const ClassDef* type = class_of(context, instance.name);
            if (type == nullptr)
            {
                return fail(state, std::format("{}: no class named '{}'", instance.name, instance.name));
            }
            const FieldDef* field = type->find(field_name);
            if (field == nullptr)
            {
                return fail(state, std::format("{}.{}: no such field", instance.name, field_name));
            }

            const ViewSource                             source(context, instance.snapshot);
            const std::expected<FieldValue, std::string> value = read_field(*field, instance.address, source);
            if (!value)
            {
                if (lenient)
                {
                    lua_pushnil(state);
                    return 1;
                }
                return fail(state, std::format("{}.{}: {}", instance.name, field_name, value.error()));
            }
            push_value(state, *value, instance.snapshot, instance.name, field_name);
            return 1;
        }

        // Reads element `position` of `array` and pushes its Lua view.
        int push_element(lua_State*            state,
                         const StructsContext& context,
                         const ArrayRef&       array,
                         std::size_t           position,
                         bool                  lenient = false)
        {
            const FieldDef      element = element_field(array.element);
            const ViewSource    source(context, array.snapshot);
            const std::uint64_t address = array.address + position * array.stride;

            const std::expected<FieldValue, std::string> value = read_field(element, address, source);
            if (!value)
            {
                if (lenient)
                {
                    lua_pushnil(state);
                    return 1;
                }
                return fail(state, std::format("{}.{}[{}]: {}", array.name, array.field, position + 1, value.error()));
            }
            push_value(state, *value, array.snapshot, array.name, array.field);
            return 1;
        }

        // The Lua value at `index` as the plain layer writes it to `field`.
        bool to_field_value(lua_State* state, int index, const FieldDef& field, FieldValue& value)
        {
            switch (lua_type(state, index))
            {
            case LUA_TNUMBER:
                value.kind = FieldValue::Kind::number;
                if (lua_isinteger(state, index) != 0)
                {
                    value.number   = static_cast<double>(lua_tointeger(state, index));
                    value.integral = true;
                }
                else
                {
                    value.number = lua_tonumber(state, index);
                }
                return true;
            case LUA_TBOOLEAN:
                value.kind    = FieldValue::Kind::boolean;
                value.boolean = lua_toboolean(state, index) != 0;
                return true;
            case LUA_TSTRING:
            {
                std::size_t length = 0;
                const char* text   = lua_tolstring(state, index, &length);
                if (field.kind == FieldKind::bytes)
                {
                    value.kind        = FieldValue::Kind::bytes;
                    const auto* first = reinterpret_cast<const std::byte*>(text);
                    value.bytes.assign(first, first + length);
                    return true;
                }
                value.kind = FieldValue::Kind::text;
                value.text.assign(text, length);
                return true;
            }
            case LUA_TUSERDATA:
            {
                const auto* other = test_user<InstanceRef>(state, index, kInstanceMeta);
                if (other == nullptr)
                {
                    return false;
                }
                value.kind    = FieldValue::Kind::address;
                value.address = other->address;
                return true;
            }
            case LUA_TNIL:
                if (field.kind == FieldKind::pointer || field.kind == FieldKind::class_pointer)
                {
                    value.kind    = FieldValue::Kind::address;
                    value.address = 0;
                    return true;
                }
                return false;
            default:
                return false;
            }
        }

        int write_named_field(lua_State*            state,
                              const StructsContext& context,
                              const InstanceRef&    instance,
                              std::string_view      field_name,
                              int                   value_index)
        {
            const ClassDef* type = class_of(context, instance.name);
            if (type == nullptr)
            {
                return fail(state, std::format("{}: no class named '{}'", instance.name, instance.name));
            }
            const FieldDef* field = type->find(field_name);
            if (field == nullptr)
            {
                return fail(state, std::format("{}.{}: no such field", instance.name, field_name));
            }

            FieldValue value;
            if (!to_field_value(state, value_index, *field, value))
            {
                return fail(state,
                            std::format("{}.{}: the value cannot be written to this field", instance.name, field_name));
            }

            ViewSource                             source(context, instance.snapshot);
            const std::expected<void, std::string> written = write_field(*field, instance.address, value, source);
            if (!written)
            {
                return fail(state, std::format("{}.{}: {}", instance.name, field_name, written.error()));
            }
            return 0;
        }

        int instance_gc(lua_State* state)
        {
            check_user<InstanceRef>(state, 1, kInstanceMeta)->~InstanceRef();
            return 0;
        }

        int class_gc(lua_State* state)
        {
            check_user<ClassRef>(state, 1, kClassMeta)->~ClassRef();
            return 0;
        }

        int array_gc(lua_State* state)
        {
            check_user<ArrayRef>(state, 1, kArrayMeta)->~ArrayRef();
            return 0;
        }

        // ---- instance helpers ------------------------------------------------

        int helper_base(lua_State* state)
        {
            const InstanceRef& instance = *check_user<InstanceRef>(state, 1, kInstanceMeta);
            lua_pushinteger(state, static_cast<lua_Integer>(instance.address));
            return 1;
        }

        int helper_address_of(lua_State* state)
        {
            const StructsContext& context  = *context_of(state);
            const InstanceRef&    instance = *check_user<InstanceRef>(state, 1, kInstanceMeta);
            if (lua_type(state, 2) != LUA_TSTRING)
            {
                return fail(state, std::format("{}:address_of: the field name must be a string", instance.name));
            }
            const std::string_view field_name {lua_tolstring(state, 2, nullptr)};

            const ClassDef* type = class_of(context, instance.name);
            if (type == nullptr)
            {
                return fail(state, std::format("{}: no class named '{}'", instance.name, instance.name));
            }
            const FieldDef* field = type->find(field_name);
            if (field == nullptr)
            {
                return fail(state, std::format("{}.{}: no such field", instance.name, field_name));
            }
            lua_pushinteger(state, static_cast<lua_Integer>(instance.address + field->offset));
            return 1;
        }

        int helper_field(lua_State* state)
        {
            const StructsContext& context  = *context_of(state);
            const InstanceRef&    instance = *check_user<InstanceRef>(state, 1, kInstanceMeta);
            if (lua_type(state, 2) != LUA_TSTRING)
            {
                return fail(state, std::format("{}:field: the field name must be a string", instance.name));
            }
            const std::string_view field_name {lua_tolstring(state, 2, nullptr)};
            if (lua_gettop(state) >= 3)
            {
                return write_named_field(state, context, instance, field_name, 3);
            }
            return push_field(state, context, instance, field_name);
        }

        // The field without following a pointer: the address a pointer, an
        // inlined class or an array sits at rather than what it points to.
        int helper_raw(lua_State* state)
        {
            const StructsContext& context  = *context_of(state);
            const InstanceRef&    instance = *check_user<InstanceRef>(state, 1, kInstanceMeta);
            if (lua_type(state, 2) != LUA_TSTRING)
            {
                return fail(state, std::format("{}:raw: the field name must be a string", instance.name));
            }
            const std::string_view field_name {lua_tolstring(state, 2, nullptr)};

            const ClassDef* type = class_of(context, instance.name);
            if (type == nullptr)
            {
                return fail(state, std::format("{}: no class named '{}'", instance.name, instance.name));
            }
            const FieldDef* field = type->find(field_name);
            if (field == nullptr)
            {
                return fail(state, std::format("{}.{}: no such field", instance.name, field_name));
            }

            const ViewSource                             source(context, instance.snapshot);
            const std::expected<FieldValue, std::string> value = read_field(*field, instance.address, source);
            if (!value)
            {
                return fail(state, std::format("{}.{}: {}", instance.name, field_name, value.error()));
            }
            switch (value->kind)
            {
            case FieldValue::Kind::instance:
            case FieldValue::Kind::array:
                lua_pushinteger(state, static_cast<lua_Integer>(value->address));
                return 1;
            case FieldValue::Kind::nothing:
                lua_pushinteger(state, 0);
                return 1;
            default:
                break;
            }
            push_value(state, *value, instance.snapshot, instance.name, field_name);
            return 1;
        }

        int helper_snapshot(lua_State* state)
        {
            const StructsContext& context  = *context_of(state);
            const InstanceRef&    instance = *check_user<InstanceRef>(state, 1, kInstanceMeta);
            if (instance.snapshot)
            {
                lua_pushvalue(state, 1);
                return 1;
            }

            const ClassDef* type = class_of(context, instance.name);
            if (type == nullptr)
            {
                return fail(state, std::format("{}: no class named '{}'", instance.name, instance.name));
            }

            const ViewSource                                         live(context, std::nullopt);
            const std::expected<std::vector<std::byte>, std::string> bytes = load_extent(*type, instance.address, live);
            if (!bytes)
            {
                return fail(state, std::format("{}:snapshot: {}", instance.name, bytes.error()));
            }

            InstanceRef snapshot;
            snapshot.name     = instance.name;
            snapshot.address  = instance.address;
            snapshot.snapshot = Snapshot {std::make_shared<const std::vector<std::byte>>(*bytes), instance.address};
            push_user(state, kInstanceMeta, std::move(snapshot));
            return 1;
        }

        // ---- instance internals ---------------------------------------------

        // The helpers win over a field of the same name, so a field called
        // `base` or `raw` stays reachable through `inst:field("base")`.
        lua_CFunction helper_for(std::string_view key) noexcept
        {
            static const std::pair<std::string_view, lua_CFunction> kHelpers[] = {
                {"address_of", &helper_address_of},
                {      "base",       &helper_base},
                {     "field",      &helper_field},
                {       "raw",        &helper_raw},
                {  "snapshot",   &helper_snapshot},
            };
            for (const auto& [name, function] : kHelpers)
            {
                if (name == key)
                {
                    return function;
                }
            }
            return nullptr;
        }

        int instance_index(lua_State* state)
        {
            const StructsContext& context  = *context_of(state);
            const InstanceRef&    instance = *check_user<InstanceRef>(state, 1, kInstanceMeta);

            if (lua_type(state, 2) != LUA_TSTRING)
            {
                lua_pushnil(state);
                return 1;
            }
            std::size_t            length = 0;
            const char*            text   = lua_tolstring(state, 2, &length);
            const std::string_view key {text, length};

            if (const lua_CFunction helper = helper_for(key))
            {
                lua_pushcfunction(state, helper);
                return 1;
            }
            return push_field(state, context, instance, key);
        }

        int instance_new_index(lua_State* state)
        {
            const StructsContext& context  = *context_of(state);
            const InstanceRef&    instance = *check_user<InstanceRef>(state, 1, kInstanceMeta);

            if (lua_type(state, 2) != LUA_TSTRING)
            {
                return fail(state, std::format("{}: the field name must be a string", instance.name));
            }
            std::size_t            length = 0;
            const char*            text   = lua_tolstring(state, 2, &length);
            const std::string_view key {text, length};

            if (helper_for(key) != nullptr)
            {
                return fail(state,
                            std::format("{}.{}: '{}' is a helper; write the field with field('{}', value)",
                                        instance.name,
                                        key,
                                        key,
                                        key));
            }
            return write_named_field(state, context, instance, key, 3);
        }

        int instance_tostring(lua_State* state)
        {
            const InstanceRef& instance = *check_user<InstanceRef>(state, 1, kInstanceMeta);
            const std::string  text     = std::format("{}@0x{:X}", instance.name, instance.address);
            lua_pushlstring(state, text.data(), text.size());
            return 1;
        }

        int instance_pairs_next(lua_State* state)
        {
            const StructsContext& context  = *context_of(state);
            const InstanceRef&    instance = *check_user<InstanceRef>(state, 1, kInstanceMeta);

            const ClassDef* type = class_of(context, instance.name);
            if (type == nullptr)
            {
                return fail(state, std::format("{}: no class named '{}'", instance.name, instance.name));
            }

            std::size_t position = 0;
            if (lua_type(state, 2) == LUA_TSTRING)
            {
                const std::string_view cursor {lua_tolstring(state, 2, nullptr)};
                for (; position < type->fields.size(); ++position)
                {
                    if (type->fields[position].name == cursor)
                    {
                        ++position;
                        break;
                    }
                }
            }
            if (position >= type->fields.size())
            {
                return 0;
            }

            const FieldDef& field = type->fields[position];
            lua_pushlstring(state, field.name.data(), field.name.size());
            return 1 + push_field(state, context, instance, field.name, true);
        }

        int instance_pairs(lua_State* state)
        {
            lua_pushcfunction(state, &instance_pairs_next);
            lua_pushvalue(state, 1);
            lua_pushnil(state);
            return 3;
        }

        // ---- class handle ----------------------------------------------------

        int class_at(lua_State* state)
        {
            const StructsContext& context = *context_of(state);
            const ClassRef&       handle  = *check_user<ClassRef>(state, 1, kClassMeta);
            if (class_of(context, handle.name) == nullptr)
            {
                return fail(state, std::format("structs.get: no class named '{}'", handle.name));
            }

            bool          whole = false;
            std::uint64_t address {};
            if (lua_type(state, 2) == LUA_TNUMBER)
            {
                if (lua_isinteger(state, 2) != 0)
                {
                    const lua_Integer value = lua_tointeger(state, 2);
                    whole                   = value >= 0;
                    address                 = static_cast<std::uint64_t>(value);
                }
                else
                {
                    const double value = lua_tonumber(state, 2);
                    const double limit = 18446744073709551615.0;
                    if (value >= 0.0 && value <= limit
                        && value == static_cast<double>(static_cast<std::uint64_t>(value)))
                    {
                        whole   = true;
                        address = static_cast<std::uint64_t>(value);
                    }
                }
            }
            if (!whole)
            {
                return fail(state, std::format("{}:at: the address must be a whole non-negative number", handle.name));
            }

            InstanceRef instance;
            instance.name    = handle.name;
            instance.address = address;
            push_user(state, kInstanceMeta, std::move(instance));
            return 1;
        }

        int class_fields(lua_State* state)
        {
            const StructsContext& context = *context_of(state);
            const ClassRef&       handle  = *check_user<ClassRef>(state, 1, kClassMeta);
            const ClassDef*       type    = class_of(context, handle.name);
            if (type == nullptr)
            {
                return fail(state, std::format("{}:fields: no class named '{}'", handle.name, handle.name));
            }

            lua_createtable(state, static_cast<int>(type->fields.size()), 0);
            int position = 1;
            for (const FieldDef& field : type->fields)
            {
                lua_createtable(state, 0, 4);
                lua_pushlstring(state, field.name.data(), field.name.size());
                lua_setfield(state, -2, "name");
                lua_pushinteger(state, static_cast<lua_Integer>(field.offset));
                lua_setfield(state, -2, "offset");
                lua_pushlstring(state, field.type_text.data(), field.type_text.size());
                lua_setfield(state, -2, "type");
                lua_pushinteger(state, static_cast<lua_Integer>(field.size));
                lua_setfield(state, -2, "size");
                lua_seti(state, -2, position++);
            }
            return 1;
        }

        int class_tostring(lua_State* state)
        {
            const ClassRef& handle = *check_user<ClassRef>(state, 1, kClassMeta);
            lua_pushlstring(state, handle.name.data(), handle.name.size());
            return 1;
        }

        // ---- array proxy -----------------------------------------------------

        int array_index(lua_State* state)
        {
            const StructsContext& context = *context_of(state);
            const ArrayRef&       array   = *check_user<ArrayRef>(state, 1, kArrayMeta);

            if (lua_type(state, 2) != LUA_TNUMBER || lua_isinteger(state, 2) == 0)
            {
                return fail(state, std::format("{}.{}: the index must be a whole number", array.name, array.field));
            }
            const lua_Integer index = lua_tointeger(state, 2);
            if (index < 1 || static_cast<std::size_t>(index) > array.count)
            {
                return fail(state,
                            std::format("{}.{}[{}]: the index is out of range (the array holds {} element(s))",
                                        array.name,
                                        array.field,
                                        index,
                                        array.count));
            }
            return push_element(state, context, array, static_cast<std::size_t>(index) - 1);
        }

        int array_new_index(lua_State* state)
        {
            const StructsContext& context = *context_of(state);
            const ArrayRef&       array   = *check_user<ArrayRef>(state, 1, kArrayMeta);

            if (lua_type(state, 2) != LUA_TNUMBER || lua_isinteger(state, 2) == 0)
            {
                return fail(state, std::format("{}.{}: the index must be a whole number", array.name, array.field));
            }
            const lua_Integer index = lua_tointeger(state, 2);
            if (index < 1 || static_cast<std::size_t>(index) > array.count)
            {
                return fail(state,
                            std::format("{}.{}[{}]: the index is out of range (the array holds {} element(s))",
                                        array.name,
                                        array.field,
                                        index,
                                        array.count));
            }

            const FieldDef      element = element_field(array.element);
            const std::uint64_t address = array.address + (static_cast<std::size_t>(index) - 1) * array.stride;

            FieldValue value;
            if (!to_field_value(state, 3, element, value))
            {
                return fail(
                    state,
                    std::format(
                        "{}.{}[{}]: the value cannot be written to this field", array.name, array.field, index));
            }

            ViewSource                             source(context, array.snapshot);
            const std::expected<void, std::string> written = write_field(element, address, value, source);
            if (!written)
            {
                return fail(state, std::format("{}.{}[{}]: {}", array.name, array.field, index, written.error()));
            }
            return 0;
        }

        int array_len(lua_State* state)
        {
            const ArrayRef& array = *check_user<ArrayRef>(state, 1, kArrayMeta);
            lua_pushinteger(state, static_cast<lua_Integer>(array.count));
            return 1;
        }

        int array_tostring(lua_State* state)
        {
            const ArrayRef&   array = *check_user<ArrayRef>(state, 1, kArrayMeta);
            const std::string text =
                std::format("{}.{}[{}]@0x{:X}", array.name, array.field, array.count, array.address);
            lua_pushlstring(state, text.data(), text.size());
            return 1;
        }

        int array_pairs_next(lua_State* state)
        {
            const StructsContext& context = *context_of(state);
            const ArrayRef&       array   = *check_user<ArrayRef>(state, 1, kArrayMeta);

            lua_Integer position = lua_type(state, 2) == LUA_TNUMBER ? lua_tointeger(state, 2) : 0;
            position += 1;
            if (position < 1 || static_cast<std::size_t>(position) > array.count)
            {
                return 0;
            }
            lua_pushinteger(state, position);
            return 1 + push_element(state, context, array, static_cast<std::size_t>(position) - 1, true);
        }

        int array_pairs(lua_State* state)
        {
            lua_pushcfunction(state, &array_pairs_next);
            lua_pushvalue(state, 1);
            lua_pushnil(state);
            return 3;
        }

        // ---- the global ------------------------------------------------------

        int structs_get(lua_State* state)
        {
            const StructsContext& context = *context_of(state);
            if (lua_type(state, 1) != LUA_TSTRING)
            {
                return fail(state, std::string {"structs.get: the class name must be a string"});
            }
            std::size_t            length = 0;
            const char*            text   = lua_tolstring(state, 1, &length);
            const std::string_view name {text, length};

            if (class_of(context, name) == nullptr)
            {
                return fail(state, std::format("structs.get: no class named '{}'", name));
            }

            ClassRef handle;
            handle.name = name;
            push_user(state, kClassMeta, std::move(handle));
            return 1;
        }

        int structs_list(lua_State* state)
        {
            const StructsContext& context = *context_of(state);
            const std::size_t     count   = context.classes != nullptr ? context.classes->classes.size() : 0;

            lua_createtable(state, static_cast<int>(count), 0);
            int position = 1;
            if (context.classes != nullptr)
            {
                for (const ClassDef& entry : context.classes->classes)
                {
                    lua_pushlstring(state, entry.name.data(), entry.name.size());
                    lua_seti(state, -2, position++);
                }
            }
            return 1;
        }
    }

    void install_structs(lua_State* state, StructsContext& context)
    {
        lua_pushlightuserdata(state, &context);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &kContextKey);

        luaL_newmetatable(state, kInstanceMeta);
        lua_pushcfunction(state, &instance_gc);
        lua_setfield(state, -2, "__gc");
        lua_pushcfunction(state, &instance_index);
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, &instance_new_index);
        lua_setfield(state, -2, "__newindex");
        lua_pushcfunction(state, &instance_tostring);
        lua_setfield(state, -2, "__tostring");
        lua_pushcfunction(state, &instance_pairs);
        lua_setfield(state, -2, "__pairs");
        lua_pushstring(state, "structs instance");
        lua_setfield(state, -2, "__name");
        lua_pop(state, 1);

        luaL_newmetatable(state, kArrayMeta);
        lua_pushcfunction(state, &array_gc);
        lua_setfield(state, -2, "__gc");
        lua_pushcfunction(state, &array_index);
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, &array_new_index);
        lua_setfield(state, -2, "__newindex");
        lua_pushcfunction(state, &array_len);
        lua_setfield(state, -2, "__len");
        lua_pushcfunction(state, &array_tostring);
        lua_setfield(state, -2, "__tostring");
        lua_pushcfunction(state, &array_pairs);
        lua_setfield(state, -2, "__pairs");
        lua_pushstring(state, "structs array");
        lua_setfield(state, -2, "__name");
        lua_pop(state, 1);

        luaL_newmetatable(state, kClassMeta);
        lua_pushcfunction(state, &class_gc);
        lua_setfield(state, -2, "__gc");
        lua_pushcfunction(state, &class_tostring);
        lua_setfield(state, -2, "__tostring");
        lua_pushstring(state, "structs class");
        lua_setfield(state, -2, "__name");
        lua_createtable(state, 0, 2);
        lua_pushcfunction(state, &class_at);
        lua_setfield(state, -2, "at");
        lua_pushcfunction(state, &class_fields);
        lua_setfield(state, -2, "fields");
        lua_setfield(state, -2, "__index");
        lua_pop(state, 1);

        lua_createtable(state, 0, 2);
        lua_pushcfunction(state, &structs_get);
        lua_setfield(state, -2, "get");
        lua_pushcfunction(state, &structs_list);
        lua_setfield(state, -2, "list");
        lua_setglobal(state, "structs");
    }
}
