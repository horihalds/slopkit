#include "reclass/cpp_header.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "reclass/node.hpp"
#include "reclass/nodes/node.hpp"
#include "reclass/nodes/number.hpp"
#include "reclass/project.hpp"

namespace slopkit::reclass
{
    namespace
    {
        // The header's own include guard; fixed, so a second include of a
        // generated header is harmless.
        constexpr std::string_view kFileGuard = "SLOPKIT_RECLASS_HEADER";

        bool is_keyword(std::string_view name)
        {
            static constexpr std::string_view kKeywords[] = {
                "alignas",     "alignof",   "and",       "and_eq",    "asm",      "auto",         "bitand",
                "bitor",       "bool",      "break",     "case",      "catch",    "char",         "char16_t",
                "char32_t",    "char8_t",   "class",     "compl",     "concept",  "const",        "const_cast",
                "consteval",   "constexpr", "constinit", "continue",  "co_await", "co_return",    "co_yield",
                "decltype",    "default",   "delete",    "do",        "double",   "dynamic_cast", "else",
                "enum",        "explicit",  "export",    "extern",    "false",    "float",        "for",
                "friend",      "goto",      "if",        "inline",    "int",      "long",         "mutable",
                "namespace",   "new",       "noexcept",  "not",       "not_eq",   "nullptr",      "operator",
                "or",          "or_eq",     "private",   "protected", "public",   "register",     "reinterpret_cast",
                "requires",    "return",    "short",     "signed",    "sizeof",   "static",       "static_assert",
                "static_cast", "struct",    "switch",    "template",  "this",     "thread_local", "throw",
                "true",        "try",       "typedef",   "typeid",    "typename", "union",        "unsigned",
                "using",       "virtual",   "void",      "volatile",  "wchar_t",  "while",        "xor",
                "xor_eq"};

            for (const std::string_view keyword : kKeywords)
            {
                if (keyword == name)
                {
                    return true;
                }
            }
            return false;
        }

        // The user's name as a C++ identifier, or an empty string when there is
        // nothing left to name a member with.
        std::string sanitize(std::string_view name)
        {
            std::string out;
            out.reserve(name.size() + 1);
            for (const char c : name)
            {
                const bool keep = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
                out.push_back(keep ? c : '_');
            }
            if (out.empty())
            {
                return out;
            }
            if (std::isdigit(static_cast<unsigned char>(out.front())) != 0)
            {
                out.insert(out.begin(), '_');
            }
            if (is_keyword(out))
            {
                out.push_back('_');
            }
            return out;
        }

        // The name a class or an enum is emitted under: the user's when it reads
        // as an identifier, the `fallback` otherwise.
        std::string type_name(std::string_view name, std::string_view fallback)
        {
            std::string sanitized = sanitize(name);
            return sanitized.empty() ? std::string {fallback} : sanitized;
        }

        // `base` as a name nothing in the same scope uses yet: a second member
        // named `value` becomes `value_1`, a third `value_2`.
        std::string unique_name(std::string base, std::vector<std::string>& used)
        {
            if (base.empty())
            {
                base = "field";
            }

            std::string name = base;
            for (std::size_t suffix = 1; std::find(used.begin(), used.end(), name) != used.end(); ++suffix)
            {
                name = std::format("{}_{}", base, suffix);
            }
            used.push_back(name);
            return name;
        }

