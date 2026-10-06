#include <catch2/catch.hpp>

#include "support/memory_view_helpers.hpp"

TEST_CASE("the memory view document windows the visible block and its neighbours", "[ui]")
{
    SECTION("windows the visible block and its neighbours")
    {
        Fixture    fixture;
        const auto requests = fixture.document.next_live_request();

        REQUIRE(requests.size() == 3);
        CHECK(requests[0].id == 0);
        CHECK(requests[0].address == kBase - kExtent);
        CHECK(requests[0].size == kExtent);
        CHECK(requests[1].id == 1);
        CHECK(requests[1].address == kBase);
        CHECK(requests[1].size == kExtent);
        CHECK(requests[2].id == 2);
        CHECK(requests[2].address == kBase + kExtent);
        CHECK(requests[2].size == kExtent);
    }

    SECTION("scrolling inside a block keeps the request set and reuses the cache")
    {
        Fixture fixture;
        fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0x11});
        fixture.pass();

        // A scroll within the visible block does not move the window.
        fixture.document.set_view(kBase + kRowBytes, kRowBytes, kRows);
        CHECK(fixture.document.visible_rows() == kRows);
        CHECK(fixture.document.bytes_per_row() == kRowBytes);
        const auto requests = fixture.document.next_live_request();
        REQUIRE(requests.size() == 3);
        CHECK(requests[1].address == kBase);

        // Crossing into the next block slides the window but reuses the two
        // overlapping blocks, so the newly visible bytes are already readable.
        fixture.document.set_view(kBase + kExtent, kRowBytes, kRows);
        CHECK(fixture.document.cell(kBase + kExtent + 5).readable);
        CHECK(fixture.document.cell(kBase + kExtent + 5).text == QStringLiteral("11"));
        // The block that fell out of the window is gone.
        fixture.document.set_view(kBase - kExtent, kRowBytes, kRows);
        CHECK_FALSE(fixture.document.cell(kBase - kExtent).readable);
    }
}

TEST_CASE("the memory view document marks only bytes that differ from the last reading", "[ui]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0x11});

    int repaints = 0;
    QObject::connect(&fixture.document,
                     &MemoryViewDocument::repaintRequested,
                     [&]
                     {
                         ++repaints;
                     });

    fixture.pass();
    CHECK(repaints == 1);
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("11"));
    CHECK_FALSE(fixture.document.cell(kBase).changed); // The first reading is never changed.

    // An idle reading never repaints.
    fixture.pass();
    CHECK(repaints == 1);

    // One byte moves: only that byte is flagged.
    fixture.access.memory->bytes[kBase + 1] = std::byte {0x22};
    fixture.pass();
    CHECK(repaints == 2);
    CHECK_FALSE(fixture.document.cell(kBase).changed);
    CHECK(fixture.document.cell(kBase + 1).changed);

    // Once the value holds still the flag clears again, which repaints once.
    fixture.pass();
    CHECK(repaints == 3);
    CHECK_FALSE(fixture.document.cell(kBase + 1).changed);
    fixture.pass();
    CHECK(repaints == 3);

    // In a wider format a cell is flagged when any byte it covers changed.
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int64, .hex = true});
    fixture.pass();
    fixture.access.memory->bytes[kBase + 3] = std::byte {0x33};
    fixture.pass();
    CHECK(fixture.document.cell(kBase).changed);
}

TEST_CASE("the memory view document drops stale and foreign readings", "[ui]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0x11});
    [[maybe_unused]] const auto submitted = fixture.document.next_live_request(); // Records the submitted bases.

    // The window moves before the pass lands: the reading is dropped.
    fixture.document.set_view(kBase + kExtent, kRowBytes, kRows);
    fixture.document.apply_live_readings(
        std::vector<slopkit::ui::LiveReading> {reading(1, *fixture.access.memory, kBase, kExtent)});
    CHECK_FALSE(fixture.document.cell(kBase).readable);

    // A reading from another target is dropped as well.
    [[maybe_unused]] const auto second = fixture.document.next_live_request();
    fixture.target.pid                 = 7;
    fixture.document.apply_live_readings(
        std::vector<slopkit::ui::LiveReading> {reading(1, *fixture.access.memory, kBase + kExtent, kExtent)});
    CHECK_FALSE(fixture.document.cell(kBase + kExtent).readable);
    fixture.target.pid = 42;

    // An unrequested slot is ignored.
    [[maybe_unused]] const auto third = fixture.document.next_live_request();
    fixture.document.apply_live_readings(
        std::vector<slopkit::ui::LiveReading> {reading(7, *fixture.access.memory, kBase + kExtent, kExtent)});
    CHECK_FALSE(fixture.document.cell(kBase + kExtent).readable);
}

