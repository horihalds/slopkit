#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "scan/types.hpp"
#include "ui/components/memory_view.hpp"
#include "ui/components/memory_view_document.hpp"
#include "ui/live_values.hpp"

// Defined in test_main.cpp; one QApplication is shared by every test file.
QApplication& slopkit_test_application();

namespace
{
    using slopkit::ui::components::MemoryView;
    using slopkit::ui::components::MemoryViewDocument;
    using slopkit::ui::components::TextEncoding;
    using slopkit::ui::components::ValueFormat;

    // Address-keyed single bytes the fake target serves; writes land here too.
    using FakeMemory = std::map<std::uint64_t, std::byte>;

    // A tiny in-memory target with write-back, so a write can be observed.
    class FakeBackend final : public slopkit::process::SessionBackend
    {
    public:
        std::shared_ptr<FakeMemory>              memory     = std::make_shared<FakeMemory>();
        std::shared_ptr<std::set<std::uint64_t>> unreadable = std::make_shared<std::set<std::uint64_t>>();

        [[nodiscard]] slopkit::process::ProcessId pid() const noexcept override
        {
            return 42;
        }

        [[nodiscard]] std::string_view plugin_id() const noexcept override
        {
            return "fake";
        }

        [[nodiscard]] slopkit::process::AccessMethod advertised_methods() const noexcept override
        {
            return slopkit::process::AccessMethod::procfs_mem;
        }

        [[nodiscard]] slopkit::process::AccessMethod last_method() const noexcept override
        {
            return slopkit::process::AccessMethod::procfs_mem;
        }

        std::expected<std::vector<std::byte>, slopkit::process::AccessError> read(std::uint64_t address,
                                                                                  std::size_t   size) override
        {
            if (unreadable->contains(address))
            {
                return std::unexpected(slopkit::process::AccessError::not_found);
            }
            std::vector<std::byte> bytes(size, std::byte {0});
            for (std::size_t index = 0; index < size; ++index)
            {
                if (const auto entry = memory->find(address + index); entry != memory->end())
                {
                    bytes[index] = entry->second;
                }
            }
            return bytes;
        }

        std::expected<std::size_t, slopkit::process::AccessError> write(std::uint64_t              address,
                                                                        std::span<const std::byte> data) override
        {
            for (std::size_t index = 0; index < data.size(); ++index)
            {
                (*memory)[address + index] = data[index];
            }
            return data.size();
        }

        std::expected<std::vector<slopkit::process::ModuleInfo>, slopkit::process::AccessError> modules() override
        {
            return std::vector<slopkit::process::ModuleInfo> {};
        }

        std::expected<std::vector<slopkit::process::ThreadInfo>, slopkit::process::AccessError> threads() override
        {
            return std::vector<slopkit::process::ThreadInfo> {};
        }

        std::expected<std::vector<slopkit::process::RegionInfo>, slopkit::process::AccessError> regions() override
        {
            return std::vector<slopkit::process::RegionInfo> {};
        }
    };

    class FakeAccess final : public slopkit::process::ProcessAccess
    {
    public:
        std::shared_ptr<FakeMemory>              memory     = std::make_shared<FakeMemory>();
        std::shared_ptr<std::set<std::uint64_t>> unreadable = std::make_shared<std::set<std::uint64_t>>();

        std::expected<std::vector<slopkit::process::ProcessInfo>, slopkit::process::AccessError>
        list_processes() override
        {
            return std::vector<slopkit::process::ProcessInfo> {};
        }

        std::expected<slopkit::process::Session, slopkit::process::AccessError> attach(slopkit::process::ProcessId,
                                                                                       std::string_view) override
        {
            auto backend        = std::make_unique<FakeBackend>();
            backend->memory     = memory;
            backend->unreadable = unreadable;
            return slopkit::process::Session {std::move(backend)};
        }
    };

    constexpr std::size_t   kRowBytes = 16;
    constexpr std::size_t   kRows     = 24;
    constexpr std::uint64_t kExtent   = kRowBytes * kRows;
    constexpr std::uint64_t kBase     = 0x1800; // A multiple of kExtent.