        // The word an unnamed member of `kind` is named after, so a generated
        // name reads like the member's type.
        std::string_view name_stem(NodeKind kind)
        {
            switch (kind)
            {
            case NodeKind::int8:
                return "int8";
            case NodeKind::int16:
                return "int16";
            case NodeKind::int32:
                return "int32";
            case NodeKind::int64:
                return "int64";
            case NodeKind::uint8:
                return "uint8";
            case NodeKind::uint16:
                return "uint16";
            case NodeKind::uint32:
                return "uint32";
            case NodeKind::uint64:
                return "uint64";
            case NodeKind::float32:
                return "float";
            case NodeKind::float64:
                return "double";
            case NodeKind::boolean:
                return "bool";
            case NodeKind::character:
                return "char";
            case NodeKind::hex8:
            case NodeKind::hex16:
            case NodeKind::hex32:
            case NodeKind::hex64:
                return "unknown";
            case NodeKind::pointer:
                return "pointer";
            case NodeKind::class_instance:
                return "instance";
            case NodeKind::class_pointer:
                return "class_pointer";
            case NodeKind::array:
                return "array";
            case NodeKind::union_value:
                return "union";
            case NodeKind::class_instance_array:
                return "instance_array";
            case NodeKind::class_wrapper_array:
                return "class_array";
            case NodeKind::nint:
                return "nint";
            case NodeKind::nuint:
                return "nuint";
            case NodeKind::bitfield:
                return "bitfield";
            case NodeKind::enum_node:
                return "enum";
            case NodeKind::utf8_text:
                return "utf8_text";
            case NodeKind::utf8_text_ptr:
                return "utf8_text_ptr";
            case NodeKind::utf16_text:
                return "utf16_text";
            case NodeKind::utf16_text_ptr:
                return "utf16_text_ptr";
            case NodeKind::utf32_text:
                return "utf32_text";
            case NodeKind::utf32_text_ptr:
                return "utf32_text_ptr";
            case NodeKind::vector2:
                return "vector2";
            case NodeKind::vector3:
                return "vector3";
            case NodeKind::vector4:
                return "vector4";
            case NodeKind::matrix3x3:
                return "matrix3x3";
            case NodeKind::matrix3x4:
                return "matrix3x4";
            case NodeKind::matrix4x4:
                return "matrix4x4";
            case NodeKind::virtual_method_table:
                return "vtable";
            case NodeKind::virtual_method:
                return "vmethod";
            case NodeKind::function:
                return "function";
            case NodeKind::function_ptr:
                return "function_ptr";
            }
            return "field";
        }

        std::string
        member_name(const Node& node, std::size_t offset, std::string_view stem, std::vector<std::string>& used)
        {
            const std::string base = sanitize(node.name);
            return unique_name(base.empty() ? std::format("{}_{:#x}", stem, offset) : base, used);
        }

        // The C++ type one member of `kind` carries on its own; empty for the
        // kinds whose declaration is more than a type, which the caller writes.
        std::string_view scalar_type(NodeKind kind)
        {
            switch (kind)
            {
            case NodeKind::int8:
                return "int8_t";
            case NodeKind::int16:
                return "int16_t";
            case NodeKind::int32:
                return "int32_t";
            case NodeKind::int64:
                return "int64_t";
            case NodeKind::uint8:
                return "uint8_t";
            case NodeKind::uint16:
                return "uint16_t";
            case NodeKind::uint32:
                return "uint32_t";
            case NodeKind::uint64:
                return "uint64_t";
            case NodeKind::float32:
                return "float";
            case NodeKind::float64:
                return "double";
            case NodeKind::boolean:
                return "bool";
            case NodeKind::character:
                return "char";
            case NodeKind::hex8:
                return "uint8_t";
            case NodeKind::hex16:
                return "uint16_t";
            case NodeKind::hex32:
                return "uint32_t";
            case NodeKind::hex64:
                return "uint64_t";
            case NodeKind::pointer:
                return "void*";
            case NodeKind::function_ptr:
                return "void*";
            case NodeKind::nint:
                return "ptrdiff_t";
            case NodeKind::nuint:
                return "size_t";
            // wchar_t is four bytes here while a utf16 node stores two, so the
            // fixed-width characters carry the encodings instead.
            case NodeKind::utf8_text_ptr:
                return "char*";
            case NodeKind::utf16_text_ptr:
                return "char16_t*";
            case NodeKind::utf32_text_ptr:
                return "char32_t*";
            case NodeKind::vector2:
                return "Vector2";
            case NodeKind::vector3:
                return "Vector3";
            case NodeKind::vector4:
                return "Vector4";
            case NodeKind::matrix3x3:
                return "Matrix3x3";
            case NodeKind::matrix3x4:
                return "Matrix3x4";
            case NodeKind::matrix4x4:
                return "Matrix4x4";
            case NodeKind::virtual_method_table:
                return "void*";
            case NodeKind::virtual_method:
                return "void*";
            default:
                return {};
            }
        }

