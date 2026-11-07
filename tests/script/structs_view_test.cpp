#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "reclass/project.hpp"
#include "reclass/value.hpp"
#include "script/structs_view.hpp"
#include "support/script_helpers.hpp"

namespace
{
    using slopkit::reclass::NodeKind;
    using slopkit::reclass::Project;
    using slopkit::script::BufferSource;
    using slopkit::script::build_class_index;
    using slopkit::script::ByteSource;
    using slopkit::script::ClassDef;
    using slopkit::script::ClassIndex;
    using slopkit::script::element_field;
    using slopkit::script::FieldDef;
    using slopkit::script::FieldKind;
    using slopkit::script::FieldValue;
    using slopkit::script::load_extent;
    using slopkit::script::NumberToken;
    using slopkit::script::read_field;
    using slopkit::script::write_field;

    constexpr std::uint64_t kBase = 0x1000;

    // A fake target window: bytes at `kBase`, counting its reads so a test can
    // tell a snapshot's single read from the reads a field access makes.
    class Window : public ByteSource
    {
    public:
        explicit Window(std::size_t size = 0x100) : bytes_(size) {}

        std::expected<void, std::string>
        load(std::uint64_t base, std::size_t offset, std::span<std::byte> out) const override
        {
            ++loads;
            const std::uint64_t address = base + offset;
            if (address < kBase || address - kBase + out.size() > bytes_.size())
            {
                return std::unexpected(std::string {"address is not mapped"});
            }
            std::memcpy(out.data(), bytes_.data() + (address - kBase), out.size());
            return {};
        }

        std::expected<void, std::string>
        store(std::uint64_t base, std::size_t offset, std::span<const std::byte> data) override
        {
            const std::uint64_t address = base + offset;
            if (address < kBase || address - kBase + data.size() > bytes_.size())
            {
                return std::unexpected(std::string {"address is not mapped"});
            }
            std::memcpy(bytes_.data() + (address - kBase), data.data(), data.size());
            return {};
        }

        [[nodiscard]] std::size_t pointer_size() const noexcept override
        {
            return 8;
        }

        template<typename T>
        void put(std::size_t offset, T value)
        {
            std::memcpy(bytes_.data() + offset, &value, sizeof(T));
        }

        void put_text(std::size_t offset, std::string_view text)
        {
            std::memcpy(bytes_.data() + offset, text.data(), text.size());
        }

        [[nodiscard]] std::vector<std::byte> take(std::size_t offset, std::size_t size) const
        {
            return std::vector<std::byte>(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                                          bytes_.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }

        mutable std::size_t loads {0};

    private:
        std::vector<std::byte> bytes_;
    };

    struct Fixture
    {
        Project    project;
        ClassIndex index;

        Fixture()
        {
            build_structs_fixture(project);
            index = build_class_index(project);
        }

        [[nodiscard]] const ClassDef& type(std::string_view name) const
        {
            const ClassDef* found = index.find(name);
            REQUIRE(found != nullptr);
            return *found;
        }

