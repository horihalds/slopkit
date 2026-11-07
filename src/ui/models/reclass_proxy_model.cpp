#include "ui/models/reclass_proxy_model.hpp"

#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/fonts.hpp"
#include "ui/models/reclass_structure_model.hpp"
#include "ui/models/reclass_value_model.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::models
{
    ReclassProxyModel::ReclassProxyModel(ReclassStructureModel& structure, ReclassValueModel& values, QObject* parent)
        : QIdentityProxyModel(parent), structure_(structure), values_(values)
    {
        setSourceModel(&structure);
        connect(&structure, &QAbstractItemModel::modelReset, this, &ReclassProxyModel::drop_cache);
    }

    QVariant ReclassProxyModel::data(const QModelIndex& index, int role) const
    {
        const ReclassRow* row = structure_.row_for(mapToSource(index));
        if (row == nullptr)
        {
            return QIdentityProxyModel::data(index, role);
        }

        if (role == Qt::DisplayRole || role == widgets::kFullTextRole)
        {
            if (index.column() == ReclassStructureModel::address)
            {
                return values_.address_text(*row);
            }
            if (index.column() == ReclassStructureModel::ascii)
            {
                // The cell draws at most kAsciiMaxChars characters, while the
                // tooltip answers the whole reading.
                return role == widgets::kFullTextRole ? values_.ascii_full_text(*row) : values_.ascii_text(*row);
            }
            if (index.column() == ReclassStructureModel::value)
            {
                return values_.value_text(*row);
            }
            if (index.column() == ReclassStructureModel::hints)
            {
                return values_.hint_text(*row);
            }
            if (index.column() == ReclassStructureModel::pointer)
            {
                return values_.pointer_text(*row);
            }
        }
        // The editor starts from the value a write parses, not from the readings
        // the cell draws beside it.
        if (role == Qt::EditRole && index.column() == ReclassStructureModel::value)
        {
            return values_.edit_text(*row);
        }

        // The live fields are machine read-outs, so they take the mono face the
        // structure model gives its own mono columns.
        if (role == Qt::FontRole
            && (index.column() == ReclassStructureModel::hints || index.column() == ReclassStructureModel::pointer))
        {
            return QVariant::fromValue(mono_font());
        }

        const QVariant base = QIdentityProxyModel::data(index, role);
        // Whatever the structure model already says about the cell - a muted
        // comment, a marked reference row - stands. The value cell adds only its
        // muted unreadable colour; the moved-byte warning is the view's own,
        // painted from the value model's per-byte mask.
        if (role != Qt::ForegroundRole || base.isValid())
        {
            return base;
        }

        if (index.column() == ReclassStructureModel::address)
        {
            return values_.node_address(*row).has_value() ? active_theme().grid_address : active_theme().text_muted;
        }
        if (index.column() == ReclassStructureModel::ascii)
        {
            return values_.live_state(*row) == ReclassValueModel::LiveState::unreadable ? active_theme().text_muted
                                                                                        : active_theme().grid_ascii;
        }
        if (index.column() == ReclassStructureModel::value)
        {
            if (values_.live_state(*row) == ReclassValueModel::LiveState::unreadable)
            {
                return active_theme().text_muted;
            }
        }
        if (index.column() == ReclassStructureModel::hints)
        {
            return active_theme().datatype_readout;
        }
        if (index.column() == ReclassStructureModel::pointer)
        {
            return active_theme().grid_pointer;
        }
        return base;
    }

    Qt::ItemFlags ReclassProxyModel::flags(const QModelIndex& index) const
    {
        Qt::ItemFlags result = QIdentityProxyModel::flags(index);
        if (index.column() == ReclassStructureModel::value)
        {
            const ReclassRow* row = structure_.row_for(mapToSource(index));
            if (row != nullptr && values_.editable(*row))
            {
                result |= Qt::ItemIsEditable;
            }
        }
        return result;
    }

    bool ReclassProxyModel::setData(const QModelIndex& index, const QVariant& value, int role)
    {
        if (role != Qt::EditRole)
        {
            return false;
        }
        if (index.column() == ReclassStructureModel::value)
        {
            const ReclassRow* row = structure_.row_for(mapToSource(index));
            return row != nullptr && values_.write_value(*row, value.toString());
        }
        // A name is the structure model's own field, so the identity base passes
        // that edit - and only that one - on to it.
        return QIdentityProxyModel::setData(index, value, role);
    }

    void ReclassProxyModel::refresh_values()
    {
        refresh({});
        cache_valid_ = true;
    }

    void ReclassProxyModel::drop_cache()
    {
        shown_.clear();
        cache_valid_ = false;
    }

    void ReclassProxyModel::refresh(const QModelIndex& parent)
    {
        const int rows = rowCount(parent);
        for (int position = 0; position < rows; ++position)
        {
            const QModelIndex address_index = index(position, ReclassStructureModel::address, parent);
            const QModelIndex ascii_index   = index(position, ReclassStructureModel::ascii, parent);
            const QModelIndex value_index   = index(position, ReclassStructureModel::value, parent);
            const QModelIndex hints_index   = index(position, ReclassStructureModel::hints, parent);
            const QModelIndex pointer_index = index(position, ReclassStructureModel::pointer, parent);
            const ReclassRow* row           = structure_.row_for(mapToSource(address_index));

            if (row != nullptr && address_index.isValid() && value_index.isValid())
            {
                const Shown now {.address = values_.address_text(*row),
                                 .ascii   = values_.ascii_text(*row),
                                 .value   = values_.value_text(*row),
                                 .hints   = values_.hint_text(*row),
                                 .pointer = values_.pointer_text(*row)};
                const auto  before = shown_.find(row);
                // The first pass after a rebuild only fills the cache: the
                // reset the rebuild performed has already repainted the view.
                if (cache_valid_ && before != shown_.end())
                {
                    if (before->second.address != now.address)
                    {
                        emit dataChanged(address_index, address_index);
                    }
                    if (before->second.ascii != now.ascii)
                    {
                        emit dataChanged(ascii_index, ascii_index);
                    }
                    if (before->second.value != now.value)
                    {
                        emit dataChanged(value_index, value_index);
                    }
                    if (before->second.hints != now.hints)
                    {
                        emit dataChanged(hints_index, hints_index);
                    }
                    if (before->second.pointer != now.pointer)
                    {
                        emit dataChanged(pointer_index, pointer_index);
                    }
                }
                shown_.insert_or_assign(row, now);
            }

            // The row's own index is what carries the children: a column-1
            // index would be rejected by the structure model's rowCount guard.
            refresh(index(position, 0, parent));
        }
    }

}