        // The fixed-width integer of `bytes` bytes; empty for a width no integer
        // type has.
        std::string_view integer_type(std::size_t bytes, bool signed_bits)
        {
            switch (bytes)
            {
            case 1:
                return signed_bits ? "int8_t" : "uint8_t";
            case 2:
                return signed_bits ? "int16_t" : "uint16_t";
            case 4:
                return signed_bits ? "int32_t" : "uint32_t";
            case 8:
                return signed_bits ? "int64_t" : "uint64_t";
            default:
                return {};
            }
        }

        // The next whole integer width that holds `bytes` bytes.
        std::size_t integer_width(std::size_t bytes)
        {
            std::size_t width = 1;
            while (width < bytes)
            {
                width *= 2;
            }
            return std::min(width, std::size_t {8});
        }

        std::string_view character_type(NodeKind kind)
        {
            switch (kind)
            {
            case NodeKind::utf16_text:
                return "char16_t";
            case NodeKind::utf32_text:
                return "char32_t";
            default:
                return "char";
            }
        }

        // Whether a class has a known non-zero layout, which is what a member
        // that inlines it needs. A class that contains itself by value has none.
        bool has_bytes(const Project& project, const Class& type)
        {
            const std::expected<Layout, std::string> layout = layout_class(project, type);
            return layout.has_value() && layout->size > 0;
        }

        // Whether the header declares a class. A class with no members at all is
        // left out: no C++ struct occupies none of the bytes the project holds
        // for it, which would break every class that inlines it.
        bool declared(const Project& project, const Class& type)
        {
            if (type.nodes.empty())
            {
                return false;
            }
            const std::expected<Layout, std::string> layout = layout_class(project, type);
            return !layout.has_value() || layout->size > 0;
        }

        // The class a by-value reference inlines, or nullptr when the project
        // does not hold it or the header does not declare it.
        const Class* inlined_class(const Project& project, const Node& node)
        {
            const NodeKind kind = node.kind();
            if (kind != NodeKind::class_instance && kind != NodeKind::class_wrapper_array)
            {
                return nullptr;
            }

            const Class* target = project.find_class(node.class_name);
            return target != nullptr && has_bytes(project, *target) ? target : nullptr;
        }

        // The offsets `nodes` sit at when each is counted as the bytes it holds.
        // A class that contains itself has no layout to ask for, so this is what
        // its members are written at.
        std::vector<std::size_t> sequential_offsets(const Project& project, std::span<const NodePtr> nodes)
        {
            std::vector<std::size_t> offsets;
            offsets.reserve(nodes.size());

            std::size_t offset = 0;
            for (const NodePtr& node : nodes)
            {
                offsets.push_back(offset);
                offset += node_size(project, *node);
            }
            return offsets;
        }

        // One class the header declares, with the name it is declared under.
        struct ExportedClass
        {
            const Class* type {};
            std::string  name;
        };

        std::string_view emitted_name(std::span<const ExportedClass> classes, const Class* type)
        {
            for (const ExportedClass& entry : classes)
            {
                if (entry.type == type)
                {
                    return entry.name;
                }
            }
            return {};
        }

        std::size_t class_index(const Project& project, const Class& type)
        {
            const std::vector<Class>& classes  = project.classes();
            const auto                position = std::find_if(classes.begin(),
                                                              classes.end(),
                                                              [&type](const Class& entry)
                                                              {
                                                   return &entry == &type;
                                                              });
            return position == classes.end() ? classes.size() : static_cast<std::size_t>(position - classes.begin());
        }

        // The name a class is emitted under; the user's unless it is empty or
        // reads as something other than an identifier.
        std::string class_name(const Project& project, const Class& type)
        {
            return type_name(type.name, std::format("Class_{:#x}", class_index(project, type)));
        }

