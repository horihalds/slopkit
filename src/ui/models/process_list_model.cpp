#include "ui/models/process_list_model.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>

#include "platform/linux/desktop_entry.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::models
{
    ProcessListModel::ProcessListModel(QObject* parent) : QAbstractTableModel(parent) {}

    int ProcessListModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(visible_.size());
    }

    int ProcessListModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant ProcessListModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(visible_.size()))
        {
            return {};
        }

        const auto& info = processes_[static_cast<std::size_t>(visible_[static_cast<std::size_t>(index.row())])];

        switch (role)
        {
        case Qt::DisplayRole:
        case Qt::ToolTipRole:
            switch (index.column())
            {
            case pid:
                return QString::number(static_cast<qulonglong>(info.pid));
            case name:
                return to_qstring(info.name);
            default:
                break;
            }
            break;
        case Qt::FontRole:
            if (index.column() == pid)
            {
                return QVariant::fromValue(mono_font());
            }
            break;
        case Qt::TextAlignmentRole:
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        default:
            break;
        }
        return {};
    }

    QVariant ProcessListModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
        {
            switch (section)
            {
            case pid:
                return tr("PID");
            case name:
                return tr("Name");
            default:
                break;
            }
        }
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    void ProcessListModel::sort(int column, Qt::SortOrder order)
    {
        sort_column_ = column;
        sort_order_  = order;

        beginResetModel();
        apply_sort();
        endResetModel();
    }

    void ProcessListModel::set_processes(std::vector<process::ProcessInfo> processes)
    {
        beginResetModel();
        processes_ = std::move(processes);
        rebuild();
        endResetModel();
    }

    void ProcessListModel::set_application_index(std::vector<std::string> executables)
    {
        beginResetModel();
        application_executables_ = std::move(executables);
        rebuild();
        endResetModel();
    }

    void ProcessListModel::set_applications_only(bool applications_only)
    {
        if (applications_only_ == applications_only)
        {
            return;
        }
        applications_only_ = applications_only;

        beginResetModel();
        rebuild();
        endResetModel();
    }

    void ProcessListModel::set_search(const QString& text)
    {
        if (search_ == text)
        {
            return;
        }
        search_ = text;

        beginResetModel();
        rebuild();
        endResetModel();
    }

    void ProcessListModel::set_plugin_filter(const QString& plugin_id)
    {
        if (plugin_filter_ == plugin_id)
        {
            return;
        }
        plugin_filter_ = plugin_id;

        beginResetModel();
        rebuild();
        endResetModel();
    }

    const process::ProcessInfo* ProcessListModel::process_at(int row) const
    {
        if (row < 0 || row >= static_cast<int>(visible_.size()))
        {
            return nullptr;
        }
        return &processes_[static_cast<std::size_t>(visible_[static_cast<std::size_t>(row)])];
    }

    int ProcessListModel::row_for_pid(process::ProcessId pid) const
    {
        for (int row = 0; row < static_cast<int>(visible_.size()); ++row)
        {
            if (processes_[static_cast<std::size_t>(visible_[static_cast<std::size_t>(row)])].pid == pid)
            {
                return row;
            }
        }
        return -1;
    }

    void ProcessListModel::rebuild()
    {
        visible_.clear();
        const std::string needle = lowercase(search_.toStdString());

        for (int index = 0; index < static_cast<int>(processes_.size()); ++index)
        {
            const auto& info = processes_[static_cast<std::size_t>(index)];
            // The Applications view keeps only desktop entries; the Processes
            // view keeps everything.
            if (applications_only_ && !platform::is_desktop_application(info.exe_path, application_executables_))
            {
                continue;
            }
            if (!plugin_filter_.isEmpty()
                && std::find(info.claimants.begin(), info.claimants.end(), plugin_filter_.toStdString())
                       == info.claimants.end())
            {
                continue;
            }
            if (!needle.empty())
            {
                const auto pid_text = std::to_string(info.pid);
                if (lowercase(info.name).find(needle) == std::string::npos
                    && lowercase(info.exe_path).find(needle) == std::string::npos
                    && pid_text.find(needle) == std::string::npos)
                {
                    continue;
                }
            }
            visible_.push_back(index);
        }

        apply_sort();
    }

    void ProcessListModel::apply_sort()
    {
        std::ranges::sort(visible_,
                          [&](int lhs, int rhs)
                          {
                              const auto& left  = processes_[static_cast<std::size_t>(lhs)];
                              const auto& right = processes_[static_cast<std::size_t>(rhs)];

                              int order = 0;
                              switch (sort_column_)
                              {
                              case name:
                                  order = left.name.compare(right.name);
                                  break;
                              default:
                                  order = left.pid < right.pid ? -1 : (left.pid > right.pid ? 1 : 0);
                                  break;
                              }
                              if (order == 0)
                              {
                                  order = left.pid < right.pid ? -1 : (left.pid > right.pid ? 1 : 0);
                              }
                              return sort_order_ == Qt::AscendingOrder ? order < 0 : order > 0;
                          });
    }

} // namespace slopkit::ui::models
