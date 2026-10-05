#include "ui/models/register_model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "ui/theme.hpp"

namespace slopkit::ui::models
{

    namespace
    {
        constexpr std::array<std::string_view, 18> kRegisterNames = {
            "RAX",
            "RBX",
            "RCX",
            "RDX",
            "RSI",
            "RDI",
            "RBP",
            "RSP",
            "R8",
            "R9",
            "R10",
            "R11",
            "R12",
            "R13",
            "R14",
            "R15",
            "RIP",
            "RFLAGS",
        };

        // The em dash a value cell shows until the debugger supplies a value.
        QString placeholder()
        {
            return QString(QChar(0x2014));
        }
    } // namespace

    RegisterModel::RegisterModel(QObject* parent) : QAbstractTableModel(parent)
    {
        values_.reserve(kRegisterNames.size());
        for (const std::string_view register_name : kRegisterNames)
        {
            values_.push_back(RegisterValue {
                .name  = std::string(register_name),
                .value = placeholder(),
                .read  = false,
            });
        }
    }

    void RegisterModel::set_values(std::span<const RegisterValue> values)
    {
        for (RegisterValue& entry : values_)
        {
            const auto it    = std::ranges::find(values, entry.name, &RegisterValue::name);
            const bool found = it != values.end() && it->read;
            entry.value      = found ? it->value : placeholder();
            entry.read       = found;
        }

        if (!values_.empty())
        {
            emit dataChanged(index(0, value),
                             index(static_cast<int>(values_.size()) - 1, value),
                             {Qt::DisplayRole, Qt::ForegroundRole});
        }
    }

    int RegisterModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(values_.size());
    }

    int RegisterModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant RegisterModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= values_.size())
        {
            return {};
        }

        const RegisterValue& entry = values_[static_cast<std::size_t>(index.row())];
        switch (role)
        {
        case Qt::DisplayRole:
            return index.column() == name ? QString::fromStdString(entry.name) : entry.value;
        case Qt::ForegroundRole:
            // The em dashes stay muted so the pane reads as a placeholder.
            return index.column() == value && !entry.read ? QVariant(active_theme().text_muted) : QVariant {};
        case Qt::TextAlignmentRole:
            return index.column() == value ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                                           : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        default:
            return {};
        }
    }

    QVariant RegisterModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        {
            return {};
        }
        switch (section)
        {
        case name:
            return tr("Register");
        case value:
            return tr("Value");
        default:
            return {};
        }
    }

    Qt::ItemFlags RegisterModel::flags(const QModelIndex& index) const
    {
        if (!index.isValid())
        {
            return Qt::NoItemFlags;
        }
        // Read-only: the debugger will drive the values, never the user.
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

} // namespace slopkit::ui::models
