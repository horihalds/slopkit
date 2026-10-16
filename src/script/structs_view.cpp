#include "script/structs_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "reclass/node.hpp"
#include "reclass/nodes/node.hpp"
#include "reclass/project.hpp"
#include "reclass/value.hpp"
#include "script/codec.hpp"

namespace slopkit::script
{
    namespace
    {
        constexpr std::uint32_t kReplacement = 0xFFFD;

        struct KindShape
        {
            FieldKind   kind {FieldKind::unsupported};
            NumberToken number {NumberToken::none};
        };

        KindShape shape_of(reclass::NodeKind kind) noexcept
        {
            switch (kind)
            {
            case reclass::NodeKind::int8:
                return {FieldKind::number, NumberToken::i8};
            case reclass::NodeKind::int16:
                return {FieldKind::number, NumberToken::i16};
            case reclass::NodeKind::int32:
                return {FieldKind::number, NumberToken::i32};
            case reclass::NodeKind::int64:
                return {FieldKind::number, NumberToken::i64};
            case reclass::NodeKind::uint8:
                return {FieldKind::number, NumberToken::u8};
            case reclass::NodeKind::uint16:
                return {FieldKind::number, NumberToken::u16};
            case reclass::NodeKind::uint32:
                return {FieldKind::number, NumberToken::u32};
            case reclass::NodeKind::uint64:
                return {FieldKind::number, NumberToken::u64};
            case reclass::NodeKind::float32:
                return {FieldKind::number, NumberToken::f32};
            case reclass::NodeKind::float64:
                return {FieldKind::number, NumberToken::f64};
            case reclass::NodeKind::boolean:
                return {FieldKind::boolean, NumberToken::none};
            case reclass::NodeKind::character:
                return {FieldKind::text_utf8, NumberToken::none};
            case reclass::NodeKind::hex8:
                return {FieldKind::number, NumberToken::u8};
            case reclass::NodeKind::hex16:
                return {FieldKind::number, NumberToken::u16};
            case reclass::NodeKind::hex32:
                return {FieldKind::number, NumberToken::u32};
            case reclass::NodeKind::hex64:
                return {FieldKind::number, NumberToken::u64};
            case reclass::NodeKind::pointer:
            case reclass::NodeKind::utf8_text_ptr:
            case reclass::NodeKind::utf16_text_ptr:
            case reclass::NodeKind::utf32_text_ptr:
            case reclass::NodeKind::function_ptr:
            case reclass::NodeKind::virtual_method_table:
            case reclass::NodeKind::virtual_method:
                return {FieldKind::pointer, NumberToken::none};
            case reclass::NodeKind::class_pointer:
                return {FieldKind::class_pointer, NumberToken::none};
            case reclass::NodeKind::class_instance:
                return {FieldKind::class_value, NumberToken::none};
            case reclass::NodeKind::array:
            case reclass::NodeKind::class_instance_array:
            case reclass::NodeKind::class_wrapper_array:
                return {FieldKind::array, NumberToken::none};
            case reclass::NodeKind::nint:
                return {FieldKind::number, NumberToken::nint};
            case reclass::NodeKind::nuint:
                return {FieldKind::number, NumberToken::nuint};
            case reclass::NodeKind::utf8_text:
                return {FieldKind::text_utf8, NumberToken::none};
            case reclass::NodeKind::utf16_text:
                return {FieldKind::text_utf16, NumberToken::none};
            case reclass::NodeKind::vector2:
            case reclass::NodeKind::vector3:
            case reclass::NodeKind::vector4:
            case reclass::NodeKind::matrix3x3:
            case reclass::NodeKind::matrix3x4:
            case reclass::NodeKind::matrix4x4:
            case reclass::NodeKind::function:
                return {FieldKind::bytes, NumberToken::none};
            case reclass::NodeKind::union_value:
            case reclass::NodeKind::bitfield:
            case reclass::NodeKind::enum_node:
            case reclass::NodeKind::utf32_text:
                return {FieldKind::unsupported, NumberToken::none};
            }
            return {};
        }