    template<typename Predicate>
    bool pump_worker(slopkit::process::AccessWorker& worker, Predicate done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            worker.drain();
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }

    void attach_session(slopkit::process::AccessWorker& worker)
    {
        bool attached = false;
        worker.submit_attach_app(worker.next_job_id(),
                                 42,
                                 "fake",
                                 [&](slopkit::process::JobResult&&)
                                 {
                                     attached = true;
                                 });
        REQUIRE(pump_worker(worker,
                            [&]
                            {
                                return attached;
                            }));
    }

    slopkit::process::AttachedTarget attached_target()
    {
        slopkit::process::AttachedTarget target;
        target.pid          = 42;
        target.session_live = true;
        target.method       = slopkit::process::AccessMethod::procfs_mem;
        return target;
    }

    void fill(FakeMemory& memory, std::uint64_t base, std::uint64_t size, std::byte value)
    {
        for (std::uint64_t index = 0; index < size; ++index)
        {
            memory[base + index] = value;
        }
    }

    std::vector<std::byte> block_bytes(const FakeMemory& memory, std::uint64_t base, std::size_t size)
    {
        std::vector<std::byte> bytes(size, std::byte {0});
        for (std::size_t index = 0; index < size; ++index)
        {
            if (const auto entry = memory.find(base + index); entry != memory.end())
            {
                bytes[index] = entry->second;
            }
        }
        return bytes;
    }

    slopkit::ui::LiveReading reading(std::size_t id, const FakeMemory& memory, std::uint64_t base, std::size_t size)
    {
        slopkit::ui::LiveReading value;
        value.id       = id;
        value.readable = true;
        value.bytes    = block_bytes(memory, base, size);
        return value;
    }

    // A document with a live target attached, its window parked on kBase.
    struct Fixture
    {
        FakeAccess                       access;
        slopkit::process::AccessWorker   worker {access};
        slopkit::process::AttachedTarget target = attached_target();
        MemoryViewDocument               document {worker, target};

        Fixture()
        {
            attach_session(worker);
            document.set_visible(true);
            document.set_view(kBase, kRowBytes, kRows);
        }

        // Refills the visible window's three blocks from memory and applies them.
        void pass()
        {
            const auto                            requests = document.next_live_request();
            std::vector<slopkit::ui::LiveReading> readings;
            readings.reserve(requests.size());
            for (const slopkit::ui::LiveRequest& request : requests)
            {
                readings.push_back(reading(request.id, *access.memory, request.address, request.size));
            }
            document.apply_live_readings(readings);
        }
    };
} // namespace

TEST_CASE("the memory view document windows the visible block and its neighbours", "[memory_view]")
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

TEST_CASE("scrolling inside a block keeps the request set and reuses the cache", "[memory_view]")
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

TEST_CASE("the memory view document marks only bytes that differ from the last reading", "[memory_view]")
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
    (*fixture.access.memory)[kBase + 1] = std::byte {0x22};
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
    (*fixture.access.memory)[kBase + 3] = std::byte {0x33};
    fixture.pass();
    CHECK(fixture.document.cell(kBase).changed);
}

TEST_CASE("the memory view document drops stale and foreign readings", "[memory_view]")
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

