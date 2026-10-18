#include <catch2/catch.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QModelIndex>
#include <QString>
#include <QStringList>

#include "scan/types.hpp"
#include "trainer/payload.hpp"
#include "trainer/runtime/gui/rows_model.hpp"
#include "trainer/runtime/gui/window.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/rows.hpp"
#include "trainer/runtime/scripts.hpp"

QApplication& slopkit_test_application();

namespace
{
    using slopkit::scan::ValueType;
    using slopkit::trainer::Action;
    using slopkit::trainer::ActionKind;
    using slopkit::trainer::build_rows;
    using slopkit::trainer::LiveEntry;
    using slopkit::trainer::Payload;
    using slopkit::trainer::PayloadEntry;
    using slopkit::trainer::PayloadScript;
    using slopkit::trainer::RowKind;
    using slopkit::trainer::RowView;
    using slopkit::trainer::ScriptState;
    using slopkit::trainer::gui::Options;
    using slopkit::trainer::gui::RowsModel;
    using slopkit::trainer::gui::TrainerWindow;

    std::vector<std::byte> int_bytes(std::uint32_t value)
    {
        std::vector<std::byte> bytes(4);
        for (std::size_t position = 0; position < bytes.size(); ++position)
        {
            bytes[position] = static_cast<std::byte>((value >> (position * 8)) & 0xff);
        }
        return bytes;
    }

    Payload sample_payload()
    {
        Payload payload;
        payload.process_name = "practice";

        PayloadEntry health;
        health.description = "Health";
        health.type        = ValueType::int32;
        payload.entries.push_back(health);

        PayloadScript no_reload {"No reload", "return true"};
        payload.scripts.push_back(no_reload);
        return payload;
    }

    std::vector<LiveEntry> sample_live()
    {
        std::vector<LiveEntry> live(1);
        live[0].address  = 0x7f2a12345678;
        live[0].resolved = true;
        live[0].bytes    = int_bytes(42);
        return live;
    }

    std::shared_ptr<const std::vector<RowView>>
    sample_rows(const Payload& payload, std::span<const LiveEntry> live, std::span<const ScriptState> scripts)
    {
        return std::make_shared<const std::vector<RowView>>(build_rows(payload, live, scripts));
    }
}

