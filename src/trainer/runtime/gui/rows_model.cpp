#include "trainer/runtime/gui/rows_model.hpp"

#include <utility>

#include <QColor>
#include <QFont>
#include <QModelIndex>

#include "ui/components/indent_delegate.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

namespace slopkit::trainer::gui
{
    namespace
    {
        QString column_title(int column)
        {
            switch (column)
            {
            case RowsModel::check:
                return {};
            case RowsModel::description:
                return QStringLiteral("Description");
            case RowsModel::address:
                return QStringLiteral("Address");
            case RowsModel::type:
                return QStringLiteral("Type");
            case RowsModel::value:
                return QStringLiteral("Value");
            case RowsModel::state:
                return QStringLiteral("State");
            case RowsModel::interval:
                return QStringLiteral("Interval");
            case RowsModel::message:
                return QStringLiteral("Message");
            default:
                return {};
            }
        }

        QString cell_text(const RowView& row, int column)
        {
            switch (column)
            {
            case RowsModel::check:
                return {};
            case RowsModel::description:
                return ui::to_qstring(row.description);
            case RowsModel::address:
                return ui::to_qstring(row.address);
            case RowsModel::type:
                return ui::to_qstring(row.type);
            case RowsModel::value:
                return ui::to_qstring(row.value);
            case RowsModel::state:
                return ui::to_qstring(row.state);
            case RowsModel::interval:
                return ui::to_qstring(row.interval);
            case RowsModel::message:
                return ui::to_qstring(row.message);
            default:
                return {};
            }
        }

        bool checked(const RowView& row)
        {
            return row.kind == RowKind::value ? row.frozen : row.active;
        }
    }

    RowsModel::RowsModel(const Payload& payload, QObject* parent) : QAbstractTableModel(parent), payload_(payload) {}

    int RowsModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() || rows_ == nullptr ? 0 : static_cast<int>(rows_->size());
    }

    int RowsModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    const RowView* RowsModel::row_at(const QModelIndex& index) const
    {
        if (rows_ == nullptr || !index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_->size()))
        {
            return nullptr;
        }
        return &(*rows_)[static_cast<std::size_t>(index.row())];
    }

    QVariant RowsModel::data(const QModelIndex& index, int role) const
    {
        const auto* row = row_at(index);
        if (row == nullptr)
        {
            return {};
        }

        switch (role)
        {
        case Qt::DisplayRole:
            return cell_text(*row, index.column());
        case Qt::EditRole:
            if (index.column() == value && row->kind == RowKind::value)
            {
                return ui::to_qstring(row->value);
            }
            return {};
        case Qt::CheckStateRole:
            if (index.column() == check)
            {
                return checked(*row) ? Qt::Checked : Qt::Unchecked;
            }
            return {};
        case Qt::ToolTipRole:
            if (index.column() == description)
            {
                return row->kind == RowKind::value ? tr("entry %1").arg(row->number)
                                                   : tr("script S%1").arg(row->number);
            }
            return {};
        case ui::widgets::kDepthRole:
            if (index.column() == description)
            {
                return static_cast<int>(row->depth);
            }
            return {};
        case Qt::FontRole:
            if (index.column() == address || index.column() == value || index.column() == interval)
            {
                return ui::mono_font();
            }
            return ui::ui_font();
        case Qt::TextAlignmentRole:
            if (index.column() == check)
            {
                return static_cast<int>(Qt::AlignCenter);
            }
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        case Qt::ForegroundRole:
            if (row->unresolved && index.column() == address)
            {
                return ui::active_theme().text_muted;
            }
            if (row->state == "error" && (index.column() == state || index.column() == message))
            {
                return ui::active_theme().error;
            }
            return {};
        default:
            return {};
        }
    }

    Qt::ItemFlags RowsModel::flags(const QModelIndex& index) const
    {
        const auto* row    = row_at(index);
        auto        result = QAbstractTableModel::flags(index);
        if (row == nullptr)
        {
            return result;
        }
        if (index.column() == check)
        {
            result |= Qt::ItemIsUserCheckable;
        }
        if (row->kind == RowKind::value && index.column() == value)
        {
            result |= Qt::ItemIsEditable;
        }
        return result;
    }

    QVariant RowsModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        {
            return {};
        }
        return column_title(section);
    }

    bool RowsModel::setData(const QModelIndex& index, const QVariant& text, int role)
    {
        const auto* row = row_at(index);
        if (row == nullptr)
        {
            return false;
        }

        if (role == Qt::CheckStateRole && index.column() == check)
        {
            Action action;
            action.index = row->number;
            if (row->kind == RowKind::value)
            {
                action.kind = ActionKind::toggle_freeze;
            }
            else
            {
                action.kind = text.toInt() == Qt::Checked ? ActionKind::activate_script : ActionKind::deactivate_script;
            }
            emit submitRequested(action);
            // The snapshot is the truth and the worker catches up on its next
            // pass, so the cell must repaint from it instead of staying painted
            // as the user just toggled it.
            emit dataChanged(index, index);
            return true;
        }

        if (role != Qt::EditRole || index.column() != value)
        {
            return false;
        }
        if (row->kind != RowKind::value || row->number == 0 || row->number > payload_.value_count())
        {
            return false;
        }

        const auto typed = text.toString();
        if (const auto refused = parse_value_text(*payload_.values()[row->number - 1], typed.toStdString()))
        {
            emit statusChanged(QString::fromStdString(*refused));
            return false;
        }

        Action action;
        action.kind  = ActionKind::set_value;
        action.index = row->number;
        action.text  = typed.toStdString();
        emit submitRequested(action);
        return true;
    }

    void RowsModel::set_rows(std::shared_ptr<const std::vector<RowView>> rows)
    {
        const auto before = rows_ == nullptr ? 0 : rows_->size();
        const auto after  = rows == nullptr ? 0 : rows->size();
        if (before != after)
        {
            beginResetModel();
            rows_ = std::move(rows);
            endResetModel();
            return;
        }

        rows_ = std::move(rows);
        if (after > 0)
        {
            emit dataChanged(index(0, 0), index(static_cast<int>(after) - 1, column_count - 1));
        }
    }

    const std::vector<RowView>& RowsModel::rows() const noexcept
    {
        static const std::vector<RowView> empty;
        return rows_ == nullptr ? empty : *rows_;
    }

    bool RowsModel::empty() const noexcept
    {
        return rows_ == nullptr || rows_->empty();
    }

}
