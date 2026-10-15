#include <catch2/catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <QStringList>

#include "ui/models/register_model.hpp"

using slopkit::ui::models::RegisterModel;
using slopkit::ui::models::RegisterValue;

namespace
{
    RegisterValue read_value(std::string name, QString value)
    {
        return RegisterValue {.name = std::move(name), .value = std::move(value), .read = true};
    }
}

TEST_CASE("register model starts as a muted placeholder table", "[ui][models][register]")
{
    RegisterModel model;
    CHECK(model.rowCount() == 18);
    CHECK(model.columnCount() == 2);
    CHECK(model.data(model.index(0, RegisterModel::name), Qt::DisplayRole).toString() == QStringLiteral("RAX"));
    CHECK(model.data(model.index(17, RegisterModel::name), Qt::DisplayRole).toString() == QStringLiteral("RFLAGS"));

    CHECK(model.data(model.index(0, RegisterModel::value), Qt::DisplayRole).toString() != QStringLiteral("0"));
    CHECK_FALSE(model.flags(model.index(0, RegisterModel::value)) & Qt::ItemIsEditable);
    CHECK_FALSE(model.editable());
}

TEST_CASE("register model parses hex and decimal values", "[ui][models][register]")
{
    CHECK(RegisterModel::parse_value("0x2000") == 0x2000);
    CHECK(RegisterModel::parse_value("0X2a") == 0x2A);
    CHECK(RegisterModel::parse_value("beef") == 0xBEEF);
    CHECK(RegisterModel::parse_value(" 0x10 ") == 0x10);
    CHECK(RegisterModel::parse_value("8192") == 0x8192);
    CHECK(RegisterModel::parse_value("#8192") == 8192);
    CHECK(RegisterModel::parse_value("0000000000000042") == 0x42);

    CHECK_FALSE(RegisterModel::parse_value("").has_value());
    CHECK_FALSE(RegisterModel::parse_value("0x").has_value());
    CHECK_FALSE(RegisterModel::parse_value("12x").has_value());
    CHECK_FALSE(RegisterModel::parse_value("0xZZ").has_value());
}

TEST_CASE("register model routes a committed edit through the handler", "[ui][models][register]")
{
    RegisterModel model;
    model.set_values(std::vector {read_value("RAX", QStringLiteral("0x0000000000000001"))});
    CHECK(model.data(model.index(0, RegisterModel::value), Qt::DisplayRole).toString()
          == QStringLiteral("0x0000000000000001"));

    bool          committed = false;
    std::string   name;
    std::uint64_t value = 0;
    model.set_commit_handler(
        [&](const std::string& written, std::uint64_t number)
        {
            committed = true;
            name      = written;
            value     = number;
            return true;
        });

    CHECK_FALSE(model.setData(model.index(0, RegisterModel::value), QStringLiteral("0x10"), Qt::EditRole));
    CHECK_FALSE(committed);

    model.set_editable(true);
    CHECK(model.editable());
    CHECK(model.flags(model.index(0, RegisterModel::value)) & Qt::ItemIsEditable);
    CHECK_FALSE(model.flags(model.index(0, RegisterModel::name)) & Qt::ItemIsEditable);

    CHECK(model.setData(model.index(0, RegisterModel::value), QStringLiteral("0x10"), Qt::EditRole));
    CHECK(committed);
    CHECK(name == "RAX");
    CHECK(value == 0x10);
    CHECK(model.data(model.index(0, RegisterModel::value), Qt::DisplayRole).toString() == QStringLiteral("0x10"));

    committed = false;
    CHECK_FALSE(model.setData(model.index(0, RegisterModel::value), QStringLiteral("nope"), Qt::EditRole));
    CHECK_FALSE(committed);

    model.set_commit_handler(
        [](const std::string&, std::uint64_t)
        {
            return false;
        });
    CHECK_FALSE(model.setData(model.index(0, RegisterModel::value), QStringLiteral("0x20"), Qt::EditRole));

    model.set_values(std::vector {read_value("RAX", QStringLiteral("0x1"))});
    model.set_values(std::span<const RegisterValue> {});
    CHECK_FALSE(model.flags(model.index(0, RegisterModel::value)) & Qt::ItemIsEditable);

    model.set_values(std::vector {read_value("RAX", QStringLiteral("0x1"))});
    model.set_editable(false);
    CHECK_FALSE(model.flags(model.index(0, RegisterModel::value)) & Qt::ItemIsEditable);
}

TEST_CASE("register model names why an edit was refused", "[ui][models][register]")
{
    RegisterModel model;
    model.set_values(std::vector {read_value("RAX", QStringLiteral("0x1"))});
    model.set_editable(true);

    QStringList messages;
    QObject::connect(&model,
                     &RegisterModel::statusChanged,
                     [&messages](const QString& message, bool error)
                     {
                         messages.push_back(error ? QStringLiteral("error: ") + message : message);
                     });

    model.set_commit_handler(
        [](const std::string&, std::uint64_t)
        {
            return true;
        });

    const QModelIndex cell = model.index(0, RegisterModel::value);
    CHECK(model.setData(cell, QStringLiteral("0x10"), Qt::EditRole));
    CHECK(messages.isEmpty());

    // A text the model cannot read names what the cell expects instead of
    // dropping the typed value silently.
    CHECK_FALSE(model.setData(cell, QStringLiteral("zzz"), Qt::EditRole));
    CHECK(messages == QStringList {QStringLiteral("error: Value: expected a number such as 1A or 0x1A.")});

    // And a write the controller turns down names that too.
    model.set_commit_handler(
        [](const std::string&, std::uint64_t)
        {
            return false;
        });
    messages.clear();
    CHECK_FALSE(model.setData(cell, QStringLiteral("0x20"), Qt::EditRole));
    CHECK(messages == QStringList {QStringLiteral("error: The register could not be written.")});
    CHECK(model.data(cell, Qt::DisplayRole).toString() == QStringLiteral("0x10"));
}