TEST_CASE("the trainer model renders the nine documented columns", "[trainer][gui]")
{
    slopkit_test_application();

    const auto               payload = sample_payload();
    const auto               live    = sample_live();
    std::vector<ScriptState> scripts(1);

    RowsModel model {payload};
    model.set_rows(sample_rows(payload, live, scripts));

    CHECK(model.columnCount() == RowsModel::column_count);
    CHECK(model.columnCount() == 9);
    CHECK(model.rowCount() == 2);

    CHECK(model.headerData(RowsModel::label, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("#"));
    CHECK(model.headerData(RowsModel::check, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("Frozen / Active"));
    CHECK(model.headerData(RowsModel::message, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("Message"));
    CHECK(model.headerData(RowsModel::value, Qt::Vertical, Qt::DisplayRole).isNull());

    const auto value_row = model.index(0, 0);
    CHECK(model.data(value_row, Qt::DisplayRole).toString() == QStringLiteral("1"));
    CHECK(model.data(model.index(0, RowsModel::description), Qt::DisplayRole).toString() == QStringLiteral("Health"));
    CHECK(model.data(model.index(0, RowsModel::value), Qt::DisplayRole).toString() == QStringLiteral("42"));
    CHECK(model.data(model.index(0, RowsModel::value), Qt::EditRole).toString() == QStringLiteral("42"));

    const auto script_row = model.index(1, 0);
    CHECK(model.data(script_row, Qt::DisplayRole).toString() == QStringLiteral("S1"));
    CHECK(model.data(model.index(1, RowsModel::state), Qt::DisplayRole).toString() == QStringLiteral("idle"));
    CHECK(model.data(model.index(1, RowsModel::value), Qt::EditRole).isNull());

    CHECK(model.data(model.index(0, RowsModel::check), Qt::CheckStateRole).toInt() == Qt::Unchecked);
    CHECK(model.data(model.index(1, RowsModel::check), Qt::CheckStateRole).toInt() == Qt::Unchecked);

    CHECK((model.flags(model.index(0, RowsModel::value)) & Qt::ItemIsEditable) != 0);
    CHECK((model.flags(model.index(1, RowsModel::value)) & Qt::ItemIsEditable) == 0);
    CHECK((model.flags(model.index(0, RowsModel::description)) & Qt::ItemIsEditable) == 0);
    CHECK((model.flags(model.index(0, RowsModel::check)) & Qt::ItemIsUserCheckable) != 0);
    CHECK((model.flags(model.index(1, RowsModel::check)) & Qt::ItemIsUserCheckable) != 0);
}

TEST_CASE("the value cell refuses a text the entry cannot hold", "[trainer][gui]")
{
    slopkit_test_application();

    const auto               payload = sample_payload();
    const auto               live    = sample_live();
    std::vector<ScriptState> scripts(1);

    RowsModel model {payload};
    model.set_rows(sample_rows(payload, live, scripts));

    std::vector<Action> submitted;
    QStringList         refusals;
    QObject::connect(&model,
                     &RowsModel::submitRequested,
                     [&](const Action& action)
                     {
                         submitted.push_back(action);
                     });
    QObject::connect(&model,
                     &RowsModel::statusChanged,
                     [&](const QString& text)
                     {
                         refusals.push_back(text);
                     });

    CHECK_FALSE(model.setData(model.index(0, RowsModel::value), QStringLiteral("hello"), Qt::EditRole));
    CHECK(submitted.empty());
    REQUIRE(refusals.size() == 1);
    CHECK(refusals.first().contains(QStringLiteral("hello")));
    CHECK(model.rows().front().value == "42");
}

TEST_CASE("an accepted value submits one set action and leaves the snapshot alone", "[trainer][gui]")
{
    slopkit_test_application();

    const auto               payload = sample_payload();
    const auto               live    = sample_live();
    std::vector<ScriptState> scripts(1);

    RowsModel model {payload};
    model.set_rows(sample_rows(payload, live, scripts));

    std::vector<Action> submitted;
    QObject::connect(&model,
                     &RowsModel::submitRequested,
                     [&](const Action& action)
                     {
                         submitted.push_back(action);
                     });

    CHECK(model.setData(model.index(0, RowsModel::value), QStringLiteral("7"), Qt::EditRole));
    REQUIRE(submitted.size() == 1);
    CHECK(submitted[0].kind == ActionKind::set_value);
    CHECK(submitted[0].index == 1);
    CHECK(submitted[0].text == "7");
    CHECK(model.rows().front().value == "42");
}

TEST_CASE("the check column toggles a value's freeze and a script's activity", "[trainer][gui]")
{
    slopkit_test_application();

    const auto               payload = sample_payload();
    const auto               live    = sample_live();
    std::vector<ScriptState> scripts(1);

    RowsModel model {payload};
    model.set_rows(sample_rows(payload, live, scripts));

    std::vector<Action> submitted;
    QObject::connect(&model,
                     &RowsModel::submitRequested,
                     [&](const Action& action)
                     {
                         submitted.push_back(action);
                     });

    CHECK(model.setData(model.index(0, RowsModel::check), Qt::Checked, Qt::CheckStateRole));
    REQUIRE(submitted.size() == 1);
    CHECK(submitted[0].kind == ActionKind::toggle_freeze);
    CHECK(submitted[0].index == 1);

    CHECK(model.setData(model.index(1, RowsModel::check), Qt::Checked, Qt::CheckStateRole));
    REQUIRE(submitted.size() == 2);
    CHECK(submitted[1].kind == ActionKind::activate_script);
    CHECK(submitted[1].index == 1);

    CHECK(model.setData(model.index(1, RowsModel::check), Qt::Unchecked, Qt::CheckStateRole));
    REQUIRE(submitted.size() == 3);
    CHECK(submitted[2].kind == ActionKind::deactivate_script);
    CHECK(submitted[2].index == 1);
}

TEST_CASE("the window lists the rows and closes cleanly", "[trainer][gui]")
{
    slopkit_test_application();

    const auto               payload = sample_payload();
    auto                     live    = sample_live();
    std::vector<ScriptState> scripts(1);

    TrainerWindow window(
        payload,
        live,
        scripts,
        []
        {
            return std::uint32_t {0};
        },
        Options {QStringLiteral("slopkit-trainer: practice"), 20},
        [](const Action&)
        {
            return std::string {};
        },
        [] {});
    window.show();

    QElapsedTimer elapsed;
    elapsed.start();
    while (window.rows_model()->rowCount() == 0 && elapsed.elapsed() < 2000)
    {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    QApplication::processEvents();

    CHECK(window.rows_model()->rowCount() == 2);
    CHECK(window.rows_model()->rows().front().kind == RowKind::value);

    window.close();
    QApplication::processEvents();
    CHECK(window.exit_code() == 0);
}
