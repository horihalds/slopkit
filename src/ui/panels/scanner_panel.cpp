#include "ui/panels/scanner_panel.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/address_format.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::panels
{
    namespace
    {
        std::size_t default_alignment(scan::ValueType type) noexcept
        {
            const std::size_t size = scan::value_size(type);
            return size == 0 ? 4 : size;
        }
    }

    ScannerPanel::ScannerPanel(process::AccessWorker& worker, process::AttachedTarget& target, QWidget* parent)
        : QWidget(parent), worker_(worker), target_(target)
    {
        setMinimumWidth(320);
        build_layout();
        apply_range(0, scan::kMaxUserAddress);
        connect_widgets();
        refresh();
    }

    ScannerPanel::~ScannerPanel()
    {
        if (target_suspended_)
        {
            worker_.submit_resume(worker_.next_job_id(), worker_pid_, [](process::JobResult&&) {});
        }
    }

    scan::ScanEngine& ScannerPanel::engine() noexcept
    {
        return engine_;
    }

    int ScannerPanel::progress_percent() const noexcept
    {
        return progress_percent_;
    }

    void ScannerPanel::focus_value_input()
    {
        if (!value_edit_->isEnabled())
        {
            return;
        }
        value_edit_->setFocus(Qt::OtherFocusReason);
        value_edit_->selectAll();
    }

    void ScannerPanel::prime_value(const QString& text, int bytes)
    {
        value_edit_->setText(text);

        // The value type combo is indexed by scan::ValueType, so the byte count
        // picks the widest type that fits a node's storage.
        scan::ValueType type = scan::ValueType::byte_array;
        switch (bytes)
        {
        case 1:
            type = scan::ValueType::byte;
            break;
        case 2:
            type = scan::ValueType::int16;
            break;
        case 4:
            type = scan::ValueType::int32;
            break;
        case 8:
            type = scan::ValueType::int64;
            break;
        default:
            break;
        }
        value_type_combo_->setCurrentIndex(static_cast<int>(type));
        update_value_inputs();
        focus_value_input();
    }

    std::uint64_t ScannerPanel::main_module_address() const noexcept
    {
        if (!map_ready_)
        {
            return 0;
        }
        const process::ModuleInfo* main_module = process::main_module(modules_);
        if (main_module == nullptr)
        {
            return 0;
        }
        return main_module->entry != 0 ? main_module->entry : main_module->base;
    }

    QWidget* ScannerPanel::tab_order_last() const noexcept
    {
        return pause_scanning_check_;
    }

    void ScannerPanel::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        layout->addWidget(widgets::section_header(tr("Scan"), this));

        auto* action_row  = new QHBoxLayout();
        scan_button_      = new widgets::PrimaryButton(tr("First Scan"), this);
        next_scan_button_ = widgets::secondary_button(tr("Next Scan"), this);
        undo_button_      = widgets::secondary_button(tr("Undo Scan"), this);
        cancel_button_    = widgets::secondary_button(tr("Cancel"), this);
        cancel_button_->setVisible(false);
        action_row->addWidget(scan_button_, 36);
        action_row->addWidget(next_scan_button_, 32);
        action_row->addWidget(undo_button_, 32);
        action_row->addWidget(cancel_button_, 16);
        layout->addLayout(action_row);

        auto* value_row = new QHBoxLayout();
        hex_check_      = new QCheckBox(tr("Hex"), this);
        value_row->addWidget(hex_check_);

        value_edit_ = new QLineEdit(this);
        value_edit_->setPlaceholderText(tr("Value"));
        value_edit_->setFont(mono_font());
        value_row->addWidget(value_edit_, 1);

        value_upper_edit_ = new QLineEdit(this);
        value_upper_edit_->setPlaceholderText(tr("Upper value"));
        value_upper_edit_->setFont(mono_font());
        value_upper_edit_->setVisible(false);
        value_row->addWidget(value_upper_edit_, 1);
        layout->addLayout(value_row);

        scan_type_combo_ = new QComboBox(this);
        for (const char* name : scan::kScanTypeNames)
        {
            scan_type_combo_->addItem(QString::fromUtf8(name));
        }
        layout->addWidget(scan_type_combo_);

        value_type_combo_ = new QComboBox(this);
        for (const char* name : scan::kValueTypeNames)
        {
            value_type_combo_->addItem(QString::fromUtf8(name));
        }
        value_type_combo_->setCurrentIndex(2);
        layout->addWidget(value_type_combo_);

        auto* options = new widgets::Panel(tr("Memory Scan Options"), this);
        layout->addWidget(options);

        auto* options_body = options->body();

        auto* range_row = new QHBoxLayout();
        module_combo_   = new widgets::ScrollingComboBox();
        module_combo_->addItem(tr("All memory"));
        module_combo_->setEnabled(false);
        module_combo_->setFont(mono_font());
        module_combo_->setToolTip(tr("Whole process or a single loaded module"));
        range_row->addWidget(module_combo_, 1);
        options_body->addLayout(range_row);

        auto* address_row = new QHBoxLayout();
        start_edit_       = new QLineEdit();
        start_edit_->setPlaceholderText(tr("Start address"));
        start_edit_->setFont(mono_font());
        address_row->addWidget(start_edit_, 1);

        stop_edit_ = new QLineEdit();
        stop_edit_->setPlaceholderText(tr("Stop address"));
        stop_edit_->setFont(mono_font());
        address_row->addWidget(stop_edit_, 1);
        options_body->addLayout(address_row);

        auto* filter_row = new QHBoxLayout();
        writable_check_  = new QCheckBox(tr("Writable"));
        writable_check_->setChecked(true);
        executable_check_    = new QCheckBox(tr("Executable"));
        copy_on_write_check_ = new QCheckBox(tr("CopyOnWrite"));
        copy_on_write_check_->setChecked(true);
        filter_row->addWidget(writable_check_);
        filter_row->addWidget(executable_check_);
        filter_row->addWidget(copy_on_write_check_);
        filter_row->addStretch(1);
        options_body->addLayout(filter_row);

        auto* fast_row   = new QHBoxLayout();
        fast_scan_check_ = new QCheckBox(tr("Fast Scan"));
        fast_scan_check_->setChecked(true);
        alignment_edit_ = new QLineEdit(QString::number(default_alignment(scan::ValueType::int32)));
        alignment_edit_->setPlaceholderText(tr("Alignment"));
        alignment_edit_->setFont(mono_font());
        alignment_edit_->setMaximumWidth(96);
        alignment_edit_->setToolTip(tr("Alignment step in bytes (decimal or 0x hex); blank uses the value size"));
        fast_row->addWidget(fast_scan_check_);
        fast_row->addWidget(alignment_edit_);
        fast_row->addStretch(1);
        options_body->addLayout(fast_row);

        pause_scanning_check_ = new QCheckBox(tr("Pause the game while scanning"));
        pause_scanning_check_->setToolTip(
            tr("Freeze the game while a scan runs and resume it as soon as the scan finishes."));
        options_body->addWidget(pause_scanning_check_);

        layout->addStretch(1);

        update_value_inputs();
        apply_tab_order();
    }

    void ScannerPanel::apply_tab_order()
    {
        widgets::chain_tab_order({scan_button_,
                                  next_scan_button_,
                                  undo_button_,
                                  cancel_button_,
                                  hex_check_,
                                  value_edit_,
                                  value_upper_edit_,
                                  scan_type_combo_,
                                  value_type_combo_,
                                  module_combo_,
                                  start_edit_,
                                  stop_edit_,
                                  writable_check_,
                                  executable_check_,
                                  copy_on_write_check_,
                                  fast_scan_check_,
                                  alignment_edit_,
                                  pause_scanning_check_});
    }

    void ScannerPanel::connect_widgets()
    {
        connect(module_combo_,
                &QComboBox::currentIndexChanged,
                this,
                [this](int index)
                {
                    on_range_selected(index);
                });
        connect(scan_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    if (engine_.has_results())
                    {
                        log::debug(log::category::ui, "New Scan");
                        engine_.reset();
                        maybe_resume_target();
                    }
                    else
                    {
                        log::debug(log::category::ui, "First Scan");
                        start_first_scan();
                    }
                    refresh();
                });
        connect(next_scan_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    log::debug(log::category::ui, "Next Scan");
                    start_next_scan();
                    refresh();
                });
        connect(undo_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    log::debug(log::category::ui, "Undo Scan");
                    engine_.undo();
                    refresh();
                });
        connect(cancel_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    log::debug(log::category::ui, "Cancel Scan");
                    engine_.cancel();
                    maybe_resume_target();
                    refresh();
                });
        connect(value_edit_, &QLineEdit::returnPressed, this, &ScannerPanel::activate_scan_from_input);
        connect(hex_check_, &QCheckBox::toggled, this, &ScannerPanel::convert_value_base_fields);

        connect(scan_type_combo_,
                &QComboBox::currentIndexChanged,
                this,
                [this]
                {
                    update_value_inputs();
                });
        connect(value_type_combo_,
                &QComboBox::currentIndexChanged,
                this,
                [this]
                {
                    apply_default_alignment();
                    update_value_inputs();
                });
        connect(fast_scan_check_,
                &QCheckBox::toggled,
                this,
                [this]
                {
                    update_value_inputs();
                });
    }

    void ScannerPanel::update_value_inputs()
    {
        const scan::ScanType type        = current_scan_type();
        const bool           wants_value = scan::needs_value(type) || type == scan::ScanType::value_between;

        value_edit_->setEnabled(wants_value);
        hex_check_->setEnabled(wants_value);
        value_upper_edit_->setEnabled(wants_value);
        value_upper_edit_->setVisible(type == scan::ScanType::value_between);
        alignment_edit_->setEnabled(fast_scan_check_->isChecked());
    }

    void ScannerPanel::apply_default_alignment()
    {
        const scan::ValueType type     = current_value_type();
        const QString         current  = alignment_edit_->text().trimmed();
        const QString         previous = QString::number(default_alignment(alignment_value_type_));
        if (current.isEmpty() || current == previous)
        {
            alignment_edit_->setText(QString::number(default_alignment(type)));
        }
        alignment_value_type_ = type;
    }

    void ScannerPanel::convert_value_base_fields(bool to_hex)
    {
        const bool                      from_hex = !to_hex;
        const scan::ValueType           type     = current_value_type();
        const std::array<QLineEdit*, 2> edits {value_edit_, value_upper_edit_};
        for (QLineEdit* edit : edits)
        {
            const QString text = edit->text();
            if (text.trimmed().isEmpty())
            {
                continue;
            }
            const auto converted = scan::convert_value_base(type, text.toStdString(), from_hex, to_hex);
            if (converted.has_value())
            {
                edit->setText(to_qstring(*converted));
            }
        }
    }

    scan::ScanType ScannerPanel::current_scan_type() const noexcept
    {
        return static_cast<scan::ScanType>(scan_type_combo_->currentIndex());
    }

    scan::ValueType ScannerPanel::current_value_type() const noexcept
    {
        return static_cast<scan::ValueType>(value_type_combo_->currentIndex());
    }

    void ScannerPanel::request_scan_session()
    {
        const bool changed = target_.pid != worker_pid_ || target_.plugin_id != worker_plugin_;
        if (!changed || handoff_pending_.has_value())
        {
            return;
        }
        if (engine_.is_running())
        {
            engine_.cancel();
            return;
        }

        worker_session_ = process::Session {};
        worker_pid_     = 0;
        worker_plugin_.clear();
        can_suspend_ = false;
        if (!target_.valid())
        {
            clear_memory_map();
            return;
        }

        const process::ProcessId pid    = target_.pid;
        const std::string        plugin = target_.plugin_id;
        worker_pid_                     = pid;
        worker_plugin_                  = plugin;

        const process::JobId job_id = worker_.next_job_id();
        handoff_pending_            = job_id;
        log::info(log::category::scan, "Preparing scan session...");

        const bool submitted =
            worker_.submit_attach_handoff(job_id,
                                          pid,
                                          plugin,
                                          [this, job_id, pid, plugin](process::JobResult&& result)
                                          {
                                              if (handoff_pending_ != job_id)
                                              {
                                                  return;
                                              }
                                              handoff_pending_.reset();

                                              auto& attached = std::get<process::AttachResult>(result);
                                              if (!target_.valid() || target_.pid != pid || target_.plugin_id != plugin)
                                              {
                                                  return;
                                              }
                                              if (attached.error)
                                              {
                                                  log::warning(log::category::scan,
                                                               std::string("Scan session failed: ")
                                                                   + std::string(process::describe(*attached.error)));
                                                  return;
                                              }

                                              worker_session_ = std::move(*attached.handed_session);
                                              can_suspend_    = attached.info.has_value() && attached.info->can_suspend;
                                          });
        if (!submitted)
        {
            handoff_pending_.reset();
        }
    }

    void ScannerPanel::request_memory_map()
    {
        if (!target_.valid() || !worker_.attached() || map_pending_.has_value())
        {
            return;
        }

        const process::ProcessId pid    = target_.pid;
        const std::string        plugin = target_.plugin_id;
        if (pid != map_pid_ || plugin != map_plugin_)
        {
            // A new target replaces the map once its fetch lands; its fetch
            // starts a fresh attempt sequence and never inherits the previous
            // target's backoff.
            map_pid_            = pid;
            map_plugin_         = plugin;
            map_attempts_       = 0;
            map_failure_logged_ = false;
            map_retry_after_    = {};
        }
        else if (map_ready_ || std::chrono::steady_clock::now() < map_retry_after_)
        {
            return;
        }

        const process::JobId job_id = worker_.next_job_id();
        map_pending_                = job_id;

        if (modules_.empty() && map_attempts_ == 0)
        {
            range_updating_ = true;
            module_combo_->clear();
            module_combo_->addItem(tr("Loading…"));
            module_combo_->setEnabled(false);
            range_updating_ = false;
        }

        const bool submitted =
            worker_.submit_memory_map(job_id,
                                      [this, job_id, pid, plugin](process::JobResult&& result)
                                      {
                                          if (map_pending_ != job_id)
                                          {
                                              return;
                                          }
                                          map_pending_.reset();
                                          if (!target_.valid() || target_.pid != pid || target_.plugin_id != plugin)
                                          {
                                              return;
                                          }
                                          apply_memory_map(std::get<process::MemoryMapResult>(std::move(result)));
                                      });
        if (!submitted)
        {
            map_pending_.reset();
        }
    }

    void ScannerPanel::apply_memory_map(process::MemoryMapResult result)
    {
        if (result.error)
        {
            // A failed fetch is inert: whatever module list, range and dropdown
            // the previous success produced stay in place until a fetch works.
            note_memory_map_failure(*result.error);
            return;
        }

        map_attempts_       = 0;
        map_failure_logged_ = false;
        map_retry_after_    = {};
        modules_            = std::move(result.modules);
        map_ready_          = true;

        apply_range(0, scan::kMaxUserAddress);
        rebuild_range_items();

        emit memoryMapApplied(modules_, std::move(result.regions));
    }

    void ScannerPanel::note_memory_map_failure(process::AccessError error)
    {
        ++map_attempts_;

        // 250 ms, then doubling up to a 5 s cap: enough to ride out the Wine
        // resident agent's bring-up without hammering a permanently dead target.
        constexpr std::chrono::milliseconds initial_retry {250};
        constexpr std::chrono::milliseconds max_retry {5000};
        const auto                          shift = std::min<unsigned>(map_attempts_ - 1, 5);
        const auto                          delay = std::min(initial_retry * (1U << shift), max_retry);
        map_retry_after_                          = std::chrono::steady_clock::now() + delay;

        if (map_failure_logged_)
        {
            log::debug(log::category::scan,
                       std::format("retrying the memory map of pid {}: {}", target_.pid, process::describe(error)));
            return;
        }
        map_failure_logged_ = true;
        log::warning(log::category::scan,
                     std::format("could not load the memory map of pid {}: {}; retrying",
                                 target_.pid,
                                 process::describe(error)));
    }

    void ScannerPanel::apply_range(std::uint64_t start, std::uint64_t end)
    {
        start_edit_->setText(ui::format_padded_hex(start));
        stop_edit_->setText(ui::format_padded_hex(end));
    }

    void ScannerPanel::clear_memory_map()
    {
        map_pending_.reset();
        map_pid_ = 0;
        map_plugin_.clear();
        modules_.clear();
        map_ready_          = false;
        map_attempts_       = 0;
        map_retry_after_    = {};
        map_failure_logged_ = false;
        apply_range(0, scan::kMaxUserAddress);

        range_updating_ = true;
        module_combo_->clear();
        module_combo_->addItem(tr("All memory"));
        module_combo_->setItemData(0, QVariant::fromValue<qulonglong>(scan::kMaxUserAddress), Qt::UserRole + 1);
        module_combo_->setCurrentIndex(0);
        module_combo_->setEnabled(false);
        module_combo_->setToolTip(tr("Whole process or a single loaded module"));
        range_updating_ = false;

        emit memoryMapApplied({}, {});
    }

    void ScannerPanel::rebuild_range_items()
    {
        const auto range_text = [](std::uint64_t start, std::uint64_t end)
        {
            return ui::format_padded_hex(start) + QStringLiteral("-") + ui::format_padded_hex(end);
        };

        range_updating_ = true;
        module_combo_->clear();

        module_combo_->addItem(tr("All memory"));
        module_combo_->setItemData(0, QVariant::fromValue<qulonglong>(0), Qt::UserRole);
        module_combo_->setItemData(0, QVariant::fromValue<qulonglong>(scan::kMaxUserAddress), Qt::UserRole + 1);
        module_combo_->setItemData(0, range_text(0, scan::kMaxUserAddress), Qt::ToolTipRole);

        const process::ModuleInfo*              main = process::main_module(modules_);
        std::vector<const process::ModuleInfo*> listed;
        for (const auto& module : modules_)
        {
            if (!process::is_file_backed(module))
            {
                continue;
            }
            listed.push_back(&module);
        }
        std::ranges::sort(listed, {}, &process::ModuleInfo::base);

        if (main != nullptr)
        {
            const auto position = std::ranges::find(listed, main);
            if (position != listed.end() && position != listed.begin())
            {
                std::rotate(listed.begin(), position, position + 1);
            }
        }

        for (const process::ModuleInfo* module : listed)
        {
            const std::uint64_t start = module->base;
            const std::uint64_t end   = module->base + module->size;
            module_combo_->addItem(ui::elide_module_name(module->name));
            const int row = module_combo_->count() - 1;
            module_combo_->setItemData(row, QVariant::fromValue<qulonglong>(start), Qt::UserRole);
            module_combo_->setItemData(row, QVariant::fromValue<qulonglong>(end), Qt::UserRole + 1);
            module_combo_->setItemData(row, to_qstring(module->name), Qt::ToolTipRole);
        }

        module_combo_->setCurrentIndex(0);
        module_combo_->setEnabled(true);
        module_combo_->setToolTip(tr("Whole process or a single loaded module"));
        range_updating_ = false;
    }

    void ScannerPanel::on_range_selected(int index)
    {
        if (range_updating_ || index < 0)
        {
            return;
        }

        const QVariant start = module_combo_->itemData(index, Qt::UserRole);
        const QVariant end   = module_combo_->itemData(index, Qt::UserRole + 1);
        if (!start.isValid() || !end.isValid())
        {
            return;
        }

        if (index == 0)
        {
            module_combo_->setToolTip(tr("Whole process or a single loaded module"));
        }
        else
        {
            module_combo_->setToolTip(module_combo_->itemData(index, Qt::ToolTipRole).toString());
        }
        apply_range(start.toULongLong(), end.toULongLong());
    }

    std::expected<scan::ScanConfig, std::string> ScannerPanel::build_config() const
    {
        scan::ScanConfig config;
        config.type       = current_scan_type();
        config.value_type = current_value_type();
        config.hex        = hex_check_->isChecked();

        const std::string value_text = value_edit_->text().toStdString();

        const bool wants_value = scan::needs_value(config.type) || config.type == scan::ScanType::value_between;
        if (wants_value)
        {
            auto value = scan::parse_value(config.value_type, value_text, config.hex);
            if (!value)
            {
                return std::unexpected("Value: " + value.error().message);
            }
            config.value = std::move(*value);
        }
        if (config.type == scan::ScanType::value_between)
        {
            auto upper = scan::parse_value(config.value_type, value_upper_edit_->text().toStdString(), config.hex);
            if (!upper)
            {
                return std::unexpected("Upper value: " + upper.error().message);
            }
            config.value_upper = std::move(*upper);
        }

        const std::string start_text = start_edit_->text().toStdString();
        if (!start_text.empty())
        {
            auto start = scan::parse_address(start_text);
            if (!start)
            {
                return std::unexpected("Start address: " + start.error().message);
            }
            config.filter.start = *start;
        }
        const std::string stop_text = stop_edit_->text().toStdString();
        if (!stop_text.empty())
        {
            auto stop = scan::parse_address(stop_text);
            if (!stop)
            {
                return std::unexpected("Stop address: " + stop.error().message);
            }
            config.filter.stop = *stop;
        }

        config.filter.writable      = writable_check_->isChecked();
        config.filter.executable    = executable_check_->isChecked();
        config.filter.copy_on_write = copy_on_write_check_->isChecked();
        config.filter.alignment     = 1;
        if (fast_scan_check_->isChecked())
        {
            const std::string alignment_text = alignment_edit_->text().toStdString();
            if (alignment_text.empty())
            {
                config.filter.alignment = default_alignment(config.value_type);
            }
            else
            {
                auto alignment = scan::parse_alignment(alignment_text);
                if (!alignment)
                {
                    return std::unexpected("Alignment: " + alignment.error().message);
                }
                config.filter.alignment = *alignment;
            }
        }
        return config;
    }

    void ScannerPanel::start_first_scan()
    {
        auto config = build_config();
        if (!config)
        {
            log::warning(log::category::scan, config.error());
            return;
        }
        if (!worker_session_)
        {
            log::warning(log::category::scan, "No scan session; attach a process first.");
            return;
        }
        begin_scan(std::move(*config), false);
    }

    void ScannerPanel::start_next_scan()
    {
        auto config = build_config();
        if (!config)
        {
            log::warning(log::category::scan, config.error());
            return;
        }
        if (!engine_.has_results())
        {
            log::warning(log::category::scan, "Run a first scan before refining.");
            return;
        }
        begin_scan(std::move(*config), true);
    }

    void ScannerPanel::start_engine(scan::ScanConfig config, bool refine)
    {
        if (refine)
        {
            engine_.next_scan(std::move(config));
        }
        else
        {
            engine_.first_scan(std::move(config), scan::make_session_source(worker_session_));
        }
    }

    void ScannerPanel::begin_scan(scan::ScanConfig config, bool refine)
    {
        if (suspend_pending_.has_value() || pending_scan_.has_value())
        {
            return;
        }

        const bool pause = pause_scanning_check_->isChecked() && can_suspend_ && !debug_session_active_
                        && static_cast<bool>(worker_session_);
        if (!pause)
        {
            start_engine(std::move(config), refine);
            return;
        }

        // Suspend first; the scan starts from the completion, and a refused
        // suspend still starts the scan against a running target.
        pending_scan_ = PendingScan {std::move(config), refine};

        const process::JobId job_id = worker_.next_job_id();
        suspend_pending_            = job_id;
        const bool submitted =
            worker_.submit_suspend(job_id,
                                   worker_pid_,
                                   [this, job_id](process::JobResult&& result)
                                   {
                                       if (suspend_pending_ != job_id)
                                       {
                                           return;
                                       }

                                       const auto suspended = std::get<process::SuspendResult>(std::move(result));
                                       if (suspended.error)
                                       {
                                           log::warning(log::category::scan,
                                                        std::string("Pause failed, scanning a running target: ")
                                                            + std::string(process::describe(*suspended.error)));
                                       }
                                       else
                                       {
                                           target_suspended_ = true;
                                       }

                                       if (pending_scan_)
                                       {
                                           PendingScan pending = std::move(*pending_scan_);
                                           pending_scan_.reset();
                                           start_engine(std::move(pending.config), pending.refine);
                                       }
                                       suspend_pending_.reset();
                                       refresh();
                                   });
        if (!submitted)
        {
            suspend_pending_.reset();
            PendingScan pending = std::move(*pending_scan_);
            pending_scan_.reset();
            log::warning(log::category::scan, "Pause unavailable, scanning a running target.");
            start_engine(std::move(pending.config), pending.refine);
        }
    }

    void ScannerPanel::maybe_resume_target()
    {
        if (!target_suspended_ || resume_pending_.has_value() || suspend_pending_.has_value() || engine_.is_running())
        {
            return;
        }

        const process::JobId job_id = worker_.next_job_id();
        resume_pending_             = job_id;
        const bool submitted        = worker_.submit_resume(
            job_id,
            worker_pid_,
            [this, job_id](process::JobResult&& result)
            {
                if (resume_pending_ != job_id)
                {
                    return;
                }
                resume_pending_.reset();

                const auto resumed = std::get<process::SuspendResult>(std::move(result));
                if (!resumed.error)
                {
                    target_suspended_   = false;
                    resume_warn_logged_ = false;
                    return;
                }
                if (*resumed.error == process::AccessError::not_found
                    || *resumed.error == process::AccessError::unsupported)
                {
                    target_suspended_ = false;
                    return;
                }
                if (!resume_warn_logged_)
                {
                    log::warning(log::category::scan,
                                 std::string("Resume failed: ") + std::string(process::describe(*resumed.error)));
                    resume_warn_logged_ = true;
                }
            });
        if (!submitted)
        {
            resume_pending_.reset();
        }
    }

    void ScannerPanel::update_pause_check()
    {
        if (!target_.valid() || !static_cast<bool>(worker_session_))
        {
            pause_scanning_check_->setEnabled(false);
            pause_scanning_check_->setToolTip(tr("Attach to a target first."));
            return;
        }
        if (!can_suspend_)
        {
            pause_scanning_check_->setEnabled(false);
            pause_scanning_check_->setToolTip(tr("This target's plugin cannot suspend the game."));
            return;
        }
        if (debug_session_active_)
        {
            pause_scanning_check_->setEnabled(false);
            pause_scanning_check_->setToolTip(
                tr("Unavailable while a debug session runs; resuming the game would fight the debugger."));
            return;
        }
        pause_scanning_check_->setEnabled(true);
        pause_scanning_check_->setToolTip(
            tr("Freeze the game while a scan runs and resume it as soon as the scan finishes."));
    }

    void ScannerPanel::set_debug_session_active(bool active)
    {
        if (debug_session_active_ == active)
        {
            return;
        }
        debug_session_active_ = active;
        refresh();
    }

    void ScannerPanel::activate_scan_from_input()
    {
        if (engine_.is_running())
        {
            return;
        }
        if (engine_.has_results())
        {
            log::debug(log::category::ui, "Next Scan");
            start_next_scan();
        }
        else
        {
            log::debug(log::category::ui, "First Scan");
            start_first_scan();
        }
        refresh();
    }

    void ScannerPanel::set_default_alignment(std::uint64_t alignment)
    {
        alignment_edit_->setText(QString::number(alignment));
    }

    void ScannerPanel::refresh()
    {
        request_scan_session();
        request_memory_map();

        const scan::ScanSnapshot snapshot    = engine_.snapshot();
        const bool               running     = snapshot.state == scan::ScanState::running;
        const bool               has_results = engine_.has_results();
        const bool               preparing   = handoff_pending_.has_value() || suspend_pending_.has_value();
        const bool               attached    = target_.valid() && static_cast<bool>(worker_session_) && !preparing;

        scan_button_->setText(has_results ? tr("New Scan") : tr("First Scan"));
        scan_button_->setEnabled(attached && !running);
        next_scan_button_->setEnabled(attached && has_results && !running);
        undo_button_->setEnabled(attached && has_results && !running && engine_.can_undo());
        cancel_button_->setVisible(running);

        progress_percent_ = static_cast<int>(std::clamp(snapshot.progress, 0.0f, 1.0f) * 100.0f);

        maybe_resume_target();
        update_pause_check();
    }

}
