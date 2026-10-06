#include "ui/models/breakpoint_model.hpp"

#include <cstddef>

#include "ui/address_format.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::models
{

    namespace
    {
        QString kind_text(slopkit::debug::Kind kind)
        {
            switch (kind)
            {
            case slopkit::debug::Kind::hardware_execute:
                return QStringLiteral("Hardware execute");
            case slopkit::debug::Kind::hardware_write:
                return QStringLiteral("Hardware write");
            case slopkit::debug::Kind::hardware_read_write:
                return QStringLiteral("Hardware read/write");
            case slopkit::debug::Kind::software:
            default:
                return QStringLiteral("Software");
            }
        }
    } // namespace

    BreakpointModel::BreakpointModel(slopkit::debug::Controller& controller, QObject* parent)
        : QAbstractTableModel(parent), controller_(controller)
    {
        refresh();
    }

    void BreakpointModel::refresh()
    {
        beginResetModel();
        const auto entries = controller_.breakpoints();
        // A hidden entry (the access watch) owns a slot but is not a user
        // breakpoint, so it stays out of this window.
        rows_.clear();
        for (const slopkit::debug::Breakpoint& entry : entries)
        {
            if (!entry.hidden)
            {
                rows_.push_back(entry);
            }
        }
        endResetModel();
    }

    std::uint64_t BreakpointModel::id_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size() ? rows_[static_cast<std::size_t>(row)].id : 0;
    }

    bool BreakpointModel::is_armed_at(int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < rows_.size() && rows_[static_cast<std::size_t>(row)].armed;
    }

    int BreakpointModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }

    int BreakpointModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant BreakpointModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= rows_.size())
        {
            return {};
        }

        const slopkit::debug::Breakpoint& entry = rows_[static_cast<std::size_t>(index.row())];
        switch (role)
        {
        case Qt::DisplayRole:
            switch (index.column())
            {
            case enabled:
                return {};
            case kind:
                return kind_text(entry.kind);
            case size:
                return QString::number(static_cast<qulonglong>(entry.size));
            case address:
                return QString::fromStdString(entry.expression);
            case hits:
                return QString::number(static_cast<qulonglong>(entry.hits));
            default:
                return {};
            }
        case Qt::CheckStateRole:
            return index.column() == enabled ? QVariant(entry.enabled ? Qt::Checked : Qt::Unchecked) : QVariant {};
        case Qt::ToolTipRole:
            if (index.column() == address)
            {
                return ui::format_padded_hex(entry.address);
            }
            if (index.column() == enabled && !entry.armed && entry.enabled)
            {
                return tr("Not armed");
            }
            return {};
        case Qt::FontRole:
            // Only the machine columns line up; the kind text and the enable
            // checkbox keep the UI font.
            if (index.column() == size || index.column() == address || index.column() == hits)
            {
                return QVariant::fromValue(mono_font());
            }
            return {};
        case Qt::ForegroundRole:
            // A disabled breakpoint is muted; so is one that has not been armed.
            return !entry.enabled || !entry.armed ? QVariant(active_theme().text_muted) : QVariant {};
        default:
            return {};
        }
    }

    QVariant BreakpointModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        {
            return {};
        }
        switch (section)
        {
        case enabled:
            return tr("Enabled");
        case kind:
            return tr("Kind");
        case size:
            return tr("Size");
        case address:
            return tr("Address");
        case hits:
            return tr("Hits");
        default:
            return {};
        }
    }

    Qt::ItemFlags BreakpointModel::flags(const QModelIndex& index) const
    {
        if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= rows_.size())
        {
            return Qt::NoItemFlags;
        }
        Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (index.column() == enabled)
        {
            result |= Qt::ItemIsUserCheckable;
        }
        return result;
    }

    bool BreakpointModel::setData(const QModelIndex& index, const QVariant& value, int role)
    {
        if (role != Qt::CheckStateRole || !index.isValid() || index.column() != enabled || index.row() < 0
            || static_cast<std::size_t>(index.row()) >= rows_.size())
        {
            return false;
        }

        const bool enabled = value.toInt() == Qt::Checked;
        controller_.set_breakpoint_enabled(rows_[static_cast<std::size_t>(index.row())].id, enabled);
        refresh();
        return true;
    }

} // namespace slopkit::ui::models