TEST_CASE("the memory view document renders each value format", "[memory_view]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0});
    (*fixture.access.memory)[kBase]      = std::byte {0x41};
    (*fixture.access.memory)[kBase + 16] = std::byte {0x00};
    (*fixture.access.memory)[kBase + 17] = std::byte {0x00};
    (*fixture.access.memory)[kBase + 18] = std::byte {0x80};
    (*fixture.access.memory)[kBase + 19] = std::byte {0x3F}; // 1.0f
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

TEST_CASE("the memory view document decodes the row for each text encoding", "[memory_view]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0});
    (*fixture.access.memory)[kBase]      = std::byte {0x48}; // 'H'
    (*fixture.access.memory)[kBase + 1]  = std::byte {0x69}; // 'i'
    (*fixture.access.memory)[kBase + 2]  = std::byte {0xC3}; // U+00E9 in UTF-8
    (*fixture.access.memory)[kBase + 3]  = std::byte {0xA9};
    (*fixture.access.memory)[kBase + 16] = std::byte {0x41}; // 'A' as a UTF-16 code unit
    (*fixture.access.memory)[kBase + 17] = std::byte {0x00};
    (*fixture.access.memory)[kBase + 18] = std::byte {0x42}; // 'B'
    (*fixture.access.memory)[kBase + 19] = std::byte {0x00};
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

TEST_CASE("the memory view document reports unreadable bytes", "[memory_view]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0x11});
    fixture.pass();
    CHECK_FALSE(fixture.document.any_unreadable());

    slopkit::ui::LiveReading failed;
    failed.id                             = 1;
    failed.readable                       = false;
    [[maybe_unused]] const auto submitted = fixture.document.next_live_request();
    fixture.document.apply_live_readings(std::vector<slopkit::ui::LiveReading> {failed});

    CHECK(fixture.document.any_unreadable());
    CHECK_FALSE(fixture.document.cell(kBase).readable);
    CHECK(fixture.document.cell(kBase).text.isEmpty());
}

TEST_CASE("the memory view document writes through the access worker", "[memory_view]")
{
    Fixture fixture;
    fill(*fixture.access.memory, kBase - kExtent, 3 * kExtent, std::byte {0x11});
    fixture.pass();
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::byte, .hex = true});

    QString message;
    bool    is_error = false;
    QObject::connect(&fixture.document,
                     &MemoryViewDocument::statusChanged,
                     [&](const QString& text, bool error)
                     {
                         message  = text;
                         is_error = error;
                     });

    CHECK(fixture.document.write_value(kBase, QStringLiteral("0xEF")));
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return fixture.access.memory->at(kBase) == std::byte {0xEF};
                        }));
    CHECK_FALSE(is_error);

    // The next identical reading does not flash the written byte as changed.
    fixture.pass();
    CHECK_FALSE(fixture.document.cell(kBase).changed);
    CHECK(fixture.document.cell(kBase).text == QStringLiteral("EF"));

    // A malformed value is rejected without touching memory.
    CHECK_FALSE(fixture.document.write_value(kBase, QStringLiteral("not a number")));
    CHECK(is_error);
    CHECK(fixture.access.memory->at(kBase) == std::byte {0xEF});

    // Only one write may be in flight.
    CHECK(fixture.document.write_value(kBase, QStringLiteral("0x01")));
    CHECK_FALSE(fixture.document.write_value(kBase, QStringLiteral("0x02")));
    pump_worker(fixture.worker,
                [&]
                {
                    return fixture.access.memory->at(kBase) == std::byte {0x01};
                });

    // A detached target refuses the write with the expected message.
    fixture.target.session_live = false;
    CHECK_FALSE(fixture.document.write_value(kBase, QStringLiteral("0x03")));
    CHECK(message == QStringLiteral("Not attached; cannot write."));
}

TEST_CASE("the memory view document clamps the window at address zero and the user-space ceiling", "[memory_view]")
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

TEST_CASE("the memory view auto-fits its rows to the widget", "[memory_view]")
{
    slopkit_test_application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();

    const std::size_t wide = view.bytes_per_row();
    REQUIRE(wide > 0);
    CHECK(view.horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);

    // A narrower widget fits fewer bytes per row.
    view.resize(360, 600);
    const std::size_t narrow = view.bytes_per_row();
    REQUIRE(narrow > 0);
    CHECK(narrow <= wide);

    // Hiding the text column gives its space back to the value cells.
    const std::size_t with_text = view.bytes_per_row();
    view.set_text_column_visible(false);
    CHECK_FALSE(view.text_column_visible());
    CHECK(view.bytes_per_row() >= with_text);

    // The row width is always a whole number of format units.
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = true});
    view.relayout();
    CHECK(view.bytes_per_row() % 4 == 0);

    view.hide();
}

