#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "process/types.hpp"

#include <QAbstractTableModel>
#include <QString>

namespace slopkit::ui::models
{
    // PID / Name rows for one tab, search text and plugin filter of the
    // Process List dialog.
    class ProcessListModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            pid,
            name,
            column_count,
        };

        explicit ProcessListModel(QObject* parent = nullptr);

        [[nodiscard]] int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
        void                   sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

        void set_processes(std::vector<process::ProcessInfo> processes);
        void set_application_index(std::vector<std::string> executables);
        void set_applications_only(bool applications_only);
        void set_search(const QString& text);
        void set_plugin_filter(const QString& plugin_id);

        [[nodiscard]] const process::ProcessInfo* process_at(int row) const;
        [[nodiscard]] int                         row_for_pid(process::ProcessId pid) const;

    private:
        void rebuild();
        void apply_sort();

        std::vector<process::ProcessInfo> processes_;
        std::vector<std::string>          application_executables_;
        std::vector<int>                  visible_;

        bool          applications_only_ {false};
        QString       search_;
        QString       plugin_filter_;
        int           sort_column_ {pid};
        Qt::SortOrder sort_order_ {Qt::AscendingOrder};
    };

} // namespace slopkit::ui::models