        // The name an enum is emitted under.
        std::string enum_name(const Project& project, const EnumDescription& entry)
        {
            const std::vector<EnumDescription>& enums    = project.enums();
            const auto                          position = std::find_if(enums.begin(),
                                                                        enums.end(),
                                                                        [&entry](const EnumDescription& candidate)
                                                                        {
                                                   return &candidate == &entry;
                                                                        });
            const std::size_t                   index =
                position == enums.end() ? enums.size() : static_cast<std::size_t>(position - enums.begin());
            return type_name(entry.name, std::format("Enum_{:#x}", index));
        }

        // Renders the members of one structure into a document.
        class Emitter
        {
        public:
            Emitter(const Project& project, std::span<const ExportedClass> classes)
                : project_(project), classes_(classes)
            {
            }

            // The lines for `nodes` in declaration order, `offsets` holding each
            // node's offset inside the structure. `comments` is off for the
            // members of a union, which share the union's one address.
            void write_members(std::span<const NodePtr>     nodes,
                               std::span<const std::size_t> offsets,
                               std::string_view             indent,
                               bool                         comments)
            {
                std::vector<std::string> used;
                std::size_t              index = 0;
                while (index < nodes.size())
                {
                    if (is_hex_kind(nodes[index]->kind()))
                    {
                        index = write_hex_run(nodes, offsets, index, indent, comments, used);
                        continue;
                    }

                    write_member(*nodes[index], offsets[index], indent, comments, used);
                    ++index;
                }
            }

            // One class between the packing pragma's push and pop. A class that
            // contains itself by value has no layout the project can state, so
            // its members are written at their own widths with the member that
            // closes the cycle left out and no size assertion.
            void write_class(const ExportedClass& entry)
            {
                const std::expected<Layout, std::string> layout = layout_class(project_, *entry.type);
                if (!layout.has_value())
                {
                    out_ += std::format("struct {}  // the class contains itself by value\n{{\n", entry.name);
                    write_members(entry.type->nodes, sequential_offsets(project_, entry.type->nodes), "    ", true);
                    out_ += "};\n\n";
                    return;
                }

                out_ += std::format("struct {}  // {:#x} bytes\n{{\n", entry.name, layout->size);
                write_members(entry.type->nodes, layout->offsets, "    ", true);
                out_ += "};\n";
                out_ += std::format(
                    "static_assert(sizeof({}) == {:#x}, \"{}\");\n\n", entry.name, layout->size, entry.name);
            }

            [[nodiscard]] const std::string& text() const noexcept
            {
                return out_;
            }

        private:
            void write_line(std::string_view indent, bool comments, std::size_t offset, std::string_view declaration)
            {
                out_ += indent;
                if (comments)
                {
                    out_ += std::format("/* {:#04x} */ ", offset);
                }
                out_ += declaration;
                out_ += '\n';
            }

            // A run of hex nodes is the bytes no member names; one block of them
            // keeps the class byte-exact where one member per hex node would
            // leave a hole between the kinds that do have a type.
            std::size_t write_hex_run(std::span<const NodePtr>     nodes,
                                      std::span<const std::size_t> offsets,
                                      std::size_t                  start,
                                      std::string_view             indent,
                                      bool                         comments,
                                      std::vector<std::string>&    used)
            {
                std::size_t bytes = 0;
                std::size_t end   = start;
                while (end < nodes.size() && is_hex_kind(nodes[end]->kind()))
                {
                    const std::size_t size = node_size(project_, *nodes[end]);
                    if (size == 0 || offsets[end] != offsets[start] + bytes)
                    {
                        break;
                    }
                    bytes += size;
                    ++end;
                }

                if (bytes == 0)
                {
                    return start + 1;
                }
                if (end - start == 1)
                {
                    write_line(
                        indent,
                        comments,
                        offsets[start],
                        std::format("{} {};",
                                    scalar_type(nodes[start]->kind()),
                                    member_name(*nodes[start], offsets[start], name_stem(nodes[start]->kind()), used)));
                    return end;
                }

                write_line(
                    indent,
                    comments,
                    offsets[start],
                    std::format("uint8_t {}[{:#x}];", member_name(*nodes[start], offsets[start], "pad", used), bytes));
                return end;
            }