        // A signed kind the user reads as hex holds a bit pattern, so the script
        // gets the unsigned number of the same width.
        void widen_hex(FieldDef& field, const reclass::Node& node)
        {
            if (!node.hex || !number_token_is_integer(field.number))
            {
                return;
            }
            switch (field.number)
            {
            case NumberToken::i8:
                field.number = NumberToken::u8;
                break;
            case NumberToken::i16:
                field.number = NumberToken::u16;
                break;
            case NumberToken::i32:
                field.number = NumberToken::u32;
                break;
            case NumberToken::i64:
                field.number = NumberToken::u64;
                break;
            default:
                break;
            }
        }

        std::size_t class_extent(const reclass::Project& project, std::string_view class_name)
        {
            const reclass::Class* type = project.find_class(class_name);
            if (type == nullptr)
            {
                return 0;
            }
            return reclass::layout_class(project, *type).value_or(reclass::Layout {}).size;
        }

        std::size_t element_count(const reclass::Node& node)
        {
            if (node.kind() == reclass::NodeKind::array)
            {
                return node.children().size();
            }
            return node.repeat();
        }

        std::string number_label(NumberToken number, FieldKind kind)
        {
            switch (number)
            {
            case NumberToken::nint:
                return "nint";
            case NumberToken::nuint:
                return "nuint";
            default:
                break;
            }
            if (kind == FieldKind::pointer)
            {
                return "ptr";
            }
            return std::string {number_token(number, sizeof(void*))};
        }

        std::string field_type_text(const reclass::Project& project, const reclass::Node& node)
        {
            const KindShape shape = shape_of(node.kind());
            switch (shape.kind)
            {
            case FieldKind::number:
                return number_label(shape.number, shape.kind);
            case FieldKind::pointer:
                return "ptr";
            case FieldKind::class_pointer:
                return node.class_name.empty() ? std::string {"ptr"} : std::format("ptr -> {}", node.class_name);
            case FieldKind::class_value:
                return node.class_name.empty() ? std::string {"?"} : node.class_name;
            case FieldKind::array:
            {
                const std::span<const reclass::NodePtr> children = node.children();
                std::string                             element;
                if (!children.empty())
                {
                    element = field_type_text(project, *children.front());
                }
                else if (!node.class_name.empty())
                {
                    element = node.class_name;
                }
                else
                {
                    element = std::string {reclass::describe(node.kind())};
                }
                return std::format("{} [{}]", element, element_count(node));
            }
            case FieldKind::boolean:
                return "bool";
            case FieldKind::text_utf8:
                return std::format("utf8 [{}]", node.extent().value_or(node.value_size()));
            case FieldKind::text_utf16:
                return std::format("utf16 [{}]", node.extent().value_or(node.value_size() / 2));
            case FieldKind::bytes:
                return std::format("bytes [{}]", node.value_size());
            case FieldKind::unsupported:
                return std::string {reclass::describe(node.kind())};
            }
            return {};
        }

        FieldDef field_from(const reclass::Project& project, const reclass::Node& node)
        {
            const KindShape shape = shape_of(node.kind());

            FieldDef field;
            field.name      = node.name;
            field.kind      = shape.kind;
            field.number    = shape.number;
            field.size      = reclass::node_size(project, node);
            field.type_text = field_type_text(project, node);

            switch (shape.kind)
            {
            case FieldKind::class_pointer:
            case FieldKind::class_value:
                field.target = node.class_name;
                if (const reclass::Class* target = project.find_class(node.class_name))
                {
                    field.target_id = target->id;
                }
                break;
            case FieldKind::array:
            {
                field.count           = element_count(node);
                field.through_pointer = node.kind() == reclass::NodeKind::class_instance_array;

                if (node.kind() == reclass::NodeKind::array)
                {
                    const std::span<const reclass::NodePtr> children = node.children();
                    if (children.empty())
                    {
                        field.element.type_text = std::string {reclass::describe(node.kind())};
                        break;
                    }
                    const reclass::Node& element       = *children.front();
                    const KindShape      element_shape = shape_of(element.kind());
                    field.element.kind                 = element_shape.kind;
                    field.element.number               = element_shape.number;
                    field.element.size                 = reclass::node_size(project, element);
                    field.element.count =
                        element.extent().value_or(element_shape.kind == FieldKind::text_utf8 ? field.element.size : 0);
                    field.element.type_text = field_type_text(project, element);
                    field.element.target    = element.class_name;
                    if (const reclass::Class* target = project.find_class(element.class_name))
                    {
                        field.element.target_id = target->id;
                    }
                }
                else
                {
                    field.element.kind      = FieldKind::class_value;
                    field.element.type_text = node.class_name.empty() ? std::string {"?"} : node.class_name;
                    field.element.target    = node.class_name;
                    field.element.size      = class_extent(project, node.class_name);
                    if (const reclass::Class* target = project.find_class(node.class_name))
                    {
                        field.element.target_id = target->id;
                    }
                }

                field.stride = field.element.size;
                break;
            }
            case FieldKind::text_utf8:
            case FieldKind::text_utf16:
                field.count = node.extent().value_or(node.value_size());
                break;
            case FieldKind::bytes:
                field.count = field.size;
                break;
            default:
                break;
            }

            widen_hex(field, node);
            return field;
        }