TEST_CASE("the memory view document renders each value format", "[ui]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0});
    fixture.access.memory->bytes[kBase]      = std::byte {0x41};
    fixture.access.memory->bytes[kBase + 16] = std::byte {0x00};
    fixture.access.memory->bytes[kBase + 17] = std::byte {0x00};
    fixture.access.memory->bytes[kBase + 18] = std::byte {0x80};
    fixture.access.memory->bytes[kBase + 19] = std::byte {0x3F}; // 1.0f
    fixture.pass();

    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::byte, .hex = true});
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("41"));
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::byte, .hex = false});
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("65"));

    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int16, .hex = true});
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("0041"));

    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = false});
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("65"));

    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int64, .hex = true});
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("0000000000000041"));

    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::float32, .hex = false});
    CHECK(fixture.document.cell(kBase + 16).text == QStringLiteral("1"));

    // A cell that cannot cover the format's bytes is unreadable.
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = true});
    CHECK_FALSE(fixture.document.cell(kBase + kExtent - 2).readable);
}

TEST_CASE("the memory view document decodes the row for each text encoding", "[ui]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0});
    fixture.access.memory->bytes[kBase]      = std::byte {0x48}; // 'H'
    fixture.access.memory->bytes[kBase + 1]  = std::byte {0x69}; // 'i'
    fixture.access.memory->bytes[kBase + 2]  = std::byte {0xC3}; // U+00E9 in UTF-8
    fixture.access.memory->bytes[kBase + 3]  = std::byte {0xA9};
    fixture.access.memory->bytes[kBase + 16] = std::byte {0x41}; // 'A' as a UTF-16 code unit
    fixture.access.memory->bytes[kBase + 17] = std::byte {0x00};
    fixture.access.memory->bytes[kBase + 18] = std::byte {0x42}; // 'B'
    fixture.access.memory->bytes[kBase + 19] = std::byte {0x00};
    fixture.pass();

    fixture.document.set_encoding(TextEncoding::ascii);
    CHECK(fixture.document.text_row(kBase) == QStringLiteral("Hi.............."));

    fixture.document.set_encoding(TextEncoding::utf8);
    CHECK(fixture.document.text_row(kBase).startsWith(QString::fromUtf8("Hi\xC3\xA9")));

    fixture.document.set_encoding(TextEncoding::utf16);
    const QString utf16 = fixture.document.text_row(kBase + 16);
    REQUIRE(utf16.size() >= 2);
    CHECK(utf16.at(0) == QLatin1Char('A'));
    CHECK(utf16.at(1) == QLatin1Char('B'));

    fixture.document.set_encoding(TextEncoding::ascii);
    CHECK(fixture.document.text_row(kBase + 16).startsWith(QStringLiteral("A.B.")));
}

TEST_CASE("the memory view document renders unreadable bytes as placeholders", "[ui]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0xAB});

    // Nothing has been read for the window yet, so the cell stays blank: "not
    // read yet" is not the same as "could not be read".
    CHECK(fixture.document.cell(kBase).text.isEmpty());

    const auto fail_visible = [&fixture]
    {
        slopkit::ui::LiveReading failed;
        failed.id                             = 1;
        failed.readable                       = false;
        [[maybe_unused]] const auto submitted = fixture.document.next_live_request();
        fixture.document.apply_live_readings(std::vector<slopkit::ui::LiveReading> {failed});
    };

    // A failed read gives the block its submitted buffer and paints `??`.
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::byte, .hex = true});
    fail_visible();
    CHECK_FALSE(fixture.document.cell(kBase).readable);
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("??"));

    // A wider hex format shows two `?` per unreadable byte, matching the
    // readable cell's width so columns stay aligned.
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = true});
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("????????"));
    CHECK(fixture.document.cell(kBase).text.size() == 8);

    // A decimal format collapses to a single `?`.
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = false});
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("?"));

    // A good pass replaces the placeholders with the value.
    fixture.access.unreadable->clear();
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::byte, .hex = true});
    fixture.pass();
    CHECK(fixture.document.cell(kBase).readable);
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("AB"));
}