            void write_member(const Node&               node,
                              std::size_t               offset,
                              std::string_view          indent,
                              bool                      comments,
                              std::vector<std::string>& used)
            {
                const std::string name = member_name(node, offset, name_stem(node.kind()), used);
                const std::size_t size = node_size(project_, node);

                switch (node.kind())
                {
                case NodeKind::array:
                    write_array(node, name, offset, indent, comments);
                    return;
                case NodeKind::union_value:
                    write_union(node, name, offset, indent, comments);
                    return;
                case NodeKind::bitfield:
                {
                    const auto&       field = static_cast<const BitFieldNode&>(node);
                    const std::size_t bytes = (field.bits + 7) / 8;
                    const std::size_t width = integer_width(bytes);
                    if (width != bytes)
                    {
                        write_raw_bytes(size, name, offset, indent, comments);
                        return;
                    }
                    write_line(indent,
                               comments,
                               offset,
                               std::format("{} {} : {};", integer_type(width, field.signed_bits), name, field.bits));
                    return;
                }
                case NodeKind::utf8_text:
                case NodeKind::utf16_text:
                case NodeKind::utf32_text:
                    write_line(
                        indent,
                        comments,
                        offset,
                        std::format("{} {}[{:#x}];", character_type(node.kind()), name, node.extent().value_or(size)));
                    return;
                case NodeKind::enum_node:
                {
                    const std::string type = named_type(node);
                    if (type.empty())
                    {
                        write_raw_bytes(size, name, offset, indent, comments);
                        return;
                    }
                    write_line(indent, comments, offset, std::format("{} {};", type, name));
                    return;
                }
                case NodeKind::class_instance:
                {
                    const Class* target = inlined_class(project_, node);
                    if (target == nullptr)
                    {
                        write_skipped(node, name, offset, indent, comments);
                        return;
                    }
                    write_line(indent, comments, offset, std::format("{} {};", emitted_name(classes_, target), name));
                    return;
                }
                case NodeKind::class_wrapper_array:
                {
                    const Class* target = inlined_class(project_, node);
                    if (target == nullptr)
                    {
                        write_skipped(node, name, offset, indent, comments);
                        return;
                    }
                    write_line(
                        indent,
                        comments,
                        offset,
                        std::format("{} {}[{:#x}];", emitted_name(classes_, target), name, node.extent().value_or(1)));
                    return;
                }
                // Both are one pointer wide: the instance array holds the
                // address of the classes it counts, not the classes themselves.
                case NodeKind::class_pointer:
                case NodeKind::class_instance_array:
                    write_line(indent, comments, offset, std::format("{} {};", pointer_type(node.class_name), name));
                    return;
                default:
                    break;
                }

                if (size == 0)
                {
                    return;
                }

                const std::string_view type = scalar_type(node.kind());
                if (type.empty() || size != kind_size(node.kind()))
                {
                    write_raw_bytes(size, name, offset, indent, comments);
                    return;
                }

                write_line(indent, comments, offset, std::format("{} {};", type, name));
            }

            // An array of one repeated element maps to `T name[count]`; an array
            // whose elements differ is written as the bytes it holds.
            void write_array(
                const Node& node, std::string_view name, std::size_t offset, std::string_view indent, bool comments)
            {
                const std::span<const NodePtr> elements = node.children();
                std::string                    element  = element_type(elements);
                if (element.empty())
                {
                    write_raw_bytes(node_size(project_, node), name, offset, indent, comments);
                    return;
                }

                write_line(indent, comments, offset, std::format("{} {}[{:#x}];", element, name, elements.size()));
            }

