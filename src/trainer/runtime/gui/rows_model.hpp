#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <QAbstractTableModel>
#include <QMetaType>
#include <QString>
#include <QVariant>

#include "trainer/payload.hpp"
#include "trainer/runtime/action.hpp"
#include "trainer/runtime/rows.hpp"

namespace slopkit::trainer::gui
{
    // The address list's five columns, so the window reads like the table it was
    // generated from; a script's state and message live in the check box and the
    // log pane instead of extra columns.
    class RowsModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            check,
            description,
            address,
            type,
            value,
            column_count,
        };

        explicit RowsModel(const Payload& payload, QObject* parent = nullptr);

        [[nodiscard]] int           rowCount(const QModelIndex& parent = {}) const override;
        [[nodiscard]] int           columnCount(const QModelIndex& parent = {}) const override;
        [[nodiscard]] QVariant      data(const QModelIndex& index, int role) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        [[nodiscard]] QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
        bool                        setData(const QModelIndex& index, const QVariant& value, int role) override;

        // The worker's latest pass. Row count never changes (the table's own row
        // order), so a live pass repaints cells instead of resetting the model and
        // never closes an open editor.
        void set_rows(std::shared_ptr<const std::vector<RowView>> rows);

        [[nodiscard]] const std::vector<RowView>& rows() const noexcept;
        [[nodiscard]] bool                        empty() const noexcept;

    signals:
        // What the user asked for; the window hands it to the session worker.
        void submitRequested(slopkit::trainer::Action action);
        // Why a cell refused the text it was given, for the owning status bar.
        void statusChanged(const QString& message);

    private:
        [[nodiscard]] const RowView* row_at(const QModelIndex& index) const;

        const Payload&                              payload_;
        std::shared_ptr<const std::vector<RowView>> rows_;
    };

}

Q_DECLARE_METATYPE(slopkit::trainer::Action)
