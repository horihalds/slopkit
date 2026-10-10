#pragma once

#include <catch2/catch.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
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

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "debug/controller.hpp"
#include "plugin/plugin_host.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/plugin_access.hpp"
#include "process/types.hpp"
#include "scan/source.hpp"
#include "support/fake_debug.hpp"
#include "support/fake_process.hpp"
#include "table/serializer.hpp"
#include "ui/address_format.hpp"
#include "ui/components/input_box.hpp"
#include "ui/components/memory_view.hpp"
#include "ui/components/memory_view_document.hpp"
#include "ui/components/message_box.hpp"
#include "ui/components/widgets.hpp"
#include "ui/dialogs/access_watch.hpp"
#include "ui/dialogs/add_address.hpp"
#include "ui/dialogs/breakpoints.hpp"
#include "ui/dialogs/log.hpp"
#include "ui/dialogs/memory_viewer.hpp"
#include "ui/dialogs/process_list.hpp"
#include "ui/dialogs/settings.hpp"
#include "ui/dialogs/table_conflict.hpp"
#include "ui/dialogs/table_settings.hpp"
#include "ui/main_window.hpp"
#include "ui/models/address_table_model.hpp"
#include "ui/models/found_results_model.hpp"
#include "ui/panels/address_list_panel.hpp"
#include "ui/panels/found_list_panel.hpp"
#include "ui/panels/scanner_panel.hpp"
#include "ui/settings.hpp"
#include "ui/theme.hpp"

namespace
{
    using slopkit::test::application;
    using slopkit::test::fake_target;
    using slopkit::test::FakeAccess;
    using slopkit::test::FakeBackend;
    using slopkit::test::FakeMemory;
    using slopkit::test::module_image;
    using slopkit::test::pump_ui;
    using slopkit::test::sample_processes;
    using slopkit::test::scratch_settings_file;

    [[maybe_unused]] void check_all_roles_defined(const slopkit::ui::Theme& theme)
    {
        const QColor roles[] = {
            theme.background,     theme.surface,         theme.surface_hover,    theme.text,
            theme.text_muted,     theme.accent,          theme.accent_hover,     theme.accent_active,
            theme.on_accent,      theme.border,          theme.success,          theme.warning,
            theme.error,          theme.syntax_register, theme.syntax_immediate, theme.syntax_module,
            theme.syntax_keyword, theme.syntax_string,   theme.syntax_comment,   theme.syntax_number};
        for (const QColor& role : roles)
        {
            CHECK(role.isValid());
        }
    }

    [[maybe_unused]] QList<QString> action_texts(const QList<QAction*>& actions)
    {
        QList<QString> texts;
        for (QAction* action : actions)
        {
            if (!action->isSeparator())
            {
                texts.append(action->text());
            }
        }
        return texts;
    }

    // A fresh settings file for this suite's cases.
    [[maybe_unused]] QString scratch_settings_file(std::string_view name)
    {
        return slopkit::test::scratch_settings_file("ui_test", name);
    }

    // The scanner panel's Start/Stop fields, located by their placeholder text.
    [[maybe_unused]] QLineEdit* address_field(QWidget& panel, const char* placeholder)
    {
        for (auto* edit : panel.findChildren<QLineEdit*>())
        {
            if (edit->placeholderText() == QString::fromUtf8(placeholder))
            {
                return edit;
            }
        }
        return nullptr;
    }