            // A union's members all sit at the union's own address, so they are
            // written without an offset of their own.
            void write_union(
                const Node& node, std::string_view name, std::size_t offset, std::string_view indent, bool comments)
            {
                const std::span<const NodePtr> children = node.children();
                const std::vector<std::size_t> offsets(children.size(), 0);

                Emitter inner(project_, classes_);
                inner.write_members(children, offsets, std::format("{}    ", indent), false);
                if (inner.text().empty())
                {
                    return;
                }

                // The members sit one level deeper than the member the union
                // itself is declared as.
                out_ += indent;
                if (comments)
                {
                    out_ += std::format("/* {:#04x} */ ", offset);
                }
                out_ += std::format("union\n{}{{\n", indent);
                out_ += inner.text();
                out_ += std::format("{}}} {};\n", indent, name);
            }

            // A member the header cannot declare keeps its line, so the reader
            // sees which member the project held there.
            void write_skipped(
                const Node& node, std::string_view name, std::size_t offset, std::string_view indent, bool comments)
            {
                write_line(indent,
                           comments,
                           offset,
                           std::format("// {}: class {} has no size of its own", name, node.class_name));
            }

            void write_raw_bytes(
                std::size_t size, std::string_view name, std::size_t offset, std::string_view indent, bool comments)
            {
                if (size > 0)
                {
                    write_line(indent, comments, offset, std::format("uint8_t {}[{:#x}];", name, size));
                }
            }

            // The element type of an array whose elements all share one kind and
            // one size, so `T name[count]` declares exactly the bytes it holds.
            std::string element_type(std::span<const NodePtr> elements) const
            {
                if (elements.empty())
                {
                    return {};
                }

                const NodeKind    kind = elements.front()->kind();
                const std::size_t size = node_size(project_, *elements.front());
                if (size == 0)
                {
                    return {};
                }
                for (const NodePtr& element : elements)
                {
                    if (element->kind() != kind || node_size(project_, *element) != size)
                    {
                        return {};
                    }
                }
                return named_type(*elements.front());
            }

            // The type a node stands under in a declaration of its own; empty
            // for a kind whose declaration is more than a type.
            std::string named_type(const Node& node) const
            {
                switch (node.kind())
                {
                case NodeKind::class_instance:
                {
                    const Class* target = inlined_class(project_, node);
                    return target == nullptr ? std::string {} : std::string {emitted_name(classes_, target)};
                }
                case NodeKind::class_pointer:
                case NodeKind::class_instance_array:
                    return pointer_type(node.class_name);
                case NodeKind::enum_node:
                {
                    const EnumDescription* entry = project_.find_enum(node.class_name);
                    if (entry != nullptr)
                    {
                        return enum_name(project_, *entry);
                    }
                    return std::string {integer_type(node_size(project_, node), false)};
                }
                default:
                    break;
                }
                return std::string {scalar_type(node.kind())};
            }

            // A pointer into a class the header declares keeps the class's type;
            // one into a class it does not declare is a plain pointer.
            std::string pointer_type(std::string_view class_name) const
            {
                const Class*           target = project_.find_class(class_name);
                const std::string_view name = target != nullptr ? emitted_name(classes_, target) : std::string_view {};
                return name.empty() ? std::string {"void*"} : std::format("{}*", name);
            }

            const Project&                 project_;
            std::span<const ExportedClass> classes_;
            std::string                    out_;
        };

        // The classes a class embeds by value, in member order.
        std::vector<const Class*> inlined_classes(const Project& project, const Class& type)
        {
            std::vector<const Class*> targets;
            for (const NodePtr& node : type.nodes)
            {
                const Class* target = inlined_class(project, *node);
                if (target != nullptr)
                {
                    targets.push_back(target);
                }
            }
            return targets;
        }

        // `type` after every class it embeds by value, so an embedder is always
        // defined once the class it holds is complete. A class that contains
        // itself is not ordered through: the cycle has no first class to start
        // from, and the classes around it still export.
        void order_class(const Project&             project,
                         const Class&               type,
                         std::vector<const Class*>& ordered,
                         std::vector<const Class*>& seen)
        {
            if (!declared(project, type) || std::find(seen.begin(), seen.end(), &type) != seen.end())
            {
                return;
            }
            seen.push_back(&type);

            for (const Class* target : inlined_classes(project, type))
            {
                order_class(project, *target, ordered, seen);
            }
            ordered.push_back(&type);
        }

