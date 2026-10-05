#include "ui/models/register_model.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

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

        std::string_view trimmed(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            {
                text.remove_suffix(1);
            }
            return text;
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
            entry.pending    = false;
        }

        if (!values_.empty())
        {
            emit dataChanged(index(0, value),
                             index(static_cast<int>(values_.size()) - 1, value),
                             {Qt::DisplayRole, Qt::ForegroundRole});
        }
    }

    void RegisterModel::set_commit_handler(CommitHandler handler)
    {
        commit_ = std::move(handler);
    }

    void RegisterModel::set_editable(bool editable)
    {
        if (editable_ == editable)
        {
            return;
        }
        editable_ = editable;
        if (!values_.empty())
        {
            emit dataChanged(index(0, value), index(static_cast<int>(values_.size()) - 1, value), {});
        }
    }

    bool RegisterModel::editable() const noexcept
    {
        return editable_;
    }

    std::optional<std::uint64_t> RegisterModel::parse_value(std::string_view text)
    {
        text = trimmed(text);
        if (text.empty())
        {
            return std::nullopt;
        }

        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            base = 16;
            text.remove_prefix(2);
        }
        else if (text.find_first_of("abcdefABCDEF") != std::string_view::npos)
        {
            base = 16;
        }
        if (text.empty())
        {
            return std::nullopt;
        }

        std::uint64_t result    = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result, base);
        if (error != std::errc {} || end != text.data() + text.size())
        {
            return std::nullopt;
        }
        return result;
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
            // Unread values and in-flight writes stay muted.
            return index.column() == value && (!entry.read || entry.pending) ? QVariant(active_theme().text_muted)
                                                                             : QVariant {};
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
        if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= values_.size())
        {
            return Qt::NoItemFlags;
        }

        Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (editable_ && index.column() == value && values_[static_cast<std::size_t>(index.row())].read)
        {
            result |= Qt::ItemIsEditable;
        }
        return result;
    }

    bool RegisterModel::setData(const QModelIndex& index, const QVariant& value, int role)
    {
        if (role != Qt::EditRole || !index.isValid() || index.column() != RegisterModel::value || index.row() < 0
            || static_cast<std::size_t>(index.row()) >= values_.size())
        {
            return false;
        }
        if (!editable_)
        {
            return false;
        }

        RegisterValue& entry = values_[static_cast<std::size_t>(index.row())];
        if (!entry.read)
        {
            return false;
        }
        const auto parsed = parse_value(value.toString().toStdString());
        if (!parsed)
        {
            return false;
        }
        if (commit_ && !commit_(entry.name, *parsed))
        {
            return false;
        }

        entry.pending = true;
        entry.value   = value.toString();
        emit dataChanged(index, index, {Qt::DisplayRole, Qt::ForegroundRole});
        return true;
    }

} // namespace slopkit::ui::models