    // Attaches the app-wide session to the fake backend on pid 42.
    [[maybe_unused]] void attach_app_session(slopkit::process::AccessWorker& worker)
    {
        bool attached = false;
        worker.submit_attach_app(worker.next_job_id(),
                                 42,
                                 "fake",
                                 [&](slopkit::process::JobResult&&)
                                 {
                                     attached = true;
                                 });
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return attached;
                        }));
    }

    // The UI fixtures hand a debug controller to the Memory Viewer and the Main
    // Window. The existing suites never start a session, so one shared
    // controller per translation unit is enough; it is created on first use.
    [[maybe_unused]] slopkit::debug::Controller& shared_debug_controller()
    {
        static slopkit::tests::FakeDebugBackend backend;
        static slopkit::debug::Controller       controller {backend};
        return controller;
    }

    // Writes a `.skt` table carrying only the given settings.
    [[maybe_unused]] void write_table_with_settings(const QString&     path,
                                                    const std::string& target,
                                                    bool               auto_attach,
                                                    bool               match_exe_path,
                                                    const std::string& exe_path = {})
    {
        slopkit::table::AddressTable table;
        table.settings().target_process = target;
        table.settings().exe_path       = exe_path;
        table.settings().auto_attach    = auto_attach;
        table.settings().match_exe_path = match_exe_path;
        REQUIRE(slopkit::table::save(std::filesystem::path(path.toStdString()), table).has_value());
    }

    // Writes a `.skt` table with one described int32 row and an optional target.
    [[maybe_unused]] void write_entry_table(const QString&     path,
                                            const QString&     description,
                                            std::uint64_t      address,
                                            const std::string& target = {})
    {
        slopkit::table::AddressTable table;
        table.settings().target_process = target;
        slopkit::table::AddressEntry entry;
        entry.address     = address;
        entry.description = description.toStdString();
        entry.type        = slopkit::scan::ValueType::int32;
        entry.bytes       = {std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}};
        table.add(entry);
        REQUIRE(slopkit::table::save(std::filesystem::path(path.toStdString()), table).has_value());
    }

    // The scan-range dropdown, located by its tooltip.
    [[maybe_unused]] QComboBox* range_combo(QWidget& panel)
    {
        for (auto* combo : panel.findChildren<QComboBox*>())
        {
            if (combo->toolTip() == QStringLiteral("Whole process or a single loaded module"))
            {
                return combo;
            }
        }
        return nullptr;
    }

    // The nearest widgets::Panel ancestor of a widget, or nullptr.
    [[maybe_unused]] QWidget* ancestor_panel(QWidget* widget)
    {
        for (auto* parent = widget->parentWidget(); parent != nullptr; parent = parent->parentWidget())
        {
            if (qobject_cast<slopkit::ui::widgets::Panel*>(parent) != nullptr)
            {
                return parent;
            }
        }
        return nullptr;
    }

    [[maybe_unused]] bool panel_has_title(QWidget* panel, const QString& title)
    {
        for (auto* label : panel->findChildren<QLabel*>())
        {
            if (label->text() == title)
            {
                return true;
            }
        }
        return false;
    }

    [[maybe_unused]] QCheckBox* checkbox_labelled(QWidget& root, const QString& text)
    {
        for (auto* box : root.findChildren<QCheckBox*>())
        {
            if (box->text() == text)
            {
                return box;
            }
        }
        return nullptr;
    }

    [[maybe_unused]] QPushButton* button_labelled(QWidget& root, const QString& text)
    {
        for (auto* button : root.findChildren<QPushButton*>())
        {
            if (button->text() == text)
            {
                return button;
            }
        }
        return nullptr;
    }

    // The position of `widget` among the window's focusable controls in focus-
    // chain order, or -1 when it is not in the window's chain. Non-focusable
    // widgets are skipped because Tab skips them too, and the walk stops when
    // the ring wraps back to the window, so a broken chain cannot hang.
    int focusable_chain_index(QWidget& window, const QWidget* widget)
    {
        int index = 0;
        for (const QWidget* current = window.nextInFocusChain(); current != &window;
             current                = current->nextInFocusChain())
        {
            if (current->focusPolicy() == Qt::NoFocus)
            {
                continue;
            }
            if (current == widget)
            {
                return index;
            }
            ++index;
        }
        return -1;
    }

    // Asserts that both widgets are in `window`'s focus chain and that no other
    // focusable control sits between them, i.e. Tab reaches `after` from `before`.
    [[maybe_unused]] void check_tab_order(QWidget& window, const QWidget* before, const QWidget* after)
    {
        const int before_index = focusable_chain_index(window, before);
        const int after_index  = focusable_chain_index(window, after);
        CHECK(before_index >= 0);
        CHECK(after_index >= 0);
        CHECK(before_index + 1 == after_index);
    }

    [[maybe_unused]] QRadioButton* radio_labelled(QWidget& root, const QString& text)
    {
        for (auto* button : root.findChildren<QRadioButton*>())
        {
            if (button->text() == text)
            {
                return button;
            }
        }
        return nullptr;
    }

    // Restores the process-wide log level on scope exit.
    class LevelGuard
    {
    public:
        LevelGuard() : previous_(slopkit::log::Logger::instance().minimum_level()) {}

        LevelGuard(const LevelGuard&)            = delete;
        LevelGuard& operator=(const LevelGuard&) = delete;

        ~LevelGuard()
        {
            slopkit::log::Logger::instance().set_minimum_level(previous_);
        }

    private:
        slopkit::log::Level previous_;
    };

    // Registers a sink for the lifetime of the guard.
    class SinkGuard
    {
    public:
        explicit SinkGuard(slopkit::log::Sink sink) : id_(slopkit::log::Logger::instance().add_sink(std::move(sink))) {}

        SinkGuard(const SinkGuard&)            = delete;
        SinkGuard& operator=(const SinkGuard&) = delete;

        ~SinkGuard()
        {
            slopkit::log::Logger::instance().remove_sink(id_);
        }

    private:
        slopkit::log::SinkId id_;
    };

    // A live surface that records what one pass asks for and what it applies,
    // so the coordinator's cadence and gating can be asserted directly.
    class StubLiveSurface : public slopkit::ui::LiveSurface
    {
    public:
        std::vector<slopkit::ui::LiveRequest> requests;
        int                                   request_calls {0};
        int                                   apply_calls {0};
        std::vector<bool>                     readables;

        std::vector<slopkit::ui::LiveRequest> next_live_request() override
        {
            ++request_calls;
            return requests;
        }

        void apply_live_readings(std::span<const slopkit::ui::LiveReading> readings) override
        {
            ++apply_calls;
            readables.clear();
            for (const auto& reading : readings)
            {
                readables.push_back(reading.readable);
            }
        }
    };

    // A live ticker that records how often it was asked, with which interval,
    // and whether it claimed to have submitted, so the coordinator's cadence and
    // gating can be asserted directly.
    class StubLiveTicker : public slopkit::ui::LiveTicker
    {
    public:
        int                       calls {0};
        int                       submits {0};
        bool                      submits_now {false};
        std::chrono::milliseconds last_interval {0};

        bool tick(std::chrono::milliseconds interval) override
        {
            ++calls;
            last_interval = interval;
            if (submits_now)
            {
                ++submits;
                return true;
            }
            return false;
        }
    };

    // Adds an int32 entry with the given stored bytes.
    [[maybe_unused]] void
    add_int32(slopkit::table::AddressTable& table, std::uint64_t address, std::byte stored = std::byte {1})
    {
        slopkit::table::AddressEntry entry;
        entry.address = address;
        entry.type    = slopkit::scan::ValueType::int32;
        entry.bytes   = {stored, std::byte {0}, std::byte {0}, std::byte {0}};
        table.add(entry);
    }

    // Wraps the found-results model as a surface: the model is not a widget and
    // the whole panel/engine is not needed to exercise the live path.
    class ModelLiveSurface : public slopkit::ui::LiveSurface
    {
    public:
        explicit ModelLiveSurface(slopkit::ui::models::FoundResultsModel& model) : model_(model) {}

        std::vector<slopkit::ui::LiveRequest> next_live_request() override
        {
            return model_.next_live_request();
        }

        void apply_live_readings(std::span<const slopkit::ui::LiveReading> readings) override
        {
            model_.apply_live_readings(readings);
        }

    private:
        slopkit::ui::models::FoundResultsModel& model_;
    };

    // A whole-list result set of 300 hits whose first 256 stored hits are all
    // heap hits, with one static hit at index 280, past the display page.
    [[maybe_unused]] std::shared_ptr<std::vector<slopkit::scan::ScanHit>> late_static_result()
    {
        auto whole = std::make_shared<std::vector<slopkit::scan::ScanHit>>();
        for (int i = 0; i < 300; ++i)
        {
            slopkit::scan::ScanHit hit;
            hit.address               = 0x100000 + static_cast<std::uint64_t>(i) * 4;
            // Big-endian so a byte-wise compare orders the values like the
            // integers they stand for.
            const std::uint32_t value = static_cast<std::uint32_t>(i);
            hit.value                 = {static_cast<std::byte>((value >> 24) & 0xFF),
                                         static_cast<std::byte>((value >> 16) & 0xFF),
                                         static_cast<std::byte>((value >> 8) & 0xFF),
                                         static_cast<std::byte>(value & 0xFF)};
            whole->push_back(std::move(hit));
        }
        (*whole)[280].address = 0x500000;
        return whole;
    }

    [[maybe_unused]] slopkit::scan::ScanSnapshot
    snapshot_over(const std::shared_ptr<const std::vector<slopkit::scan::ScanHit>>& whole)
    {
        slopkit::scan::ScanSnapshot snapshot;
        snapshot.hit_count = whole->size();
        snapshot.hits.assign(whole->begin(), whole->begin() + static_cast<std::ptrdiff_t>(slopkit::scan::kDisplayPage));
        snapshot.result_hits = whole;
        return snapshot;
    }
} // namespace
