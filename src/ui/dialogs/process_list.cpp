#include "ui/dialogs/process_list.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "platform/linux/desktop_entry.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        }

        std::string lowercase(std::string_view value)
        {
            std::string result(value);
            std::ranges::transform(result,
                                   result.begin(),
                                   [](char character)
                                   {
                                       return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                                   });
            return result;
        }
    } // namespace

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
            case plugin:
                return to_qstring(info.plugin_id);
            case executable:
                return info.exe_path.empty() ? tr("(unknown)") : to_qstring(info.exe_path);
            default:
                break;
            }
            break;
        case Qt::FontRole:
            if (index.column() == pid || index.column() == executable)
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
            case plugin:
                return tr("Plugin");
            case executable:
                return tr("Executable");
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
            if (platform::is_desktop_application(info.exe_path, application_executables_) != applications_only_)
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
                              case plugin:
                                  order = left.plugin_id.compare(right.plugin_id);
                                  break;
                              case executable:
                                  order = left.exe_path.compare(right.exe_path);
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

    ProcessListDialog::ProcessListDialog(process::AccessWorker&   worker,
                                         process::AttachedTarget& target,
                                         QWidget*                 parent)
        : QDialog(parent), worker_(worker), target_(target)
    {
        setWindowTitle(tr("Process List"));
        resize(900, 560);

        build_layout();

        auto_refresh_timer_ = new QTimer(this);
        auto_refresh_timer_->setInterval(2000);
        connect(auto_refresh_timer_,
                &QTimer::timeout,
                this,
                [this]
                {
                    if (isVisible())
                    {
                        refresh(); // No-ops while a list job is already pending.
                    }
                });
        auto_refresh_timer_->start();

        update_detail();
        update_buttons();
        update_status();
    }

    void ProcessListDialog::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        auto* controls      = new QHBoxLayout();
        refresh_button_     = new widgets::PrimaryButton(tr("Refresh"), this);
        auto_refresh_check_ = new QCheckBox(tr("Auto-refresh"), this);
        auto_refresh_check_->setChecked(true);
        search_edit_ = new QLineEdit(this);
        search_edit_->setPlaceholderText(tr("Filter by name, PID or path"));
        search_edit_->setClearButtonEnabled(true);
        plugin_combo_ = new QComboBox(this);
        controls->addWidget(refresh_button_);
        controls->addWidget(auto_refresh_check_);
        controls->addWidget(search_edit_, 1);
        controls->addWidget(plugin_combo_);
        layout->addLayout(controls);

        view_tabs_ = new QTabBar(this);
        view_tabs_->addTab(tr("Applications"));
        view_tabs_->addTab(tr("Processes"));
        view_tabs_->setCurrentIndex(1); // The Processes view is the default.
        layout->addWidget(view_tabs_);

        auto* body  = new QHBoxLayout();
        model_      = new ProcessListModel(this);
        table_view_ = new QTableView(this);
        table_view_->setModel(model_);
        table_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_view_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_view_->setAlternatingRowColors(true);
        table_view_->setSortingEnabled(true);
        table_view_->sortByColumn(ProcessListModel::pid, Qt::AscendingOrder);
        table_view_->verticalHeader()->setVisible(false);
        table_view_->horizontalHeader()->setSectionResizeMode(ProcessListModel::pid, QHeaderView::ResizeToContents);
        table_view_->horizontalHeader()->setSectionResizeMode(ProcessListModel::name, QHeaderView::Stretch);
        table_view_->horizontalHeader()->setSectionResizeMode(ProcessListModel::plugin, QHeaderView::ResizeToContents);
        table_view_->horizontalHeader()->setSectionResizeMode(ProcessListModel::executable, QHeaderView::Stretch);
        body->addWidget(table_view_, 3);

        auto* detail_panel = new widgets::Panel(tr("Details"), this);
        detail_hint_       = new QLabel(tr("Select a process to see its details."), detail_panel);
        detail_hint_->setWordWrap(true);
        detail_panel->body()->addWidget(detail_hint_);

        detail_body_        = new QWidget(detail_panel);
        auto* detail_layout = new QVBoxLayout(detail_body_);
        detail_layout->setContentsMargins(0, 0, 0, 0);
        detail_layout->setSpacing(4);

        detail_pid_ = new QLabel(detail_body_);
        detail_pid_->setFont(mono_font());
        detail_layout->addWidget(detail_pid_);

        detail_name_ = new QLabel(detail_body_);
        detail_name_->setWordWrap(true);
        detail_layout->addWidget(detail_name_);

        detail_executable_ = new QLabel(detail_body_);
        detail_executable_->setFont(mono_font());
        detail_executable_->setWordWrap(true);
        detail_layout->addWidget(detail_executable_);

        detail_plugin_ = new QLabel(detail_body_);
        detail_plugin_->setWordWrap(true);
        detail_layout->addWidget(detail_plugin_);

        claimants_area_   = new QWidget(detail_body_);
        claimants_layout_ = new QVBoxLayout(claimants_area_);
        claimants_layout_->setContentsMargins(0, 0, 0, 0);
        claimants_layout_->setSpacing(2);
        detail_layout->addWidget(claimants_area_);

        detail_inspecting_ = new QLabel(tr("Inspecting..."), detail_body_);
        detail_layout->addWidget(detail_inspecting_);

        detail_access_ = new QLabel(detail_body_);
        detail_access_->setFont(mono_font());
        detail_access_->setWordWrap(true);
        detail_layout->addWidget(detail_access_);

        detail_modules_ = new QLabel(detail_body_);
        detail_modules_->setFont(mono_font());
        detail_layout->addWidget(detail_modules_);

        detail_threads_ = new QLabel(detail_body_);
        detail_threads_->setFont(mono_font());
        detail_layout->addWidget(detail_threads_);

        detail_error_ = new widgets::StatusLabel(detail_body_);
        detail_layout->addWidget(detail_error_);

        attach_button_   = new widgets::PrimaryButton(tr("Attach"), detail_body_);
        auto* attach_row = new QHBoxLayout();
        attach_row->addWidget(attach_button_);
        attached_label_ = new widgets::StatusLabel(detail_body_);
        attach_row->addWidget(attached_label_);
        attach_row->addStretch(1);
        detail_layout->addLayout(attach_row);
        detail_layout->addStretch(1);

        detail_panel->body()->addWidget(detail_body_);
        body->addWidget(detail_panel, 2);
        layout->addLayout(body, 1);

        connect(refresh_button_, &QPushButton::clicked, this, &ProcessListDialog::refresh);

        connect(search_edit_,
                &QLineEdit::textChanged,
                this,
                [this](const QString& text)
                {
                    restoring_ = true;
                    model_->set_search(text);
                    after_model_change();
                });

        connect(plugin_combo_,
                &QComboBox::currentIndexChanged,
                this,
                [this]
                {
                    restoring_ = true;
                    model_->set_plugin_filter(plugin_combo_->currentData().toString());
                    after_model_change();
                });

        connect(view_tabs_,
                &QTabBar::currentChanged,
                this,
                [this](int index)
                {
                    restoring_ = true;
                    model_->set_applications_only(index == 0);
                    after_model_change();
                    if (index == 0)
                    {
                        request_application_index();
                    }
                });

        connect(table_view_->selectionModel(),
                &QItemSelectionModel::currentRowChanged,
                this,
                [this]
                {
                    if (restoring_)
                    {
                        return;
                    }
                    if (const auto* info = selected(); info != nullptr)
                    {
                        selected_pid_ = info->pid;
                    }
                    else
                    {
                        selected_pid_ = 0;
                    }
                    selected_plugin_.clear();
                    probe_selection();
                    update_detail();
                    update_buttons();
                });

        connect(attach_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    const auto* info = selected();
                    if (info != nullptr && target_.valid() && target_.pid == info->pid)
                    {
                        detach_selected();
                    }
                    else
                    {
                        attach_selected();
                    }
                });

        // Enter must attach, not re-trigger the dialog's default button.
        refresh_button_->setAutoDefault(false);

        connect(search_edit_, &QLineEdit::returnPressed, this, &ProcessListDialog::attach_selected);

        connect(table_view_,
                &QTableView::activated,
                this,
                [this](const QModelIndex& index)
                {
                    table_view_->setCurrentIndex(index); // Enter on the table: honour the activated row.
                    attach_selected();
                });
    }

    void ProcessListDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        selected_pid_ = 0; // Re-open with the top result preselected.
        restoring_    = true;
        table_view_->selectionModel()->clearCurrentIndex();
        restoring_ = false;
        request_application_index();
        refresh();
        search_edit_->setFocus(Qt::OtherFocusReason);
    }

    void ProcessListDialog::set_status(std::string message, bool is_error)
    {
        status_text_     = std::move(message);
        status_is_error_ = is_error;
        update_status();
    }

    const process::ProcessInfo* ProcessListDialog::selected() const
    {
        return model_->process_at(table_view_->currentIndex().row());
    }

    std::string ProcessListDialog::chosen_plugin(const process::ProcessInfo& info) const
    {
        if (!selected_plugin_.empty()
            && std::find(info.claimants.begin(), info.claimants.end(), selected_plugin_) != info.claimants.end())
        {
            return selected_plugin_;
        }
        return info.plugin_id;
    }

    void ProcessListDialog::refresh()
    {
        if (list_pending_.has_value())
        {
            return; // A list job is already in flight.
        }

        const process::JobId job_id = worker_.next_job_id();
        list_pending_               = job_id;
        refresh_button_->setText(tr("Refreshing..."));
        refresh_button_->setEnabled(false);

        const bool submitted = worker_.submit_list(
            job_id,
            [this, job_id](process::JobResult&& result)
            {
                if (list_pending_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                list_pending_.reset();
                refresh_button_->setText(tr("Refresh"));
                refresh_button_->setEnabled(true);

                auto& listed = std::get<process::ListResult>(result);
                if (listed.error)
                {
                    set_status(std::string("could not list processes: ")
                                   + std::string(process::describe(*listed.error)),
                               true);
                    return;
                }

                plugin_ids_.clear();
                for (const auto& info : listed.processes)
                {
                    for (const auto& claimant : info.claimants)
                    {
                        if (std::find(plugin_ids_.begin(), plugin_ids_.end(), claimant) == plugin_ids_.end())
                        {
                            plugin_ids_.push_back(claimant);
                        }
                    }
                }
                std::ranges::sort(plugin_ids_);

                restoring_ = true;
                rebuild_plugin_filter();
                model_->set_processes(std::move(listed.processes));
                after_model_change();
            });
        if (!submitted)
        {
            list_pending_.reset();
            refresh_button_->setText(tr("Refresh"));
            refresh_button_->setEnabled(true);
            set_status("List unavailable.", true);
        }
    }

    void ProcessListDialog::rebuild_plugin_filter()
    {
        const QString        previous = plugin_combo_->currentData().toString();
        const QSignalBlocker blocker(plugin_combo_);

        plugin_combo_->clear();
        plugin_combo_->addItem(tr("All plugins"), QString());
        for (const auto& id : plugin_ids_)
        {
            plugin_combo_->addItem(to_qstring(id), to_qstring(id));
        }
        const int index = plugin_combo_->findData(previous);
        plugin_combo_->setCurrentIndex(index >= 0 ? index : 0);
        plugin_combo_->setVisible(!plugin_ids_.empty());

        // Keep the model's filter in step with the combo.
        model_->set_plugin_filter(plugin_combo_->currentData().toString());
    }

    void ProcessListDialog::request_application_index()
    {
        if (index_built_ || index_pending_.has_value())
        {
            return;
        }

        const process::JobId job_id = worker_.next_job_id();
        index_pending_              = job_id;

        const bool submitted = worker_.submit_application_index(
            job_id,
            [this, job_id](process::JobResult&& result)
            {
                if (index_pending_ != job_id)
                {
                    return;
                }
                index_pending_.reset();
                index_built_ = true;

                restoring_ = true;
                model_->set_application_index(std::move(std::get<process::AppIndexResult>(result).executables));
                after_model_change();
            });
        if (!submitted)
        {
            index_pending_.reset();
        }
    }

    void ProcessListDialog::after_model_change()
    {
        restore_selection();
        if (const auto* info = selected(); info != nullptr)
        {
            selected_pid_ = info->pid;
        }
        else
        {
            selected_pid_ = 0;
        }
        restoring_ = false;

        probe_selection();
        update_detail();
        update_buttons();
        update_status();
    }

    void ProcessListDialog::restore_selection()
    {
        int row = model_->row_for_pid(selected_pid_);
        if (row < 0 && model_->rowCount() > 0)
        {
            row = 0; // Top result of the current filtered/sorted view.
        }
        if (row < 0)
        {
            selected_pid_ = 0;
            table_view_->selectionModel()->clearCurrentIndex();
            return;
        }
        table_view_->setCurrentIndex(model_->index(row, ProcessListModel::pid));
    }

    void ProcessListDialog::probe_selection()
    {
        const auto* info = selected();
        if (info == nullptr)
        {
            detail_pid_value_    = -1;
            detail_methods_      = process::AccessMethod::none;
            detail_module_count_ = 0;
            detail_thread_count_ = 0;
            detail_error_message_.clear();
            return;
        }
        if (probe_pending_.has_value() && probe_pending_pid_ == static_cast<int>(info->pid))
        {
            return; // Already inspecting this process.
        }

        const process::ProcessId pid    = info->pid;
        const std::string        plugin = chosen_plugin(*info);

        // Clear the cached detail and show the placeholder until it lands.
        detail_pid_value_    = static_cast<int>(pid);
        detail_methods_      = process::AccessMethod::none;
        detail_module_count_ = 0;
        detail_thread_count_ = 0;
        detail_error_message_.clear();

        const process::JobId job_id = worker_.next_job_id();
        probe_pending_              = job_id;
        probe_pending_pid_          = static_cast<int>(pid);

        const bool submitted = worker_.submit_probe(
            job_id,
            pid,
            plugin,
            [this, pid, job_id](process::JobResult&& result)
            {
                if (probe_pending_ != job_id)
                {
                    return; // Superseded by a newer probe.
                }
                probe_pending_.reset();
                probe_pending_pid_ = -1;

                // Drop a result whose selection changed while it was in flight.
                const auto* current = selected();
                if (detail_pid_value_ != static_cast<int>(pid) || current == nullptr || current->pid != pid)
                {
                    return;
                }

                auto& probe = std::get<process::ProbeResult>(result);
                if (probe.error)
                {
                    detail_error_message_ =
                        std::string("cannot inspect: ") + std::string(process::describe(*probe.error));
                }
                else
                {
                    detail_methods_      = probe.method;
                    detail_module_count_ = probe.modules;
                    detail_thread_count_ = probe.threads;
                    if (probe.modules_error)
                    {
                        detail_error_message_ =
                            std::string("modules unavailable: ") + std::string(process::describe(*probe.modules_error));
                    }
                }
                update_detail();
            });
        if (!submitted)
        {
            probe_pending_.reset();
            probe_pending_pid_ = -1;
        }
    }

    void ProcessListDialog::attach_selected()
    {
        const auto* info = selected();
        if (info == nullptr)
        {
            set_status("Select a process first.", true);
            return;
        }
        if (attach_pending_.has_value())
        {
            return;
        }

        const process::ProcessId pid    = info->pid;
        const std::string        name   = info->name;
        const std::string        plugin = chosen_plugin(*info);

        const process::JobId job_id = worker_.next_job_id();
        attach_pending_             = job_id;
        update_buttons();

        const bool submitted = worker_.submit_attach_app(
            job_id,
            pid,
            plugin,
            [this, pid, name, job_id](process::JobResult&& result)
            {
                if (attach_pending_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                attach_pending_.reset();

                const auto& attached = std::get<process::AttachResult>(result);
                if (attached.error)
                {
                    target_.clear();
                    set_status(std::string("attach failed: ") + std::string(process::describe(*attached.error)), true);
                    update_buttons();
                    emit targetChanged();
                    return;
                }

                target_.clear();
                target_.pid          = pid;
                target_.name         = name;
                target_.plugin_id    = attached.info->plugin_id;
                target_.method       = attached.info->method;
                target_.session_live = true;
                set_status(target_.label() + "; methods: " + process::describe(target_.method), false);
                update_buttons();
                emit targetChanged();
                close(); // The picker has done its job once we are attached.
            });
        if (!submitted)
        {
            attach_pending_.reset();
            set_status("Attach unavailable.", true);
            update_buttons();
        }
    }

    void ProcessListDialog::detach_selected()
    {
        if (!target_.valid())
        {
            set_status("Not attached.", true);
            return;
        }
        if (detach_pending_.has_value())
        {
            return;
        }

        const process::JobId job_id = worker_.next_job_id();
        detach_pending_             = job_id;
        update_buttons();

        const bool submitted = worker_.submit_detach(job_id,
                                                     [this, job_id](process::JobResult&&)
                                                     {
                                                         if (detach_pending_ != job_id)
                                                         {
                                                             return;
                                                         }
                                                         detach_pending_.reset();
                                                         target_.clear();
                                                         set_status("Detached.", false);
                                                         update_buttons();
                                                         emit targetChanged();
                                                     });
        if (!submitted)
        {
            detach_pending_.reset();
            set_status("Detach unavailable.", true);
            update_buttons();
        }
    }

    void ProcessListDialog::update_buttons()
    {
        const auto* info        = selected();
        const bool  is_attached = info != nullptr && target_.valid() && target_.pid == info->pid;

        if (is_attached)
        {
            attach_button_->setText(detach_pending_.has_value() ? tr("Detaching...") : tr("Detach"));
        }
        else
        {
            attach_button_->setText(attach_pending_.has_value() ? tr("Attaching...") : tr("Attach"));
        }
        attach_button_->setEnabled(info != nullptr && !attach_pending_.has_value() && !detach_pending_.has_value());
        attached_label_->setVisible(is_attached);
        if (is_attached)
        {
            attached_label_->set_status(widgets::StatusKind::success, tr("attached"));
        }
    }

    void ProcessListDialog::update_detail()
    {
        const auto* info = selected();
        detail_hint_->setVisible(info == nullptr);
        detail_body_->setVisible(info != nullptr);
        if (info == nullptr)
        {
            return;
        }

        detail_pid_->setText(tr("PID: %1").arg(static_cast<qulonglong>(info->pid)));
        detail_name_->setText(tr("Name: %1").arg(to_qstring(info->name)));
        detail_executable_->setText(
            tr("Executable: %1").arg(info->exe_path.empty() ? tr("(unknown)") : to_qstring(info->exe_path)));
        detail_plugin_->setText(tr("Default plugin: %1").arg(to_qstring(info->plugin_id)));

        update_claimants(*info);

        const bool inspecting = probe_pending_.has_value() && probe_pending_pid_ == static_cast<int>(info->pid);
        detail_inspecting_->setVisible(inspecting);
        detail_access_->setVisible(!inspecting);
        detail_modules_->setVisible(!inspecting);
        detail_threads_->setVisible(!inspecting);

        detail_access_->setText(tr("Access methods: %1").arg(to_qstring(process::describe(detail_methods_))));
        detail_modules_->setText(tr("Modules: %1").arg(static_cast<qulonglong>(detail_module_count_)));
        detail_threads_->setText(tr("Threads: %1").arg(static_cast<qulonglong>(detail_thread_count_)));

        detail_error_->setVisible(!detail_error_message_.empty());
        if (!detail_error_message_.empty())
        {
            detail_error_->set_status(widgets::StatusKind::warning, to_qstring(detail_error_message_));
        }
    }

    void ProcessListDialog::update_claimants(const process::ProcessInfo& info)
    {
        while (QLayoutItem* item = claimants_layout_->takeAt(0))
        {
            delete item->widget();
            delete item;
        }

        const bool multiple = info.claimants.size() > 1;
        claimants_area_->setVisible(multiple);
        if (!multiple)
        {
            return;
        }

        for (const auto& claimant : info.claimants)
        {
            const bool is_default = claimant == info.plugin_id;
            const auto label      = to_qstring(claimant) + (is_default ? tr(" (default)") : QString());
            auto*      button     = new QRadioButton(label, claimants_area_);
            button->setChecked(chosen_plugin(info) == claimant);
            connect(button,
                    &QRadioButton::clicked,
                    this,
                    [this, claimant]
                    {
                        selected_plugin_ = claimant;
                        probe_selection();
                        update_detail();
                    });
            claimants_layout_->addWidget(button);
        }
    }

    void ProcessListDialog::update_status()
    {
        if (!status_text_.empty())
        {
            status_->set_status(status_is_error_ ? widgets::StatusKind::error : widgets::StatusKind::info,
                                to_qstring(status_text_));
            return;
        }
        status_->set_status(widgets::StatusKind::info,
                            tr("%1 process(es) shown").arg(model_ != nullptr ? model_->rowCount() : 0));
    }

} // namespace slopkit::ui::dialogs