        [[nodiscard]] const FieldDef& field(std::string_view type_name, std::string_view field_name) const
        {
            const FieldDef* found = type(type_name).find(field_name);
            REQUIRE(found != nullptr);
            return *found;
        }
    };
}

TEST_CASE("the class index keeps every kind at its model offset", "[script][structs]")
{
    const Fixture fixture;

    CHECK(fixture.index.classes.size() == 3);
    CHECK(fixture.type("Player").extent == 105);

    const FieldDef& health = fixture.field("Player", "health");
    CHECK(health.offset == 0);
    CHECK(health.size == 4);
    CHECK(health.kind == FieldKind::number);
    CHECK(health.number == NumberToken::f32);
    CHECK(health.type_text == "f32");

    CHECK(fixture.field("Player", "ammo").offset == 4);
    CHECK(fixture.field("Player", "ammo").number == NumberToken::i32);

    const FieldDef& weapon = fixture.field("Player", "weapon");
    CHECK(weapon.offset == 8);
    CHECK(weapon.size == 8);
    CHECK(weapon.kind == FieldKind::class_pointer);
    CHECK(weapon.target == "Weapon");
    CHECK(weapon.target_id == fixture.type("Weapon").id);
    CHECK(weapon.type_text == "ptr -> Weapon");

    const FieldDef& position = fixture.field("Player", "pos");
    CHECK(position.offset == 16);
    CHECK(position.kind == FieldKind::class_value);
    CHECK(position.type_text == "Vec");

    const FieldDef& items = fixture.field("Player", "items");
    CHECK(items.offset == 24);
    CHECK(items.kind == FieldKind::array);
    CHECK(items.count == 3);
    CHECK(items.stride == 4);
    CHECK_FALSE(items.through_pointer);
    CHECK(items.type_text == "i32 [3]");

    CHECK(fixture.field("Player", "name").type_text == "utf8 [8]");
    CHECK(fixture.field("Player", "alive").kind == FieldKind::boolean);
    CHECK(fixture.field("Player", "alive").offset == 44);

    const FieldDef& flags = fixture.field("Player", "flags");
    CHECK(flags.kind == FieldKind::number);
    CHECK(flags.number == NumberToken::u32);
    CHECK(flags.type_text == "u32");

    CHECK(fixture.field("Player", "next").kind == FieldKind::pointer);
    CHECK(fixture.field("Player", "next").type_text == "ptr");

    const FieldDef& unsupported = fixture.field("Player", "u");
    CHECK(unsupported.kind == FieldKind::unsupported);
    CHECK(unsupported.type_text == slopkit::reclass::describe(NodeKind::union_value));

    const FieldDef& slots = fixture.field("Player", "slots");
    CHECK(slots.kind == FieldKind::array);
    CHECK(slots.count == 2);
    CHECK(slots.stride == 4);
    CHECK(slots.element.kind == FieldKind::class_value);
    CHECK(slots.element.target == "Weapon");
    CHECK_FALSE(slots.through_pointer);
    CHECK(slots.type_text == "Weapon [2]");

    const FieldDef& targets = fixture.field("Player", "targets");
    CHECK(targets.count == 2);
    CHECK(targets.stride == 4);
    CHECK(targets.through_pointer);

    CHECK(fixture.field("Player", "speed").number == NumberToken::f64);
    CHECK(fixture.field("Player", "tag").kind == FieldKind::text_utf16);
    CHECK(fixture.field("Player", "tag").count == 4);
    CHECK(fixture.field("Player", "blob").kind == FieldKind::bytes);
    CHECK(fixture.field("Player", "blob").size == 12);
}

TEST_CASE("numeric fields read and write through the mem codec", "[script][structs]")
{
    const Fixture fixture;
    Window        window;

    window.put<std::int32_t>(4, 7);
    window.put<float>(0, 12.5F);
    window.put<std::uint32_t>(45, 0xDEADBEEFU);
    window.put<double>(77, 2.5);

    const auto ammo = read_field(fixture.field("Player", "ammo"), kBase, window);
    REQUIRE(ammo.has_value());
    CHECK(ammo->kind == FieldValue::Kind::number);
    CHECK(ammo->integral);
    CHECK(ammo->number == 7.0);

    const auto health = read_field(fixture.field("Player", "health"), kBase, window);
    REQUIRE(health.has_value());
    CHECK_FALSE(health->integral);
    CHECK(health->number == 12.5);

    const auto flags = read_field(fixture.field("Player", "flags"), kBase, window);
    REQUIRE(flags.has_value());
    CHECK(flags->number == 0xDEADBEEFU);

    const auto speed = read_field(fixture.field("Player", "speed"), kBase, window);
    REQUIRE(speed.has_value());
    CHECK(speed->number == 2.5);

    FieldValue written;
    written.kind     = FieldValue::Kind::number;
    written.number   = 100.0;
    written.integral = true;
    REQUIRE(write_field(fixture.field("Player", "health"), kBase, written, window).has_value());
    CHECK(window.take(0, 4)
          == std::vector<std::byte> {std::byte {0x00}, std::byte {0x00}, std::byte {0xC8}, std::byte {0x42}});

    written.number = 30.0;
    REQUIRE(write_field(fixture.field("Player", "ammo"), kBase, written, window).has_value());
    CHECK(window.take(4, 4)
          == std::vector<std::byte> {std::byte {0x1E}, std::byte {0x00}, std::byte {0x00}, std::byte {0x00}});

    SECTION("a fractional value never reaches an integer field")
    {
        written.number     = 1.5;
        written.integral   = false;
        const auto refused = write_field(fixture.field("Player", "ammo"), kBase, written, window);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == "the field holds a whole number, so 1.5 is refused");
        CHECK(window.take(4, 4)
              == std::vector<std::byte> {std::byte {0x1E}, std::byte {0x00}, std::byte {0x00}, std::byte {0x00}});
    }