        std::vector<ExportedClass> exported_classes(const Project& project)
        {
            std::vector<const Class*> ordered;
            std::vector<const Class*> seen;
            for (const Class& type : project.classes())
            {
                order_class(project, type, ordered, seen);
            }

            std::vector<ExportedClass> classes;
            std::vector<std::string>   names;
            for (const Class* type : ordered)
            {
                classes.push_back({.type = type, .name = unique_name(class_name(project, *type), names)});
            }
            return classes;
        }

        // One helper type the header may define. The fields and the size are the
        // shape the reference's vector and matrix nodes are read with.
        struct HelperType
        {
            std::string_view name;
            std::string_view fields;
            std::size_t      size {};
        };

        constexpr HelperType kHelperTypes[] {
            {  "Vector2",       "float x, y;",  8},
            {  "Vector3",    "float x, y, z;", 12},
            {  "Vector4", "float x, y, z, w;", 16},
            {"Matrix3x3",    "float m[3][3];", 36},
            {"Matrix3x4",    "float m[3][4];", 48},
            {"Matrix4x4",    "float m[4][4];", 64},
        };

        // The preprocessor name a helper type's guard carries.
        std::string guard_name(std::string_view type)
        {
            std::string out;
            out.reserve(type.size() + 7);
            for (const char c : type)
            {
                out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
            out += "_DEFINED";
            return out;
        }

        // The geometry types a target may own already. Each carries its own
        // guard, so a target that defines only some of them keeps its own
        // definitions and still gets the rest.
        std::string helper_types()
        {
            std::string out;
            for (const HelperType& type : kHelperTypes)
            {
                const std::string guard = guard_name(type.name);
                out += std::format("#ifndef {0}\n#define {0}\n", guard);
                out += std::format("struct {} {{ {} }};\n", type.name, type.fields);
                out += std::format("static_assert(sizeof({}) == {:#x});\n", type.name, type.size);
                out += "#endif\n\n";
            }
            return out;
        }

        // A value's number. An eight-byte enum needs the suffix to keep the
        // literal inside its underlying type.
        std::string enum_number(std::uint64_t value, std::size_t size)
        {
            std::string text = std::format("{:#x}", value);
            if (size == 8)
            {
                text += "ull";
            }
            return text;
        }

        std::string enum_definitions(const Project& project)
        {
            std::string out;
            for (const EnumDescription& entry : project.enums())
            {
                out +=
                    std::format("enum class {} : {}\n{{\n", enum_name(project, entry), integer_type(entry.size, false));

                std::vector<std::string> used;
                for (const EnumValue& value : entry.values)
                {
                    const std::string base = sanitize(value.name);
                    const std::string name =
                        unique_name(base.empty() ? std::format("Value_{:#x}", value.value) : base, used);

                    out += std::format("    {} = {},", name, enum_number(value.value, entry.size));
                    if (!value.comment.empty())
                    {
                        out += std::format("  // {}", value.comment);
                    }
                    out += '\n';
                }

                out += "};\n\n";
            }
            return out;
        }
    }

    std::string cpp_header(const Project& project)
    {
        const std::vector<ExportedClass> classes = exported_classes(project);
        Emitter                          emitter(project, classes);

        std::string out;
        out += "// Generated by slopkit from the ReClass project. Do not edit.\n";
        out += std::format("#ifndef {}\n#define {}\n\n", kFileGuard, kFileGuard);
        out += "#include <cstddef>\n#include <cstdint>\n\n";
        out += helper_types();

        const std::string enums = enum_definitions(project);
        if (!enums.empty())
        {
            out += enums;
        }

        if (!classes.empty())
        {
            for (const ExportedClass& entry : classes)
            {
                out += std::format("struct {};\n", entry.name);
            }
            out += "\n#pragma pack(push, 1)\n";
            for (const ExportedClass& entry : classes)
            {
                emitter.write_class(entry);
            }
            out += emitter.text();
            out += "#pragma pack(pop)\n\n";
        }

        out += std::format("#endif  // {}\n", kFileGuard);
        return out;
    }

}
