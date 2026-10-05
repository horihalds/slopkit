#include "ui/models/call_stack_model.hpp"

#include <cstddef>

#include "ui/theme.hpp"

namespace slopkit::ui::models
{

    CallStackModel::CallStackModel(QObject* parent) : QAbstractTableModel(parent) {}

    void CallStackModel::set_frames(std::span<const slopkit::debug::Frame> frames,
                                    const ui::ModuleSpans&                 spans,
                                    ui::AddressMode                        mode)
    {
        beginResetModel();
        address_.clear();
        absolute_.clear();
        address_.reserve(frames.size());
        absolute_.reserve(frames.size());
        for (const slopkit::debug::Frame& frame : frames)
        {
            const auto relative = ui::module_relative_text(mode, spans, frame.pc);
            address_.push_back(relative ? *relative : ui::format_absolute(frame.pc));
            absolute_.push_back(ui::format_absolute(frame.pc));
        }
        endResetModel();
    }

    void CallStackModel::clear()
    {
        beginResetModel();
        address_.clear();
        absolute_.clear();
        endResetModel();
    }

    int CallStackModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(address_.size());
    }

    int CallStackModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant CallStackModel::data(const QModelIndex& model_index, int role) const
    {
        if (!model_index.isValid() || model_index.row() < 0
            || static_cast<std::size_t>(model_index.row()) >= address_.size())
        {
            return {};
        }

        const auto row = static_cast<std::size_t>(model_index.row());
        switch (role)
        {
        case Qt::DisplayRole:
            switch (model_index.column())
            {
            case frame:
                return QString::number(static_cast<qlonglong>(row));
            case address:
                return address_[row];
            case absolute:
                return absolute_[row];
            default:
                return {};
            }
        case Qt::ForegroundRole:
            return model_index.column() == frame ? QVariant(active_theme().text_muted) : QVariant {};
        case Qt::TextAlignmentRole:
            return model_index.column() == frame ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                                                 : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        default:
            return {};
        }
    }

    QVariant CallStackModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        {
            return {};
        }
        switch (section)
        {
        case frame:
            return tr("#");
        case address:
            return tr("Address");
        case absolute:
            return tr("Absolute");
        default:
            return {};
        }
    }

} // namespace slopkit::ui::models