    SECTION("a value outside the field's range is refused")
    {
        written.number     = 4294967296.0;
        const auto refused = write_field(fixture.field("Player", "flags"), kBase, written, window);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == "the value is outside the field's 'u32' range");
    }

    SECTION("a bad address carries the seam's own reason")
    {
        const auto refused = read_field(fixture.field("Player", "ammo"), 0x9000, window);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == "address is not mapped");
    }
}

TEST_CASE("booleans, text and blobs map onto their own values", "[script][structs]")
{
    const Fixture fixture;
    Window        window;
    window.put<std::uint8_t>(44, 1);
    window.put_text(36, "hero");
    window.put_text(85, std::string_view {"\x11\x22\x00\x00", 4});

    const auto alive = read_field(fixture.field("Player", "alive"), kBase, window);
    REQUIRE(alive.has_value());
    CHECK(alive->kind == FieldValue::Kind::boolean);
    CHECK(alive->boolean);

    const auto name = read_field(fixture.field("Player", "name"), kBase, window);
    REQUIRE(name.has_value());
    CHECK(name->kind == FieldValue::Kind::text);
    CHECK(name->text == "hero");

    const auto tag = read_field(fixture.field("Player", "tag"), kBase, window);
    REQUIRE(tag.has_value());
    CHECK(tag->kind == FieldValue::Kind::text);
    CHECK(tag->text == "\xE2\x88\x91");

    window.put<std::uint8_t>(93, 0x2A);
    const auto blob = read_field(fixture.field("Player", "blob"), kBase, window);
    REQUIRE(blob.has_value());
    CHECK(blob->kind == FieldValue::Kind::bytes);
    REQUIRE(blob->bytes.size() == 12);
    CHECK(blob->bytes.front() == std::byte {0x2A});

    FieldValue written;
    written.kind    = FieldValue::Kind::boolean;
    written.boolean = false;
    REQUIRE(write_field(fixture.field("Player", "alive"), kBase, written, window).has_value());
    CHECK(window.take(44, 1) == std::vector<std::byte> {std::byte {0x00}});

    written.kind = FieldValue::Kind::text;
    written.text = "bo";
    REQUIRE(write_field(fixture.field("Player", "name"), kBase, written, window).has_value());
    CHECK(window.take(36, 8)
          == std::vector<std::byte> {std::byte {'b'},
                                     std::byte {'o'},
                                     std::byte {0},
                                     std::byte {0},
                                     std::byte {0},
                                     std::byte {0},
                                     std::byte {0},
                                     std::byte {0}});

    SECTION("text past the field's width is refused, not truncated")
    {
        written.text       = "way too long for the field";
        const auto refused = write_field(fixture.field("Player", "name"), kBase, written, window);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == "the text does not fit in 8 character(s)");
    }

    SECTION("a blob must match the field exactly")
    {
        written.kind       = FieldValue::Kind::bytes;
        written.bytes      = std::vector<std::byte> {std::byte {1}};
        const auto refused = write_field(fixture.field("Player", "blob"), kBase, written, window);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == "the field holds exactly 12 byte(s)");
    }
}