TEST_CASE("a resize and a format change keep the previously top-most byte on screen", "[memory_view]")
{
    slopkit_test_application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.set_first_byte(kBase);
    const std::uint64_t before = view.first_byte();
    REQUIRE(before <= kBase);

    view.resize(360, 600);
    const std::uint64_t after = view.first_byte();
    CHECK(after <= before);
    CHECK(before < after + view.bytes_per_row());

    const std::uint64_t anchor = view.first_byte();
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = true});
    view.relayout();
    CHECK(view.bytes_per_row() % 4 == 0);
    CHECK(view.first_byte() <= anchor);
    CHECK(anchor < view.first_byte() + view.bytes_per_row());

    view.hide();
}

TEST_CASE("the memory view scrolls by whole rows and clamps at zero", "[memory_view]")
{
    slopkit_test_application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.set_first_byte(0x100000);
    const std::size_t   bpr   = view.bytes_per_row();
    const std::uint64_t start = view.first_byte();

    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(&view, &down);
    CHECK(view.first_byte() == start + bpr);

    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(&view, &up);
    CHECK(view.first_byte() == start);

    QKeyEvent page_down(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
    QApplication::sendEvent(&view, &page_down);
    CHECK(view.first_byte() == start + view.visible_rows() * bpr);

    // Scrolling up at the start of the address space clamps at zero.
    view.set_first_byte(0);
    QApplication::sendEvent(&view, &up);
    CHECK(view.first_byte() == 0);

    // The wheel scrolls three rows per notch.
    view.set_first_byte(start);
    const QPointF centre(view.viewport()->rect().center());
    QWheelEvent   wheel_down(centre,
                             QPointF(view.viewport()->mapToGlobal(centre.toPoint())),
                             QPoint(),
                             QPoint(0, -120),
                             Qt::NoButton,
                             Qt::NoModifier,
                             Qt::NoScrollPhase,
                             false);
    QApplication::sendEvent(view.viewport(), &wheel_down);
    CHECK(view.first_byte() == start + 3 * bpr);

    view.hide();
}

TEST_CASE("the memory view edits a cell inline", "[memory_view]")
{
    slopkit_test_application();
    Fixture fixture;
    fill(*fixture.access.memory, kBase - 0x1000, 0x3000, std::byte {0x11});

    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();
    view.set_first_byte(kBase);
    fixture.pass(); // Fill the window the view just asked for.

    const std::uint64_t        cell_address = view.first_byte();
    const std::optional<QRect> cell         = view.cell_rect(cell_address);
    REQUIRE(cell.has_value());
    CHECK(view.editor() == nullptr);

    const auto double_click_at = [&view](const QPoint& point)
    {
        QMouseEvent event(QEvent::MouseButtonDblClick,
                          QPointF(point),
                          QPointF(view.viewport()->mapToGlobal(point)),
                          Qt::LeftButton,
                          Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &event);
    };

    double_click_at(cell->center());
    REQUIRE(view.editor() != nullptr);
    CHECK(view.editor()->isVisible());
    CHECK(view.editor()->text() == QStringLiteral("11"));
    CHECK(view.editor()->geometry() == *cell);

    view.editor()->setText(QStringLiteral("0xEF"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(view.editor(), &enter);
    CHECK_FALSE(view.editor()->isVisible());
    REQUIRE(pump_worker(fixture.worker,
                        [&]
                        {
                            return fixture.access.memory->at(cell_address) == std::byte {0xEF};
                        }));

    // Escape cancels the second edit without writing.
    double_click_at(cell->center());
    REQUIRE(view.editor()->isVisible());
    view.editor()->setText(QStringLiteral("0x01"));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(view.editor(), &escape);
    CHECK_FALSE(view.editor()->isVisible());
    CHECK(fixture.access.memory->at(cell_address) == std::byte {0xEF});

    view.hide();
}

TEST_CASE("the memory view options menu switches the display", "[memory_view]")
{
    slopkit_test_application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();

    QMenu menu;
    view.populate_options_menu(menu);

    const auto submenu = [](QMenu* parent, const QString& title) -> QMenu*
    {
        for (QAction* action : parent->actions())
        {
            if (action->menu() != nullptr && action->menu()->title() == title)
            {
                return action->menu();
            }
        }
        return nullptr;
    };
    const auto action = [](QMenu* parent, const QString& text) -> QAction*
    {
        for (QAction* candidate : parent->actions())
        {
            if (candidate->text() == text)
            {
                return candidate;
            }
        }
        return nullptr;
    };

    QMenu* format_menu = submenu(&menu, QStringLiteral("Value format"));
    REQUIRE(format_menu != nullptr);
    for (const QString& type :
         {QStringLiteral("Byte"), QStringLiteral("2 Bytes"), QStringLiteral("4 Bytes"), QStringLiteral("8 Bytes")})
    {
        QMenu* type_menu = submenu(format_menu, type);
        REQUIRE(type_menu != nullptr);
        CHECK(action(type_menu, QStringLiteral("Hex")) != nullptr);
        CHECK(action(type_menu, QStringLiteral("Decimal")) != nullptr);
    }
    CHECK(action(format_menu, QStringLiteral("Float")) != nullptr);
    CHECK(action(format_menu, QStringLiteral("Double")) != nullptr);

    // The active format and encoding are checked.
    QMenu* byte_menu = submenu(format_menu, QStringLiteral("Byte"));
    REQUIRE(byte_menu != nullptr);
    CHECK(action(byte_menu, QStringLiteral("Hex"))->isChecked());

    QMenu* encoding_menu = submenu(&menu, QStringLiteral("Text encoding"));
    REQUIRE(encoding_menu != nullptr);
    CHECK(action(encoding_menu, QStringLiteral("ASCII"))->isChecked());
    CHECK(action(encoding_menu, QStringLiteral("UTF-8"))->isCheckable());
    CHECK(action(encoding_menu, QStringLiteral("UTF-16"))->isCheckable());

    QAction* show_text = action(&menu, QStringLiteral("Show text column"));
    REQUIRE(show_text != nullptr);
    CHECK(show_text->isCheckable());
    CHECK(show_text->isChecked());

    // The top entry asks the dialog to prompt for an address.
    QAction* go_to = action(&menu, QStringLiteral("Go To..."));
    REQUIRE(go_to != nullptr);
    bool asked = false;
    QObject::connect(&view,
                     &MemoryView::gotoRequested,
                     &view,
                     [&asked]
                     {
                         asked = true;
                     });
    go_to->trigger();
    CHECK(asked);

    // Choosing 4 Bytes (hex) re-lays out and keeps the top byte on screen.
    const std::uint64_t anchor = view.first_byte();
    action(submenu(format_menu, QStringLiteral("4 Bytes")), QStringLiteral("Hex"))->trigger();
    CHECK(fixture.document.format().type == slopkit::scan::ValueType::int32);
    CHECK(fixture.document.format().hex);
    CHECK(view.bytes_per_row() % 4 == 0);
    CHECK(view.first_byte() <= anchor);
    CHECK(anchor < view.first_byte() + view.bytes_per_row());

    action(encoding_menu, QStringLiteral("UTF-8"))->trigger();
    CHECK(fixture.document.encoding() == TextEncoding::utf8);

    const std::size_t with_text = view.bytes_per_row();
    show_text->trigger();
    CHECK_FALSE(view.text_column_visible());
    CHECK(view.bytes_per_row() >= with_text);

    view.hide();
}

TEST_CASE("the memory view header labels each column with its offset", "[memory_view]")
{
    slopkit_test_application();
    Fixture    fixture;
    MemoryView view(fixture.document);
    view.resize(800, 600);
    view.show();

    // A byte view: two hex digits per column, no `0x` prefix.
    REQUIRE(view.bytes_per_row() >= 4);
    CHECK(view.column_offset_text(0) == QStringLiteral("00"));
    CHECK(view.column_offset_text(1) == QStringLiteral("01"));
    CHECK(view.column_offset_text(view.bytes_per_row() - 1).size() == 2);

    // A 4-byte view steps by four.
    fixture.document.set_format(ValueFormat {.type = slopkit::scan::ValueType::int32, .hex = true});
    view.relayout();
    REQUIRE(view.bytes_per_row() >= 8);
    CHECK(view.column_offset_text(0) == QStringLiteral("00"));
    CHECK(view.column_offset_text(1) == QStringLiteral("04"));
    CHECK(view.column_offset_text(2) == QStringLiteral("08"));

    view.hide();
}
