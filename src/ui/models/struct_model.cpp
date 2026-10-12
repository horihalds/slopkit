#include "ui/models/struct_model.hpp"

#include <optional>
#include <string>

#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/indent_delegate.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"

#include <QFont>

namespace slopkit::ui::models
{
    namespace
    {
        [[nodiscard]] std::optional<std::uint64_t> parse_offset(const QString& text)
        {
            const QString trimmed = text.trimmed();
            if (trimmed.isEmpty())
            {
                return std::nullopt;
            }
            bool ok = false;
            if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            {
                const qulonglong value = trimmed.mid(2).toULongLong(&ok, 16);
                return ok ? std::optional<std::uint64_t> {value} : std::nullopt;
            }
            const qulonglong value = trimmed.toULongLong(&ok, 10);
            return ok ? std::optional<std::uint64_t> {value} : std::nullopt;
        }
    }

    StructModel::StructModel(structs::Structs& definitions, components::StructDocument& document, QObject* parent)
        : QAbstractTableModel(parent), definitions_(definitions), document_(document)
    {
    }

    int StructModel::rowCount(const QModelIndex& parent) const
    {
        if (parent.isValid())
        {
            return 0;
        }
        return static_cast<int>(definitions_.size());
    }

    int StructModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    std::uint64_t StructModel::id_at(std::size_t row) const
    {
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        return row < nodes.size() ? nodes[row].id : 0;
    }

    int StructModel::depth_at(std::size_t row) const
    {
        return static_cast<int>(definitions_.depth_of(row));
    }

    QVariant StructModel::data(const QModelIndex& index, int role) const
    {
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        const std::size_t                          row   = static_cast<std::size_t>(index.row());
        if (!index.isValid() || row >= nodes.size())
        {
            return {};
        }
        const structs::StructNode& node = nodes[row];
        const std::uint64_t        id   = node.id;

        if (role == widgets::kDepthRole)
        {
            return depth_at(row);
        }

        const bool display = role == Qt::DisplayRole || role == Qt::EditRole;
        if (!display)
        {
            if (role == Qt::FontRole && index.column() == value && node.parent != 0)
            {
                return mono_font();
            }
            if (role == widgets::kFullTextRole)
            {
                return index.column() == name ? to_qstring(node.name) : QVariant {};
            }
            return {};
        }

        switch (index.column())
        {
        case offset:
            return document_.offset_text(id);
        case type:
            return document_.type_text(id);
        case name:
            return to_qstring(node.name);
        case value:
            return document_.value_text(id);
        default:
            return {};
        }
    }

    QVariant StructModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        {
            return {};
        }
        switch (section)
        {
        case offset:
            return tr("Offset");
        case type:
            return tr("Type");
        case name:
            return tr("Name");
        case value:
            return tr("Value");
        default:
            return {};
        }
    }

    Qt::ItemFlags StructModel::flags(const QModelIndex& index) const
    {
        Qt::ItemFlags flags = QAbstractTableModel::flags(index);
        if (!index.isValid())
        {
            return flags;
        }
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        const std::size_t                          row   = static_cast<std::size_t>(index.row());
        if (row >= nodes.size())
        {
            return flags;
        }
        const structs::StructNode& node = nodes[row];
        switch (index.column())
        {
        case name:
            flags |= Qt::ItemIsEditable;
            break;
        case offset:
            if (node.parent != 0)
            {
                flags |= Qt::ItemIsEditable;
            }
            break;
        case value:
            if (node.parent != 0 && node.type.kind != structs::FieldKind::structure)
            {
                flags |= Qt::ItemIsEditable;
            }
            break;
        default:
            break;
        }
        return flags;
    }

    bool StructModel::setData(const QModelIndex& index, const QVariant& edited, int role)
    {
        if (role != Qt::EditRole || !index.isValid())
        {
            return false;
        }
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        const std::size_t                          row   = static_cast<std::size_t>(index.row());
        if (row >= nodes.size())
        {
            return false;
        }
        const std::uint64_t id   = nodes[row].id;
        const QString       text = edited.toString();

        switch (index.column())
        {
        case name:
        {
            const QString trimmed = text.trimmed();
            if (trimmed.isEmpty())
            {
                emit statusChanged(tr("A field needs a name."), true);
                return false;
            }
            definitions_.rename(id, trimmed.toStdString());
            emit dataChanged(index, index);
            return true;
        }
        case offset:
        {
            const auto parsed = parse_offset(text);
            if (!parsed.has_value())
            {
                emit statusChanged(tr("The offset must be a number, for example 0x10."), true);
                return false;
            }
            definitions_.set_offset(id, *parsed);
            document_.refresh();
            emit dataChanged(this->index(index.row(), 0), this->index(index.row(), column_count - 1));
            emit statusChanged(tr("Offset updated."), false);
            return true;
        }
        case value:
            return document_.write_value(id, text);
        default:
            return false;
        }
    }

    void StructModel::refresh()
    {
        beginResetModel();
        endResetModel();
    }

    void StructModel::notify_values_changed()
    {
        if (definitions_.empty())
        {
            return;
        }
        emit dataChanged(
            index(0, value), index(static_cast<int>(definitions_.size()) - 1, value), {Qt::DisplayRole, Qt::FontRole});
    }

}