TEST_CASE("pointer fields resolve to instances or plain addresses", "[script][structs]")
{
    const Fixture fixture;
    Window        window;

    const auto null = read_field(fixture.field("Player", "weapon"), kBase, window);
    REQUIRE(null.has_value());
    CHECK(null->kind == FieldValue::Kind::nothing);

    window.put<std::uint64_t>(8, 0x2000);
    const auto weapon = read_field(fixture.field("Player", "weapon"), kBase, window);
    REQUIRE(weapon.has_value());
    CHECK(weapon->kind == FieldValue::Kind::instance);
    CHECK(weapon->address == 0x2000);
    CHECK(weapon->target == "Weapon");
    CHECK_FALSE(weapon->in_place);

    window.put<std::uint64_t>(49, 0x3000);
    const auto next = read_field(fixture.field("Player", "next"), kBase, window);
    REQUIRE(next.has_value());
    CHECK(next->kind == FieldValue::Kind::address);
    CHECK(next->address == 0x3000);

    SECTION("a pointer whose class the table no longer defines names it")
    {
        Project shrunk;
        build_structs_fixture(shrunk);
        shrunk.remove_class(0);

        const ClassIndex index = build_class_index(shrunk);
        const ClassDef*  type  = index.find("Player");
        REQUIRE(type != nullptr);
        const FieldDef* field = type->find("weapon");
        REQUIRE(field != nullptr);

        const auto refused = read_field(*field, kBase, window);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == "no class named 'Weapon'");
    }
}

TEST_CASE("inlined classes and arrays carry their own address", "[script][structs]")
{
    const Fixture fixture;
    Window        window;
    window.put<std::uint64_t>(69, 0x4000);
    window.put<std::int32_t>(24, 11);
    window.put<std::int32_t>(28, 22);
    window.put<std::int32_t>(32, 33);

    const auto position = read_field(fixture.field("Player", "pos"), kBase, window);
    REQUIRE(position.has_value());
    CHECK(position->kind == FieldValue::Kind::instance);
    CHECK(position->address == kBase + 16);
    CHECK(position->target == "Vec");
    CHECK(position->in_place);

    const auto items = read_field(fixture.field("Player", "items"), kBase, window);
    REQUIRE(items.has_value());
    CHECK(items->kind == FieldValue::Kind::array);
    CHECK(items->address == kBase + 24);
    CHECK(items->count == 3);
    CHECK(items->stride == 4);
    CHECK(items->in_place);

    const auto slots = read_field(fixture.field("Player", "slots"), kBase, window);
    REQUIRE(slots.has_value());
    CHECK(slots->kind == FieldValue::Kind::array);
    CHECK(slots->address == kBase + 61);
    CHECK(slots->count == 2);
    CHECK(slots->in_place);

    const auto targets = read_field(fixture.field("Player", "targets"), kBase, window);
    REQUIRE(targets.has_value());
    CHECK(targets->kind == FieldValue::Kind::array);
    CHECK(targets->address == 0x4000);
    CHECK_FALSE(targets->in_place);

    const FieldDef element = element_field(fixture.field("Player", "items").element);
    CHECK(element.kind == FieldKind::number);
    CHECK(element.number == NumberToken::i32);
    CHECK(element.size == 4);
    const auto third = read_field(element, items->address + 2 * items->stride, window);
    REQUIRE(third.has_value());
    CHECK(third->number == 33.0);

    SECTION("a union has no script value")
    {
        const auto refused = read_field(fixture.field("Player", "u"), kBase, window);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == "the 'Union' kind has no script value");
    }

    SECTION("an array past the cap is refused")
    {
        Project wide;
        build_structs_fixture(wide);
        slopkit::reclass::Class& player = wide.classes().back();
        player.nodes.push_back(slopkit::test::array_of(wide, 5000, NodeKind::int32));
        player.nodes.back()->name = "wide";

        const ClassIndex index = build_class_index(wide);
        const ClassDef*  type  = index.find("Player");
        REQUIRE(type != nullptr);
        const FieldDef* field = type->find("wide");
        REQUIRE(field != nullptr);
        const auto refused = read_field(*field, kBase, window);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == "the array holds 5000 element(s), past the 4096 element cap");
    }

    SECTION("a null instance array reads as no value")
    {
        window.put<std::uint64_t>(69, 0);
        const auto null = read_field(fixture.field("Player", "targets"), kBase, window);
        REQUIRE(null.has_value());
        CHECK(null->kind == FieldValue::Kind::nothing);
    }
}