TEST_CASE("the memory view document writes through the access worker", "[ui]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0x11});
    fixture.pass();
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::byte, .hex = true});

    std::vector<slopkit::log::Record> records;
    SinkGuard                         sink {[&records](const slopkit::log::Record& record)
                                            {
                        records.push_back(record);
                                            }};

    CHECK(fixture.document.write_value(kBase, QStringLiteral("0xEF")));
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return fixture.access.memory->bytes.at(kBase) == std::byte {0xEF};
                        }));
    REQUIRE_FALSE(records.empty());
    CHECK(records.back().level == slopkit::log::Level::info);
    CHECK(records.back().message.find("memory view wrote") != std::string::npos);

    // The next identical reading does not flash the written byte as changed.
    fixture.pass();
    CHECK_FALSE(fixture.document.cell(kBase).changed);
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("EF"));

    // A malformed value is rejected without touching memory, with one record.
    records.clear();
    CHECK_FALSE(fixture.document.write_value(kBase, QStringLiteral("not a number")));
    REQUIRE(records.size() == 1);
    CHECK(records.front().level == slopkit::log::Level::warning);
    CHECK(records.front().message.find("memory view value rejected") != std::string::npos);
    CHECK(fixture.access.memory->bytes.at(kBase) == std::byte {0xEF});

    // Only one write may be in flight.
    CHECK(fixture.document.write_value(kBase, QStringLiteral("0x01")));
    records.clear();
    CHECK_FALSE(fixture.document.write_value(kBase, QStringLiteral("0x02")));
    REQUIRE(records.size() == 1);
    CHECK(records.front().message == "memory view write refused: a write is already in progress");
    pump_worker(fixture.worker,
                [&]
                {
                    return fixture.access.memory->bytes.at(kBase) == std::byte {0x01};
                });

    // A detached target refuses the write with one warning record.
    records.clear();
    fixture.target.session_live = false;
    CHECK_FALSE(fixture.document.write_value(kBase, QStringLiteral("0x03")));
    REQUIRE(records.size() == 1);
    CHECK(records.front().level == slopkit::log::Level::warning);
    CHECK(records.front().message == "memory view write refused: no target attached");
}

TEST_CASE("the memory view document clamps the window at address zero and the user-space ceiling", "[ui]")
{
    Fixture fixture;
    fixture.document.set_view(0, kRowBytes, kRows);
    const auto at_zero = fixture.document.next_live_request();
    REQUIRE(at_zero.size() == 2);
    CHECK(at_zero[0].id == 1);
    CHECK(at_zero[0].address == 0);
    CHECK(at_zero[1].id == 2);
    CHECK(at_zero[1].address == kExtent);

    fixture.document.set_view(slopkit::scan::kMaxUserAddress - 8, kRowBytes, 1);
    for (const slopkit::ui::LiveRequest& request : fixture.document.next_live_request())
    {
        CHECK(request.address <= slopkit::scan::kMaxUserAddress);
        CHECK(request.address + request.size - 1 <= slopkit::scan::kMaxUserAddress);
    }
}

TEST_CASE("the memory view document renders static rows as module+RVA", "[ui]")
{
    application();

    FakeAccess                                  access;
    slopkit::process::AccessWorker              worker {access};
    slopkit::process::AttachedTarget            target = fake_target();
    slopkit::ui::components::MemoryViewDocument document {worker, target};
    document.set_view(0x1000, 16, 24);

    // Without a module map the text keeps the 16-digit padded form.
    CHECK(document.address_text(0x1010) == QStringLiteral("0000000000001010"));

    document.set_modules({module_image("app", 0x1000, 0x1000)});
    CHECK(document.address_text(0x1000) == QStringLiteral("app+0"));
    CHECK(document.address_text(0x1010) == QStringLiteral("app+10"));

    // Absolute mode restores the padded text.
    document.set_address_mode(slopkit::ui::AddressMode::absolute);
    CHECK(document.address_text(0x1010) == QStringLiteral("0000000000001010"));

    // An address outside every span stays absolute in both modes.
    document.set_address_mode(slopkit::ui::AddressMode::module_relative);
    CHECK(document.address_text(0x5000010) == QStringLiteral("0000000005000010"));
}