        std::vector<std::uint16_t> utf16_from_utf8(std::string_view text)
        {
            std::vector<std::uint16_t> units;
            std::size_t                index = 0;
            while (index < text.size())
            {
                const auto    first  = static_cast<unsigned char>(text[index]);
                std::uint32_t code   = 0;
                std::size_t   length = 0;
                if (first < 0x80)
                {
                    code   = first;
                    length = 1;
                }
                else if ((first & 0xE0U) == 0xC0U)
                {
                    code   = first & 0x1FU;
                    length = 2;
                }
                else if ((first & 0xF0U) == 0xE0U)
                {
                    code   = first & 0x0FU;
                    length = 3;
                }
                else if ((first & 0xF8U) == 0xF0U)
                {
                    code   = first & 0x07U;
                    length = 4;
                }
                else
                {
                    units.push_back(static_cast<std::uint16_t>(kReplacement));
                    ++index;
                    continue;
                }

                if (index + length > text.size())
                {
                    units.push_back(static_cast<std::uint16_t>(kReplacement));
                    break;
                }

                bool valid = true;
                for (std::size_t continuation = 1; continuation < length; ++continuation)
                {
                    const auto next = static_cast<unsigned char>(text[index + continuation]);
                    if ((next & 0xC0U) != 0x80U)
                    {
                        valid = false;
                        break;
                    }
                    code = (code << 6) | (next & 0x3FU);
                }
                index += length;

                if (!valid || code > 0x10FFFFU)
                {
                    units.push_back(static_cast<std::uint16_t>(kReplacement));
                    continue;
                }
                if (code < 0x10000U)
                {
                    units.push_back(static_cast<std::uint16_t>(code));
                    continue;
                }
                code -= 0x10000U;
                units.push_back(static_cast<std::uint16_t>(0xD800U + (code >> 10)));
                units.push_back(static_cast<std::uint16_t>(0xDC00U + (code & 0x3FFU)));
            }
            return units;
        }

        void append_utf8(std::string& text, std::uint32_t code)
        {
            if (code < 0x80U)
            {
                text.push_back(static_cast<char>(code));
                return;
            }
            if (code < 0x800U)
            {
                text.push_back(static_cast<char>(0xC0U | (code >> 6)));
                text.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
                return;
            }
            if (code < 0x10000U)
            {
                text.push_back(static_cast<char>(0xE0U | (code >> 12)));
                text.push_back(static_cast<char>(0x80U | ((code >> 6) & 0x3FU)));
                text.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
                return;
            }
            text.push_back(static_cast<char>(0xF0U | (code >> 18)));
            text.push_back(static_cast<char>(0x80U | ((code >> 12) & 0x3FU)));
            text.push_back(static_cast<char>(0x80U | ((code >> 6) & 0x3FU)));
            text.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }

        std::string utf8_from_utf16(std::span<const std::uint16_t> units)
        {
            std::string text;
            for (std::size_t index = 0; index < units.size(); ++index)
            {
                std::uint32_t code = units[index];
                if (code >= 0xD800U && code <= 0xDBFFU && index + 1 < units.size() && units[index + 1] >= 0xDC00U
                    && units[index + 1] <= 0xDFFFU)
                {
                    code = 0x10000U + ((code - 0xD800U) << 10) + (units[index + 1] - 0xDC00U);
                    ++index;
                }
                else if (code >= 0xD800U && code <= 0xDFFFU)
                {
                    code = kReplacement;
                }
                append_utf8(text, code);
            }
            return text;
        }

