#include "ui/panels/address_list_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMenu>
#include <QTableView>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "scan/types.hpp"
#include "table/serializer.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/message_box.hpp"
#include "ui/components/row_menu.hpp"
#include "ui/dialogs/add_script.hpp"
#include "ui/models/address_table_model.hpp"
#include "ui/table_file.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::panels
{

    namespace
    {
        // A log record is one line (docs/LOGGING.md), so a message that carries
        // newlines - a printed string that has one, or a Lua error with its
        // traceback - is flattened before it is logged or shown.
        std::string one_line(std::string_view text)
        {
            std::string line {text};
            std::ranges::replace(line, '\n', ' ');
            std::ranges::replace(line, '\r', ' ');
            return line;
        }

        // The interesting part of a Lua error: its message, without the
        // traceback that follows it.
        std::string error_message(std::string_view text)
        {
            return one_line(text.substr(0, text.find('\n')));
        }

        // A script's own output is the run's payload: one record per line.
        void log_script_lines(const std::vector<std::string>& lines)
        {
            for (const std::string& line : lines)
            {
                log::info(log::category::script, one_line(line));
            }
        }
    } // namespace

    AddressListPanel::AddressListPanel(table::AddressTable&     table,
                                       process::AccessWorker&   worker,
                                       process::AttachedTarget& target,
                                       QWidget*                 parent)
        : QWidget(parent), table_(table), worker_(worker), target_(target)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        model_      = new models::AddressTableModel(table_, worker, target, this);
        table_view_ = new QTableView(this);
        table_view_->setModel(model_);
        table_view_->setItemDelegate(new widgets::ElidedTooltipDelegate(table_view_));
        table_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_view_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_view_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        table_view_->setDragDropMode(QAbstractItemView::InternalMove);
        table_view_->setDefaultDropAction(Qt::MoveAction);
        table_view_->setDropIndicatorShown(true);
        table_view_->setAlternatingRowColors(true);
        table_view_->verticalHeader()->setVisible(false);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::description,
                                                              QHeaderView::Stretch);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::address,
                                                              QHeaderView::ResizeToContents);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::type,
                                                              QHeaderView::ResizeToContents);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::value, QHeaderView::Stretch);
        table_view_->horizontalHeader()->setSectionResizeMode(models::AddressTableModel::active,
                                                              QHeaderView::ResizeToContents);
        layout->addWidget(table_view_, 1);

        connect(table_view_->selectionModel(),
                &QItemSelectionModel::currentRowChanged,
                this,
                [this](const QModelIndex& current)
                {
                    table_.set_selected(current.isValid() ? current.row() : -1);
                    refresh();
                });

        connect(model_,
                &models::AddressTableModel::statusChanged,
                this,
                [this](const QString& message, bool is_error)
                {
                    set_status(message, is_error);
                });

        connect(model_,
                &models::AddressTableModel::scriptActiveRequested,
                this,
                [this](std::size_t row, bool wanted)
                {
                    toggle_script_active(row, wanted);
                });

        table_view_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(table_view_,
                &QWidget::customContextMenuRequested,
                this,
                [this](const QPoint& position)
                {
                    show_context_menu(position);
                });

        refresh();
    }

    QWidget* AddressListPanel::tab_order_first() const noexcept
    {
        return table_view_;
    }

    void AddressListPanel::set_status(const QString& message, bool is_error)
    {
        if (message == status_ && is_error == status_is_error_)
        {
            return;
        }
        status_          = message;
        status_is_error_ = is_error;
        emit statusChanged(message, is_error);
    }

    void AddressListPanel::refresh()
    {
        model_->refresh();
        maybe_resolve_expressions();

        // The engine and a script's globals are dropped with the target, so a
        // ticked script row would be a lie once it detaches. Clearing the flag
        // is itself the transition record, so the sweep is a no-op afterwards.
        if (target_.valid())
        {
            return;
        }
        bool cleared = false;
        for (std::size_t row = 0; row < table_.size(); ++row)
        {
            table::AddressEntry& entry = table_.entries()[row];
            if (entry.kind != table::EntryKind::script || !entry.active)
            {
                continue;
            }
            entry.active = false;
            cleared      = true;
            log::info(log::category::script,
                      std::format("script '{}' deactivated: the target detached", entry.description));
            model_->note_entry_changed(row);
        }
        if (cleared)
        {
            set_status(tr("Scripts deactivated: the target detached."), false);
        }
    }

    std::vector<ui::LiveRequest> AddressListPanel::next_live_request()
    {
        return model_->next_live_request();
    }

    void AddressListPanel::apply_live_readings(std::span<const ui::LiveReading> readings)
    {
        model_->apply_live_readings(readings);
    }

    void AddressListPanel::set_modules(std::vector<process::ModuleInfo> modules)
    {
        modules_.set_modules(modules);
        model_->set_modules(std::move(modules));
        // A new memory map means new bases, so resolve at the next tick.
        force_resolve_ = true;
    }

    void AddressListPanel::maybe_resolve_expressions()
    {
        if (resolving_ || !target_.valid())
        {
            return;
        }

        std::vector<process::ResolveRequest> requests;
        std::string                          signature;
        for (const table::AddressEntry& entry : table_.entries())
        {
            if (entry.expression.empty())
            {
                continue;
            }
            requests.push_back(process::ResolveRequest {.key = entry.id, .expression = entry.expression});
            signature += std::to_string(entry.id);
            signature += ':';
            signature += entry.expression;
            signature += ';';
        }

        constexpr auto kResolveInterval = std::chrono::seconds(1);
        const auto     now              = std::chrono::steady_clock::now();
        if (requests.empty())
        {
            expression_signature_.clear();
            force_resolve_ = false;
            return;
        }
        // A new or edited expression resolves at once; otherwise the interval
        // throttles the repeated pointer reads.
        if (!force_resolve_ && signature == expression_signature_ && now - last_resolve_ < kResolveInterval)
        {
            return;
        }
        force_resolve_        = false;
        expression_signature_ = signature;

        const process::JobId id = worker_.next_job_id();
        resolve_job_            = id;
        resolving_              = true;
        last_resolve_           = now;

        // Keep the submitted texts so a completion can be validated against the
        // current entries (an entry edited meanwhile is not overwritten).
        const auto submitted_requests = requests;
        const bool submitted          = worker_.submit_resolve_expressions(
            id,
            std::move(requests),
            ui::module_refs(modules_),
            8,
            [this, id, submitted_requests](process::JobResult&& result)
            {
                if (resolve_job_ != id)
                {
                    return; // a later pass superseded this one
                }
                resolve_job_.reset();
                resolving_ = false;

                const auto& resolved = std::get<process::ResolveResult>(result);
                if (resolved.error.has_value())
                {
                    return; // detached between submit and completion
                }

                auto    entries = table_.entries();
                bool    changed = false;
                QString first_error;
                for (const process::ResolveItemResult& item : resolved.items)
                {
                    const auto request = std::find_if(submitted_requests.begin(),
                                                      submitted_requests.end(),
                                                      [&](const process::ResolveRequest& candidate)
                                                      {
                                                          return candidate.key == item.key;
                                                      });
                    if (request == submitted_requests.end())
                    {
                        continue;
                    }
                    const auto entry = std::find_if(entries.begin(),
                                                    entries.end(),
                                                    [&](const table::AddressEntry& candidate)
                                                    {
                                                        return candidate.id == item.key;
                                                    });
                    // Drop an entry removed or edited while the resolve ran.
                    if (entry == entries.end() || entry->expression != request->expression)
                    {
                        continue;
                    }
                    if (item.address.has_value())
                    {
                        if (entry->address != *item.address)
                        {
                            entry->address = *item.address;
                            changed        = true;
                        }
                    }
                    else if (first_error.isEmpty())
                    {
                        first_error = QString::fromStdString(item.error);
                    }
                }

                if (changed)
                {
                    model_->refresh();
                }
                if (!first_error.isEmpty())
                {
                    log::warning(log::category::ui,
                                 std::format("expression resolve failed: {}", first_error.toStdString()));
                    set_status(tr("Expression resolve failed: %1").arg(first_error), true);
                }
            });
        if (!submitted)
        {
            resolve_job_.reset();
            resolving_ = false;
            set_status(tr("Expression resolve could not be started."), true);
        }
    }

    void AddressListPanel::set_address_mode(ui::AddressMode mode)
    {
        model_->set_address_mode(mode);
    }

    void AddressListPanel::set_table_path(const QString& path)
    {
        if (table_path_ == path)
        {
            return;
        }
        table_path_ = path;
        emit tablePathChanged(path);
    }

    QString AddressListPanel::table_path() const
    {
        return table_path_;
    }

    void AddressListPanel::set_dialog_directory(const QString& directory)
    {
        dialog_directory_ = directory;
    }

    QString AddressListPanel::suggested_table_path() const
    {
        // An existing table keeps its file name; only the directory can change.
        if (!table_path_.isEmpty())
        {
            const QString name = QFileInfo(table_path_).fileName();
            return dialog_directory_.isEmpty() ? table_path_ : QDir(dialog_directory_).filePath(name);
        }

        const std::string_view target_name = target_.valid() ? std::string_view(target_.name) : std::string_view {};
        const QString          directory = dialog_directory_.isEmpty() ? default_table_directory() : dialog_directory_;
        return QDir(directory).filePath(table_name_for_target(target_name));
    }

    void AddressListPanel::set_save_path_prompt(SavePathPrompt prompt)
    {
        save_path_prompt_ = std::move(prompt);
    }

    std::optional<QString> AddressListPanel::default_save_path_prompt(QWidget* parent, const QString& suggested)
    {
        const QString chosen = QFileDialog::getSaveFileName(
            parent, tr("Save Table As"), suggested, tr("Address tables (*.skt);;All files (*)"));
        if (chosen.isEmpty())
        {
            return std::nullopt;
        }
        return chosen;
    }

    std::optional<QString> AddressListPanel::choose_table_path()
    {
        log::debug(log::category::ui, "open table requested");
        const QString start = dialog_directory_.isEmpty() ? table_path_ : dialog_directory_;
        const QString path =
            QFileDialog::getOpenFileName(this, tr("Open Table"), start, tr("Address tables (*.skt);;All files (*)"));
        if (path.isEmpty())
        {
            return std::nullopt;
        }
        return path;
    }

    std::optional<table::AddressTable> AddressListPanel::parse_table(const QString& path)
    {
        table::AddressTable parsed;
        if (const auto result = table::load(std::filesystem::path(path.toStdString()), parsed); !result)
        {
            log::warning(log::category::ui, std::format("table load failed: {}", result.error()));
            set_status(tr("Open failed: %1").arg(to_qstring(result.error())), true);
            return std::nullopt;
        }
        return parsed;
    }

    void AddressListPanel::adopt_table(const QString& path, table::AddressTable&& loaded)
    {
        set_table_path(path);
        table_ = std::move(loaded);
        model_->refresh();
        set_status(tr("Loaded %1 entries.").arg(static_cast<qulonglong>(table_.size())), false);
        emit tableLoaded();
    }

    table::MergeSummary AddressListPanel::merge_table(const table::AddressTable& incoming)
    {
        const auto summary = table_.merge(incoming.entries());
        model_->refresh();
        set_status(tr("Merged: %1 added, %2 skipped.")
                       .arg(static_cast<qulonglong>(summary.added))
                       .arg(static_cast<qulonglong>(summary.skipped)),
                   false);
        return summary;
    }

    bool AddressListPanel::load_table(const QString& path)
    {
        auto parsed = parse_table(path);
        if (!parsed)
        {
            // The path is still remembered, so a later save keeps the target.
            set_table_path(path);
            return false;
        }
        adopt_table(path, std::move(*parsed));
        return true;
    }

    void AddressListPanel::save_table()
    {
        log::debug(log::category::ui, "save table requested");
        if (table_path_.isEmpty())
        {
            save_table_as();
            return;
        }
        save_to_path(table_path_);
    }

    void AddressListPanel::save_table_as()
    {
        log::debug(log::category::ui, "save table as requested");
        // Only the first save of a never-saved table with no remembered
        // directory creates the default folder, so the chooser can start there.
        if (table_path_.isEmpty() && dialog_directory_.isEmpty())
        {
            const QString directory = default_table_directory();
            if (!ensure_table_directory(directory))
            {
                log::warning(log::category::ui,
                             std::format("table directory could not be created: {}", directory.toStdString()));
                set_status(tr("Could not create %1.").arg(directory), true);
            }
        }
        const std::optional<QString> chosen = save_path_prompt_(this, suggested_table_path());
        if (!chosen.has_value() || chosen->isEmpty())
        {
            return;
        }
        save_to_path(table_file_path(*chosen));
    }

    void AddressListPanel::save_to_path(const QString& path)
    {
        set_table_path(path);
        if (const auto result = table::save(std::filesystem::path(path.toStdString()), table_); !result)
        {
            set_status(tr("Save failed: %1").arg(to_qstring(result.error())), true);
            return;
        }
        set_status(tr("Saved %1 entries.").arg(static_cast<qulonglong>(table_.size())), false);
    }

    void AddressListPanel::delete_selected()
    {
        const int row = table_.selected();
        if (row < 0)
        {
            log::warning(log::category::ui, "delete requested with no selection");
            set_status(tr("Select an entry to delete."), true);
            return;
        }

        const auto& entry   = table_.entries()[static_cast<std::size_t>(row)];
        const auto  address = model_->address_text(entry.address);
        const auto  label   = entry.description.empty() ? address : to_qstring(entry.description);

        if (!widgets::confirm(this, tr("Delete Entry"), tr("Delete %1?").arg(label)))
        {
            return;
        }

        table_.remove(static_cast<std::size_t>(row));
        set_status(tr("Entry deleted."), false);
    }

    void AddressListPanel::toggle_active_selected()
    {
        const int row = table_.selected();
        if (row < 0)
        {
            log::warning(log::category::ui, "active toggle requested with no selection");
            set_status(tr("Select an entry to activate."), true);
            return;
        }

        const std::size_t index = static_cast<std::size_t>(row);
        if (table_.entries()[index].kind == table::EntryKind::script)
        {
            // A script row toggles through its hooks, exactly like the checkable
            // Active cell, rather than flipping the flag in place.
            toggle_script_active(index, !table_.entries()[index].active);
            return;
        }

        auto& entry  = table_.entries()[index];
        entry.active = !entry.active;
        log::info(log::category::ui, entry.active ? "entry active" : "entry inactive");
        set_status(entry.active ? tr("Entry active.") : tr("Entry inactive."), false);
    }

    void AddressListPanel::ensure_script_dialog()
    {
        if (script_dialog_ == nullptr)
        {
            script_dialog_ = new dialogs::AddScriptDialog(table_, this);
        }
    }

    void AddressListPanel::add_script()
    {
        log::debug(log::category::ui, "add script dialog opened");
        ensure_script_dialog();
        script_dialog_->reset_for_add();
        script_dialog_->show();
        script_dialog_->raise();
        script_dialog_->activateWindow();
    }

    void AddressListPanel::add_hook_script(const QString& description, const QString& source)
    {
        log::debug(log::category::ui, "add hook script dialog opened");
        ensure_script_dialog();
        script_dialog_->reset_for_add(description, source);
        script_dialog_->show();
        script_dialog_->raise();
        script_dialog_->activateWindow();
    }

    void AddressListPanel::edit_script(std::size_t row)
    {
        if (!table_.valid_index(row) || table_.entries()[row].kind != table::EntryKind::script)
        {
            return;
        }
        log::debug(log::category::ui, std::format("edit script dialog opened on row {}", row));
        ensure_script_dialog();
        script_dialog_->edit_entry(row);
        script_dialog_->show();
        script_dialog_->raise();
        script_dialog_->activateWindow();
    }

    void AddressListPanel::run_script(std::size_t row)
    {
        if (!table_.valid_index(row) || table_.entries()[row].kind != table::EntryKind::script)
        {
            return;
        }
        // The Lua chunk touches the target, so it needs a live session; the
        // worker re-checks this before it runs anything.
        if (!target_.valid())
        {
            log::warning(log::category::script, "run script refused: no attached target");
            set_status(tr("Run Script needs an attached target."), true);
            return;
        }
        if (script_job_.has_value())
        {
            log::warning(log::category::script, "run script refused: a script is already running");
            set_status(tr("A script is already running."), true);
            return;
        }

        const table::AddressEntry& entry = table_.entries()[row];
        const process::JobId       id    = worker_.next_job_id();
        script_job_                      = id;
        log::info(log::category::script,
                  std::format("running script '{}' ({} byte(s))", entry.description, entry.script.size()));
        set_status(tr("Running script…"), false);

        const bool submitted = worker_.submit_script(id,
                                                     entry.description,
                                                     entry.script,
                                                     std::string {},
                                                     8,
                                                     [this, id](process::JobResult&& result)
                                                     {
                                                         finish_script(id, std::move(result));
                                                     });
        if (!submitted)
        {
            script_job_.reset();
            log::warning(log::category::script, "run script refused: the access worker is not accepting jobs");
            set_status(tr("The script job could not be started."), true);
        }
    }

    void AddressListPanel::finish_script(process::JobId id, process::JobResult&& result)
    {
        if (script_job_ != id)
        {
            return; // a later run superseded this one
        }
        script_job_.reset();

        const auto& ran = std::get<process::ScriptResult>(result);
        log_script_lines(ran.run.output);

        if (ran.run.ok)
        {
            // A script may have registered a symbol, so re-resolve at once
            // instead of waiting for a module-map change.
            force_resolve_ = true;
            set_status(ran.run.returned.empty() ? tr("Script ok.")
                                                : tr("Script returned %1.").arg(to_qstring(ran.run.returned)),
                       false);
            return;
        }

        // The failure is logged exactly once, here, without its traceback.
        const std::string message = error_message(ran.run.error);
        log::warning(log::category::script, message);
        set_status(tr("Script failed: %1").arg(to_qstring(message)), true);
    }

    void AddressListPanel::toggle_script_active(std::size_t row, bool wanted)
    {
        if (!table_.valid_index(row) || table_.entries()[row].kind != table::EntryKind::script)
        {
            return;
        }

        // Any refusal must snap the clicked box back to the table's state.
        const auto revert = [this, row]
        {
            model_->note_entry_changed(row);
        };

        // The hook touches the target, so it needs a live session; the worker
        // re-checks this before it runs anything.
        if (!target_.valid())
        {
            log::warning(log::category::script, "activate refused: no attached target");
            set_status(tr("Activate needs an attached target."), true);
            revert();
            return;
        }
        if (script_job_.has_value())
        {
            log::warning(log::category::script, "activate refused: a script is already running");
            set_status(tr("A script is already running."), true);
            revert();
            return;
        }

        const table::AddressEntry& entry = table_.entries()[row];
        const process::JobId       id    = worker_.next_job_id();
        script_job_                      = id;
        const std::string hook   = wanted ? std::string(script::kActivateHook) : std::string(script::kDeactivateHook);
        const std::string action = wanted ? "activating" : "deactivating";
        log::info(log::category::script, std::format("{} script '{}'", action, entry.description));
        set_status(wanted ? tr("Activating…") : tr("Deactivating…"), false);

        const std::uint64_t entry_id  = entry.id;
        const bool          submitted = worker_.submit_script(id,
                                                              entry.description,
                                                              entry.script,
                                                              hook,
                                                              8,
                                                              [this, id, entry_id, wanted](process::JobResult&& job)
                                                              {
                                                         finish_script_active(id, entry_id, wanted, std::move(job));
                                                              });
        if (!submitted)
        {
            script_job_.reset();
            log::warning(log::category::script, "activate refused: the access worker is not accepting jobs");
            set_status(tr("The script job could not be started."), true);
            revert();
        }
    }

    void AddressListPanel::finish_script_active(process::JobId       id,
                                                std::uint64_t        entry_id,
                                                bool                 wanted,
                                                process::JobResult&& result)
    {
        if (script_job_ != id)
        {
            return; // a later job superseded this one
        }
        script_job_.reset();

        const auto& ran = std::get<process::ScriptResult>(result);
        if (!ran.lifecycle.has_value())
        {
            return; // a plain run's completion, not ours
        }
        const script::LifecycleResult& outcome = *ran.lifecycle;
        log_script_lines(outcome.output);

        // A reorder or a delete may have landed between the click and the
        // completion, so the row is re-located by its entry id.
        std::optional<std::size_t> row;
        for (std::size_t index = 0; index < table_.size(); ++index)
        {
            if (table_.entries()[index].id == entry_id)
            {
                row = index;
                break;
            }
        }

        const char* action = wanted ? "activate" : "deactivate";

        if (outcome.ok)
        {
            // A script may have registered a symbol, so re-resolve at once
            // instead of waiting for a module-map change.
            force_resolve_ = true;
            if (row.has_value())
            {
                table_.entries()[*row].active = wanted;
                model_->note_entry_changed(*row);
            }
            log::info(log::category::script,
                      std::format("script '{}' {}: {}",
                                  ran.description,
                                  wanted ? "activated" : "deactivated",
                                  outcome.message.empty() ? "ok" : outcome.message));
            if (outcome.message.empty())
            {
                set_status(wanted ? tr("Script active.") : tr("Script inactive."), false);
            }
            else
            {
                const QString pattern = wanted ? tr("Script active: %1") : tr("Script inactive: %1");
                set_status(pattern.arg(to_qstring(outcome.message)), false);
            }
            return;
        }

        // Failure: the flag stays as it was, so the box snaps back.
        if (row.has_value())
        {
            model_->note_entry_changed(*row);
        }
        std::string reason = outcome.message;
        if (reason.empty())
        {
            reason = error_message(outcome.error);
        }
        if (reason.empty())
        {
            log::warning(log::category::script, std::format("script '{}' {} failed", ran.description, action));
            set_status(wanted ? tr("Activate failed.") : tr("Deactivate failed."), true);
        }
        else
        {
            log::warning(log::category::script,
                         std::format("script '{}' {} failed: {}", ran.description, action, reason));
            const QString pattern = wanted ? tr("Activate failed: %1") : tr("Deactivate failed: %1");
            set_status(pattern.arg(to_qstring(reason)), true);
        }
    }

    void AddressListPanel::report_status(std::string_view message, bool is_error)
    {
        set_status(to_qstring(message), is_error);
    }

    void AddressListPanel::report_active_error(std::string_view message)
    {
        report_status(std::format("Active write failed: {}", message), true);
    }

    void AddressListPanel::note_script_update_failed(const process::ScriptUpdateFailure& failure)
    {
        // The worker already deactivated the script and stopped ticking it, so
        // only its row's flag and the report are left. The row is located by
        // description, which is what the worker tracked the script under.
        std::optional<std::size_t> row;
        for (std::size_t index = 0; index < table_.size(); ++index)
        {
            const table::AddressEntry& entry = table_.entries()[index];
            if (entry.kind == table::EntryKind::script && entry.description == failure.description)
            {
                row = index;
                break;
            }
        }

        // Exactly one warning, whether or not the row is still there to update.
        if (failure.reason.empty())
        {
            log::warning(log::category::script, std::format("script '{}' update failed", failure.description));
        }
        else
        {
            log::warning(log::category::script,
                         std::format("script '{}' update failed: {}", failure.description, failure.reason));
        }

        // A row the detach sweep already cleared, or one whose manual hook job
        // is in flight, only gets the record: this class must not touch it, and
        // it must not submit a second deactivate either.
        if (!row.has_value() || !table_.entries()[*row].active || script_job_.has_value())
        {
            return;
        }

        table_.entries()[*row].active = false;
        model_->note_entry_changed(*row);
        set_status(failure.reason.empty() ? tr("Update failed.")
                                          : tr("Update failed: %1").arg(to_qstring(failure.reason)),
                   true);
    }

    void AddressListPanel::show_context_menu(const QPoint& position)
    {
        QMenu menu(this);
        populate_context_menu(menu, position);
        menu.exec(table_view_->viewport()->mapToGlobal(position));
    }

    void AddressListPanel::populate_context_menu(QMenu& menu, const QPoint& position)
    {
        const QModelIndex index = table_view_->indexAt(position);
        if (index.isValid() && table_.valid_index(static_cast<std::size_t>(index.row())))
        {
            table_.set_selected(index.row());
            populate_row_menu(menu, static_cast<std::size_t>(index.row()));
            return;
        }

        // A click below the rows shows the table-area commands alone and leaves
        // the current selection untouched.
        populate_panel_menu(menu);
    }

    void AddressListPanel::populate_panel_menu(QMenu& menu)
    {
        QAction* add = menu.addAction(tr("Add Address Manually…"));
        connect(add, &QAction::triggered, this, &AddressListPanel::addAddressRequested);

        QAction* script = menu.addAction(tr("Add Script…"));
        connect(script, &QAction::triggered, this, &AddressListPanel::add_script);

        QAction* settings = menu.addAction(tr("Table Settings…"));
        connect(settings, &QAction::triggered, this, &AddressListPanel::tableSettingsRequested);
    }

    void AddressListPanel::populate_row_menu(QMenu& menu, std::size_t row)
    {
        if (!table_.valid_index(row))
        {
            return;
        }
        auto& entry = table_.entries()[row];

        widgets::show_explanations(menu);

        // A script row has no address and no value, so it gets its own two
        // commands instead of the value-row set; both row kinds then share the
        // table-area tail.
        if (entry.kind == table::EntryKind::script)
        {
            QAction* run = menu.addAction(tr("Run Script"));
            connect(run,
                    &QAction::triggered,
                    this,
                    [this, row]
                    {
                        run_script(row);
                    });

            QAction* edit = menu.addAction(tr("Edit Script…"));
            connect(edit,
                    &QAction::triggered,
                    this,
                    [this, row]
                    {
                        edit_script(row);
                    });

            menu.addSeparator();
            QAction* remove_script = menu.addAction(tr("Delete"));
            connect(remove_script, &QAction::triggered, this, &AddressListPanel::delete_selected);
        }
        else
        {
            QAction* change_value = menu.addAction(tr("Change value"));
            connect(change_value,
                    &QAction::triggered,
                    this,
                    [this, row]
                    {
                        if (table_.valid_index(row))
                        {
                            table_view_->edit(model_->index(static_cast<int>(row), models::AddressTableModel::value));
                        }
                    });

            QAction* active = menu.addAction(tr("Active"));
            active->setCheckable(true);
            active->setChecked(entry.active);
            connect(active,
                    &QAction::triggered,
                    this,
                    [this, row](bool checked)
                    {
                        if (!table_.valid_index(row))
                        {
                            return;
                        }
                        auto& selected  = table_.entries()[row];
                        selected.active = checked;
                        set_status(selected.active ? tr("Entry active.") : tr("Entry inactive."), false);
                    });

            QAction* show_hex = menu.addAction(tr("Show as hex"));
            show_hex->setCheckable(true);
            show_hex->setChecked(entry.hex);
            connect(show_hex,
                    &QAction::triggered,
                    this,
                    [this, row](bool checked)
                    {
                        if (!table_.valid_index(row))
                        {
                            return;
                        }
                        table_.entries()[row].hex = checked;
                        set_status(tr("Display format updated."), false);
                    });

            QAction* browse = menu.addAction(tr("Browse this memory region"));
            connect(browse,
                    &QAction::triggered,
                    this,
                    [this, row]
                    {
                        if (table_.valid_index(row))
                        {
                            emit browseRequested(table_.entries()[row].address);
                        }
                    });

            // The watch entries arm a hardware slot, so they need a target.
            const std::size_t width =
                entry.bytes.empty() ? scan::value_size(entry.type) : static_cast<std::size_t>(entry.bytes.size());
            widgets::add_watch_commands(menu,
                                        target_.valid(),
                                        [this, row, width](widgets::WatchCommand command)
                                        {
                                            if (!table_.valid_index(row))
                                            {
                                                return;
                                            }
                                            const debug::Kind kind = command == widgets::WatchCommand::writes
                                                                       ? debug::Kind::hardware_write
                                                                       : debug::Kind::hardware_read_write;
                                            emit accessWatchRequested(table_.entries()[row].address, width, kind);
                                        });
            (void)widgets::disabled_action(menu, tr("Group"), tr("Disabled: address groups are not implemented"));

            menu.addSeparator();
            QAction* remove = menu.addAction(tr("Delete"));
            connect(remove, &QAction::triggered, this, &AddressListPanel::delete_selected);
        }

        // The table-area commands follow the row's own entries, after their own
        // separator, on a value row and a script row alike
        // (docs/UI_DESIGN.md#windows-dialogs-and-layout).
        menu.addSeparator();
        populate_panel_menu(menu);
    }

} // namespace slopkit::ui::panels