TEST_CASE("a field whose type cannot take a value refuses the write", "[script][structs]")
{
    const Fixture fixture;
    Window        window;

    FieldValue value;
    value.kind     = FieldValue::Kind::number;
    value.number   = 1.0;
    value.integral = true;

    const auto inlined = write_field(fixture.field("Player", "pos"), kBase, value, window);
    REQUIRE_FALSE(inlined.has_value());
    CHECK(inlined.error() == "an inlined class cannot be written");

    const auto array = write_field(fixture.field("Player", "items"), kBase, value, window);
    REQUIRE_FALSE(array.has_value());
    CHECK(array.error() == "an array cannot be written");

    const auto unsupported = write_field(fixture.field("Player", "u"), kBase, value, window);
    REQUIRE_FALSE(unsupported.has_value());
    CHECK(unsupported.error() == "the 'Union' kind has no script value");

    FieldValue wrong;
    wrong.kind            = FieldValue::Kind::boolean;
    const auto wrong_kind = write_field(fixture.field("Player", "ammo"), kBase, wrong, window);
    REQUIRE_FALSE(wrong_kind.has_value());
    CHECK(wrong_kind.error() == "the value must be a number");
}

TEST_CASE("a snapshot reads its extent once and stays read-only", "[script][structs]")
{
    const Fixture fixture;
    Window        window;
    window.put<std::int32_t>(4, 7);
    window.put<std::uint64_t>(8, 0x2000);

    const ClassDef& player = fixture.type("Player");
    const auto      bytes  = load_extent(player, kBase, window);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() == player.extent);
    CHECK(window.loads == 1);

    BufferSource buffer {std::span<const std::byte>(*bytes), kBase, 8};
    const auto   ammo = read_field(fixture.field("Player", "ammo"), kBase, buffer);
    REQUIRE(ammo.has_value());
    CHECK(ammo->number == 7.0);
    CHECK(window.loads == 1);

    const auto weapon = read_field(fixture.field("Player", "weapon"), kBase, buffer);
    REQUIRE(weapon.has_value());
    CHECK(weapon->kind == FieldValue::Kind::instance);
    CHECK(weapon->address == 0x2000);

    FieldValue value;
    value.kind         = FieldValue::Kind::number;
    value.number       = 3.0;
    value.integral     = true;
    const auto refused = write_field(fixture.field("Player", "ammo"), kBase, value, buffer);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == "a snapshot is read-only");

    SECTION("a read past the extent names the address it missed")
    {
        const auto missed = read_field(fixture.field("Player", "ammo"), kBase + 0x1000, buffer);
        REQUIRE_FALSE(missed.has_value());
        CHECK(missed.error() == "the snapshot does not cover address 0x2004");
    }

    SECTION("a class past the snapshot cap is refused")
    {
        const auto taken = load_extent(player, kBase, window);
        REQUIRE(taken.has_value());
        CHECK(taken->size() == player.extent);
    }
}