        std::expected<std::vector<std::byte>, std::string>
        load_bytes(std::uint64_t base, std::size_t offset, std::size_t size, const ByteSource& source)
        {
            std::vector<std::byte> bytes(size);
            if (size != 0)
            {
                const std::expected<void, std::string> loaded = source.load(base, offset, bytes);
                if (!loaded)
                {
                    return std::unexpected(loaded.error());
                }
            }
            return bytes;
        }

        // The address a pointer field holds, as wide as the target's pointers.
        std::expected<std::uint64_t, std::string>
        read_address(const FieldDef& field, std::uint64_t base, const ByteSource& source)
        {
            const std::size_t width = source.pointer_size() == 0 ? sizeof(void*) : source.pointer_size();
            const auto        raw   = load_bytes(base, field.offset, width, source);
            if (!raw)
            {
                return std::unexpected(raw.error());
            }
            const auto decoded = decode_number(number_token(NumberToken::nuint, width), *raw);
            if (!decoded)
            {
                return std::unexpected(decoded.error());
            }
            return static_cast<std::uint64_t>(*decoded);
        }

        std::expected<std::string, std::string>
        read_text(const FieldDef& field, std::uint64_t base, const ByteSource& source)
        {
            const bool        wide       = field.kind == FieldKind::text_utf16;
            const std::size_t unit       = wide ? 2 : 1;
            const std::size_t characters = std::min(field.size / unit, kMaxViewText);
            const auto        bytes      = load_bytes(base, field.offset, characters * unit, source);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }

            if (!wide)
            {
                const auto end = std::ranges::find(*bytes, std::byte {0});
                return std::string {reinterpret_cast<const char*>(bytes->data()),
                                    static_cast<std::size_t>(std::distance(bytes->begin(), end))};
            }

            std::vector<std::uint16_t> units(characters, 0);
            if (!bytes->empty())
            {
                std::memcpy(units.data(), bytes->data(), bytes->size());
            }
            const auto end = std::ranges::find(units, std::uint16_t {0});
            units.resize(static_cast<std::size_t>(std::distance(units.begin(), end)));
            return utf8_from_utf16(units);
        }

        std::expected<std::vector<std::byte>, std::string> text_bytes(const FieldDef& field, std::string_view text)
        {
            if (field.kind != FieldKind::text_utf16)
            {
                if (text.size() > field.size)
                {
                    return std::unexpected(std::format("the text does not fit in {} character(s)", field.size));
                }
                std::vector<std::byte> bytes(field.size, std::byte {0});
                std::memcpy(bytes.data(), text.data(), text.size());
                return bytes;
            }

            const std::size_t                characters = field.size / 2;
            const std::vector<std::uint16_t> units      = utf16_from_utf8(text);
            if (units.size() > characters)
            {
                return std::unexpected(std::format("the text does not fit in {} character(s)", characters));
            }
            std::vector<std::byte> bytes(field.size, std::byte {0});
            std::memcpy(bytes.data(), units.data(), units.size() * sizeof(std::uint16_t));
            return bytes;
        }

        bool integer_fits(std::string_view token, double value)
        {
            const auto width = type_width(token);
            if (!width)
            {
                return false;
            }
            const int    bits  = static_cast<int>(token.starts_with("i") ? *width * 8 - 1 : *width * 8);
            const double limit = std::ldexp(1.0, bits);
            return token.starts_with("i") ? value >= -limit && value < limit : value >= 0.0 && value < limit;
        }

