#pragma once

#include <span>
#include <vector>

#include <QAbstractTableModel>
#include <QString>

#include "debug/backend.hpp"
#include "ui/address_format.hpp"

namespace slopkit::ui::models
{

    // The call stack of the stopped thread: frame index, the address in the
    // current display mode and the absolute address. It only renders what the
    // controller supplied.
    class CallStackModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            frame,
            address,
            absolute,
            column_count,
        };

        explicit CallStackModel(QObject* parent = nullptr);

        void
        set_frames(std::span<const slopkit::debug::Frame> frames, const ui::ModuleSpans& spans, ui::AddressMode mode);
        void clear();

        [[nodiscard]] int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    private:
        std::vector<QString> address_;
        std::vector<QString> full_; // the untruncated module+RVA, for hovers
        std::vector<QString> absolute_;
    };

} // namespace slopkit::ui::models
