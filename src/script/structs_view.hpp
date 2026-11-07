#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopkit::reclass
{
    class Project;
}

namespace slopkit::script
{
    // The caps a class view holds a script to: a run longer than kMaxViewArray
    // elements is refused instead of turned into a proxy, a text field wider than
    // kMaxViewText characters reads at most that far, and a snapshot larger than
    // kMaxViewSnapshot bytes is refused instead of read into a buffer.
    inline constexpr std::size_t kMaxViewArray    = 4096;
    inline constexpr std::size_t kMaxViewText     = 4096;
    inline constexpr std::size_t kMaxViewSnapshot = 1 << 20;

    // The shape a script sees one class field as. The numeric kinds all read and
    // write through the mem codec; the rest are what the codec has no token for.
    enum class FieldKind
    {
        unsupported,   // no script value at all: a bit field, an enum, a union, UTF-32 text
        number,        // a number, decoded through the codec per `number`
        pointer,       // an address held as a whole number, as wide as the target's pointers
        class_pointer, // an address naming a class; nil when it is zero
        class_value,   // a class inlined where the field sits
        array,         // a run of elements
        boolean,
        text_utf8,
        text_utf16,
        bytes, // a raw blob the script reads as a string: a vector, a matrix, a function body
    };

    // The number a `number` field reads through. The `nint`/`nuint` widths are
    // the target's pointer width, like `pointer`.
    enum class NumberToken
    {
        none,
        u8,
        i8,
        u16,
        i16,
        u32,
        i32,
        u64,
        i64,
        f32,
        f64,
        nint,
        nuint,
    };

    // The codec token a number reads and writes through, with the target's
    // pointer width filling in the pointer-wide ones; empty when `number` has no
    // token at all.
    [[nodiscard]] std::string_view number_token(NumberToken number, std::size_t pointer_size) noexcept;

    [[nodiscard]] bool number_token_is_integer(NumberToken number) noexcept;

    // The bytes the field occupies in the class it belongs to, and the width the
    // script reads: a pointer-wide numeric kind reads `pointer_size` bytes
    // wherever the model stores the pointer in a fixed-width slot.
    [[nodiscard]] std::size_t
    number_width(NumberToken number, std::size_t model_size, std::size_t pointer_size) noexcept;

    struct ElementDef
    {
        FieldKind     kind {FieldKind::unsupported};
        NumberToken   number {NumberToken::none};
        std::string   type_text; // how the element's type is spelled to a script
        std::string   target;    // the class a class element names, empty when it names none
        std::uint64_t target_id {};
        std::size_t   size {};  // the bytes one element occupies
        std::size_t   count {}; // the characters of a text element
    };

    struct FieldDef
    {
        std::string   name;
        std::string   type_text; // how the field's type is spelled to a script
        FieldKind     kind {FieldKind::unsupported};
        NumberToken   number {NumberToken::none};
        std::size_t   offset {};
        std::size_t   size {}; // the bytes the field occupies in its class
        std::string   target;  // the class a class kind names, empty when it names none
        std::uint64_t target_id {};
        std::size_t   count {};  // text characters, blob bytes or array elements
        std::size_t   stride {}; // the bytes between array elements
        // An array whose elements live at the address the field holds rather
        // than at the field's own address: the class instance arrays.
        bool          through_pointer {};
        ElementDef    element; // what one element of an array field reads as
    };

    struct ClassDef
    {
        std::uint64_t         id {};
        std::string           name;
        std::size_t           extent {}; // the bytes a snapshot of the class reads
        std::vector<FieldDef> fields;

        [[nodiscard]] const FieldDef* find(std::string_view field) const noexcept;
    };

    // The classes a script can name, as they stood when the snapshot was taken:
    // the binding never keeps a pointer into the editing model, so a rename or a
    // deletion between two ticks shows up in the next snapshot instead of
    // dangling.
    struct ClassIndex
    {
        std::vector<ClassDef> classes;

        [[nodiscard]] const ClassDef* find(std::string_view name) const noexcept;
    };

    [[nodiscard]] ClassIndex build_class_index(const reclass::Project& project);

    [[nodiscard]] std::shared_ptr<const ClassIndex> class_index_of(const reclass::Project& project);

    // What one field read produced, in the shape the binding turns into a Lua
    // value without touching memory again.
    struct FieldValue
    {
        enum class Kind
        {
            number,
            boolean,
            text,
            bytes,
            address,  // a pointer read with no target class, as a whole number
            instance, // a class value, or a pointer followed to its target
            array,
            nothing, // a null class pointer
        };

        Kind                   kind {};
        double                 number {};
        bool                   boolean {};
        bool                   integral {}; // the number came from a whole number
        std::string            text;
        std::vector<std::byte> bytes;
        std::uint64_t          address {};
        // The class the value names: the target of a followed pointer, the
        // inlined class, or the element type of an array.
        std::string            target;
        // The value lives in the field's own bytes, so a snapshot serves it,
        // rather than at an address read out of the field.
        bool                   in_place {};
        std::size_t            count {};
        std::size_t            stride {};
        ElementDef             element;
    };

    // The bytes a view reads and writes through: live target memory, or a
    // snapshot's buffer. Both fail with the seam's own text, which the caller
    // prefixes with the access that asked.
    class ByteSource
    {
    public:
        virtual ~ByteSource() = default;

        // Fills `out` from `base + offset`.
        [[nodiscard]] virtual std::expected<void, std::string>
        load(std::uint64_t base, std::size_t offset, std::span<std::byte> out) const = 0;

        [[nodiscard]] virtual std::expected<void, std::string>
        store(std::uint64_t base, std::size_t offset, std::span<const std::byte> data) = 0;

        [[nodiscard]] virtual std::size_t pointer_size() const noexcept = 0;
    };

    // A snapshot's bytes: the extent read once from the target, served from
    // `bytes` and covering `length` bytes from `origin`. Writing fails.
    class BufferSource : public ByteSource
    {
    public:
        BufferSource(std::span<const std::byte> bytes, std::uint64_t origin, std::size_t pointer_size);

        [[nodiscard]] std::expected<void, std::string>
        load(std::uint64_t base, std::size_t offset, std::span<std::byte> out) const override;

        [[nodiscard]] std::expected<void, std::string>
        store(std::uint64_t base, std::size_t offset, std::span<const std::byte> data) override;

        [[nodiscard]] std::size_t pointer_size() const noexcept override;

    private:
        std::span<const std::byte> bytes_;
        std::uint64_t              origin_ {};
        std::size_t                pointer_size_ {8};
    };

    // The whole class extent in one read, which is what a snapshot holds; the
    // reason names nothing, so the caller prefixes the class it snapshotted.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string>
    load_extent(const ClassDef& type, std::uint64_t base, const ByteSource& source);

    // Reads one field at `base`. Reasons come back unprefixed, so the binding
    // can name the field it asked for.
    [[nodiscard]] std::expected<FieldValue, std::string>
    read_field(const FieldDef& field, std::uint64_t base, const ByteSource& source);

    // Writes one field at `base`. The same reason style as a read.
    [[nodiscard]] std::expected<void, std::string>
    write_field(const FieldDef& field, std::uint64_t base, const FieldValue& value, ByteSource& source);

    // The field one element of an array reads as, for a read at the
    // element's own address.
    [[nodiscard]] FieldDef element_field(const ElementDef& element);

}