        bool is_integral(const FieldValue& value)
        {
            if (value.kind == FieldValue::Kind::address || value.integral)
            {
                return true;
            }
            return value.number == std::floor(value.number);
        }
    }

    std::string_view number_token(NumberToken number, std::size_t pointer_size) noexcept
    {
        switch (number)
        {
        case NumberToken::none:
            return {};
        case NumberToken::u8:
            return "u8";
        case NumberToken::i8:
            return "i8";
        case NumberToken::u16:
            return "u16";
        case NumberToken::i16:
            return "i16";
        case NumberToken::u32:
            return "u32";
        case NumberToken::i32:
            return "i32";
        case NumberToken::u64:
            return "u64";
        case NumberToken::i64:
            return "i64";
        case NumberToken::f32:
            return "f32";
        case NumberToken::f64:
            return "f64";
        case NumberToken::nint:
            return pointer_size == 4 ? "i32" : "i64";
        case NumberToken::nuint:
            return pointer_size == 4 ? "u32" : "u64";
        }
        return {};
    }

    bool number_token_is_integer(NumberToken number) noexcept
    {
        const std::string_view token = number_token(number, sizeof(void*));
        return !token.empty() && is_integer_token(token);
    }

    std::size_t number_width(NumberToken number, std::size_t model_size, std::size_t pointer_size) noexcept
    {
        if (number == NumberToken::nint || number == NumberToken::nuint)
        {
            return pointer_size == 0 ? model_size : pointer_size;
        }
        const std::string_view token = number_token(number, pointer_size);
        const auto             width = token.empty() ? std::nullopt : type_width(token);
        return width.value_or(model_size);
    }

    const FieldDef* ClassDef::find(std::string_view field) const noexcept
    {
        const auto found = std::ranges::find_if(fields,
                                                [field](const FieldDef& entry)
                                                {
                                                    return entry.name == field;
                                                });
        return found != fields.end() ? &*found : nullptr;
    }

    const ClassDef* ClassIndex::find(std::string_view name) const noexcept
    {
        const auto found = std::ranges::find_if(classes,
                                                [name](const ClassDef& entry)
                                                {
                                                    return entry.name == name;
                                                });
        return found != classes.end() ? &*found : nullptr;
    }

    ClassIndex build_class_index(const reclass::Project& project)
    {
        ClassIndex index;
        index.classes.reserve(project.classes().size());

        for (const reclass::Class& type : project.classes())
        {
            ClassDef entry;
            entry.id   = type.id;
            entry.name = type.name;

            // The model lays a class's nodes out back to back with no padding,
            // so the read falls back to a running offset where `layout_class`
            // refuses a cyclic class.
            const auto layout = reclass::layout_class(project, type);

            std::size_t offset = 0;
            entry.fields.reserve(type.nodes.size());
            for (std::size_t position = 0; position < type.nodes.size(); ++position)
            {
                FieldDef field = field_from(project, *type.nodes[position]);
                field.offset   = layout ? layout->offsets[position] : offset;
                offset += field.size;
                entry.fields.push_back(std::move(field));
            }

            entry.extent = layout ? layout->size : offset;
            index.classes.push_back(std::move(entry));
        }

        return index;
    }

    std::shared_ptr<const ClassIndex> class_index_of(const reclass::Project& project)
    {
        return std::make_shared<const ClassIndex>(build_class_index(project));
    }

    FieldDef element_field(const ElementDef& element)
    {
        FieldDef field;
        field.kind      = element.kind;
        field.number    = element.number;
        field.type_text = element.type_text;
        field.size      = element.size;
        field.target    = element.target;
        field.target_id = element.target_id;
        field.count     = element.count;
        return field;
    }

    BufferSource::BufferSource(std::span<const std::byte> bytes, std::uint64_t origin, std::size_t pointer_size)
        : bytes_(bytes), origin_(origin), pointer_size_(pointer_size == 0 ? sizeof(void*) : pointer_size)
    {
    }

    std::expected<void, std::string>
    BufferSource::load(std::uint64_t base, std::size_t offset, std::span<std::byte> out) const
    {
        const std::uint64_t address = base + offset;
        if (base < origin_ || address - origin_ > bytes_.size() || out.size() > bytes_.size() - (address - origin_))
        {
            return std::unexpected(std::format("the snapshot does not cover address {:#x}", address));
        }
        if (!out.empty())
        {
            std::memcpy(out.data(), bytes_.data() + (address - origin_), out.size());
        }
        return {};
    }

    std::expected<void, std::string>
    BufferSource::store(std::uint64_t base, std::size_t offset, std::span<const std::byte> data)
    {
        static_cast<void>(base);
        static_cast<void>(offset);
        static_cast<void>(data);
        return std::unexpected(std::string {"a snapshot is read-only"});
    }

    std::size_t BufferSource::pointer_size() const noexcept
    {
        return pointer_size_;
    }

    std::expected<std::vector<std::byte>, std::string>
    load_extent(const ClassDef& type, std::uint64_t base, const ByteSource& source)
    {
        if (type.extent > kMaxViewSnapshot)
        {
            return std::unexpected(std::format(
                "the class covers {} byte(s), past the {} byte snapshot cap", type.extent, kMaxViewSnapshot));
        }
        return load_bytes(base, 0, type.extent, source);
    }

    std::expected<FieldValue, std::string>
    read_field(const FieldDef& field, std::uint64_t base, const ByteSource& source)
    {
        FieldValue value;

        switch (field.kind)
        {
        case FieldKind::number:
        {
            const std::size_t width = number_width(field.number, field.size, source.pointer_size());
            const auto        raw   = load_bytes(base, field.offset, width, source);
            if (!raw)
            {
                return std::unexpected(raw.error());
            }
            const auto decoded = decode_number(number_token(field.number, source.pointer_size()), *raw);
            if (!decoded)
            {
                return std::unexpected(decoded.error());
            }
            value.kind     = FieldValue::Kind::number;
            value.number   = *decoded;
            value.integral = number_token_is_integer(field.number);
            return value;
        }
        case FieldKind::pointer:
        {
            const auto address = read_address(field, base, source);
            if (!address)
            {
                return std::unexpected(address.error());
            }
            value.kind    = FieldValue::Kind::address;
            value.address = *address;
            return value;
        }
        case FieldKind::class_pointer:
        {
            const auto address = read_address(field, base, source);
            if (!address)
            {
                return std::unexpected(address.error());
            }
            if (*address == 0)
            {
                value.kind = FieldValue::Kind::nothing;
                return value;
            }
            if (field.target_id == 0)
            {
                if (field.target.empty())
                {
                    value.kind    = FieldValue::Kind::address;
                    value.address = *address;
                    return value;
                }
                return std::unexpected(std::format("no class named '{}'", field.target));
            }
            value.kind    = FieldValue::Kind::instance;
            value.address = *address;
            value.target  = field.target;
            return value;
        }
        case FieldKind::class_value:
        {
            if (field.target_id == 0)
            {
                return std::unexpected(field.target.empty() ? std::string {"the field names no class"}
                                                            : std::format("no class named '{}'", field.target));
            }
            value.kind     = FieldValue::Kind::instance;
            value.address  = base + field.offset;
            value.target   = field.target;
            value.in_place = true;
            return value;
        }
        case FieldKind::array:
        {
            if (field.count > kMaxViewArray)
            {
                return std::unexpected(
                    std::format("the array holds {} element(s), past the {} element cap", field.count, kMaxViewArray));
            }
            if (field.element.kind == FieldKind::unsupported)
            {
                return std::unexpected(
                    std::format("the '{}' element kind has no script value", field.element.type_text));
            }
            if (field.element.kind == FieldKind::class_value && field.element.target_id == 0)
            {
                return std::unexpected(std::format("no class named '{}'", field.element.target));
            }

            std::uint64_t address = base + field.offset;
            if (field.through_pointer)
            {
                const auto pointed = read_address(field, base, source);
                if (!pointed)
                {
                    return std::unexpected(pointed.error());
                }
                if (*pointed == 0)
                {
                    value.kind = FieldValue::Kind::nothing;
                    return value;
                }
                address = *pointed;
            }

            value.kind     = FieldValue::Kind::array;
            value.address  = address;
            value.count    = field.count;
            value.stride   = field.stride;
            value.element  = field.element;
            value.in_place = !field.through_pointer;
            value.target   = field.element.target;
            return value;
        }
        case FieldKind::boolean:
        {
            const auto raw = load_bytes(base, field.offset, 1, source);
            if (!raw)
            {
                return std::unexpected(raw.error());
            }
            value.kind    = FieldValue::Kind::boolean;
            value.boolean = raw->front() != std::byte {0};
            return value;
        }
        case FieldKind::text_utf8:
        case FieldKind::text_utf16:
        {
            const auto text = read_text(field, base, source);
            if (!text)
            {
                return std::unexpected(text.error());
            }
            value.kind = FieldValue::Kind::text;
            value.text = *text;
            return value;
        }
        case FieldKind::bytes:
        {
            const auto raw = load_bytes(base, field.offset, field.size, source);
            if (!raw)
            {
                return std::unexpected(raw.error());
            }
            value.kind  = FieldValue::Kind::bytes;
            value.bytes = *raw;
            return value;
        }
        case FieldKind::unsupported:
            break;
        }

        return std::unexpected(std::format("the '{}' kind has no script value", field.type_text));
    }

    std::expected<void, std::string>
    write_field(const FieldDef& field, std::uint64_t base, const FieldValue& value, ByteSource& source)
    {
        const auto number_of = [](const FieldValue& entry)
        {
            return entry.kind == FieldValue::Kind::number ? entry.number : static_cast<double>(entry.address);
        };

        const auto store = [&field, base, &source](const std::vector<std::byte>& bytes)
        {
            return source.store(base, field.offset, bytes);
        };

        switch (field.kind)
        {
        case FieldKind::number:
        case FieldKind::pointer:
        case FieldKind::class_pointer:
        {
            if (value.kind != FieldValue::Kind::number && value.kind != FieldValue::Kind::address)
            {
                return std::unexpected(field.kind == FieldKind::number ? std::string {"the value must be a number"}
                                                                       : std::string {"the value must be an address"});
            }

            const double      number = number_of(value);
            const std::string token {field.kind == FieldKind::number
                                         ? number_token(field.number, source.pointer_size())
                                         : number_token(NumberToken::nuint, source.pointer_size())};

            if (is_integer_token(token))
            {
                if (!is_integral(value))
                {
                    return std::unexpected(std::format("the field holds a whole number, so {} is refused", number));
                }
                if (!integer_fits(token, number))
                {
                    return std::unexpected(std::format("the value is outside the field's '{}' range", token));
                }
                const std::uint64_t raw     = token.starts_with("i")
                                                ? static_cast<std::uint64_t>(static_cast<std::int64_t>(number))
                                                : static_cast<std::uint64_t>(number);
                const auto          encoded = encode_integer(token, raw);
                if (!encoded)
                {
                    return std::unexpected(encoded.error());
                }
                return store(*encoded);
            }

            const auto encoded = encode_number(token, number);
            if (!encoded)
            {
                return std::unexpected(encoded.error());
            }
            return store(*encoded);
        }
        case FieldKind::boolean:
        {
            if (value.kind == FieldValue::Kind::boolean)
            {
                return store({value.boolean ? std::byte {1} : std::byte {0}});
            }
            if ((value.kind == FieldValue::Kind::number || value.kind == FieldValue::Kind::address)
                && is_integral(value) && (number_of(value) == 0.0 || number_of(value) == 1.0))
            {
                return store({number_of(value) == 0.0 ? std::byte {0} : std::byte {1}});
            }
            return std::unexpected(std::string {"the field holds a boolean, so the value must be true or false"});
        }
        case FieldKind::text_utf8:
        case FieldKind::text_utf16:
        {
            if (value.kind != FieldValue::Kind::text)
            {
                return std::unexpected(std::string {"the value must be a string"});
            }
            const auto bytes = text_bytes(field, value.text);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            return store(*bytes);
        }
        case FieldKind::bytes:
        {
            if (value.kind != FieldValue::Kind::bytes)
            {
                return std::unexpected(std::string {"the value must be a string of bytes"});
            }
            if (value.bytes.size() != field.size)
            {
                return std::unexpected(std::format("the field holds exactly {} byte(s)", field.size));
            }
            return store(value.bytes);
        }
        case FieldKind::class_value:
            return std::unexpected(std::string {"an inlined class cannot be written"});
        case FieldKind::array:
            return std::unexpected(std::string {"an array cannot be written"});
        case FieldKind::unsupported:
            break;
        }

        return std::unexpected(std::format("the '{}' kind has no script value", field.type_text));
    }
}
