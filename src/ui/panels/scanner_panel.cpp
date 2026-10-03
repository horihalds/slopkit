#include "ui/panels/scanner_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::panels
{

    namespace
    {
        QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        }

        // A disabled control that explains why it is unavailable.
        QCheckBox* unavailable_checkbox(const QString& label, const QString& reason)
        {
            auto* box = new QCheckBox(label);
            box->setEnabled(false);
            box->setToolTip(reason);
            return box;
        }

        // Shortens a long module name, keeping its head and tail readable.
        QString elide_name(const QString& name, int max_chars)
        {
            if (name.size() <= max_chars)
            {
                return name;
            }
            const int keep = (max_chars - 3) / 2;
            return name.left(keep) + QStringLiteral("...") + name.right(max_chars - 3 - keep);
        }

        // The alignment the fast scan uses when the field is left blank.
        std::size_t default_alignment(scan::ValueType type) noexcept
        {
            const std::size_t size = scan::value_size(type);
            return size == 0 ? 4 : size;
        }
    } // namespace

    ScannerPanel::ScannerPanel(process::AccessWorker& worker, process::AttachedTarget& target, QWidget* parent)
        : QWidget(parent), worker_(worker), target_(target)
    {
        setMinimumWidth(320);
        build_layout();
        connect_widgets();
        refresh();
    }

    scan::ScanEngine& ScannerPanel::engine() noexcept
    {
        return engine_;
    }

    int ScannerPanel::progress_percent() const noexcept
    {
        return progress_percent_;
    }

    std::uint64_t ScannerPanel::main_module_address() const noexcept
    {
        const process::ModuleInfo* main_module = nullptr;
        for (const auto& module : modules_)
        {
            if (module.kind == process::ModuleKind::anonymous || module.path.empty())
            {
                continue;
            }
            if (main_module == nullptr || module.base < main_module->base)
            {
                main_module = &module;
            }
        }
        if (main_module == nullptr)
        {
            return 0;
        }
        return main_module->entry != 0 ? main_module->entry : main_module->base;
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

        // Hex toggle, then the value (and, for "Value between", the upper bound).
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
        value_type_combo_->setCurrentIndex(2); // 4 Bytes
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
        alignment_edit_ = new QLineEdit(QStringLiteral("4"));
        alignment_edit_->setPlaceholderText(tr("Alignment"));
        alignment_edit_->setFont(mono_font());
        alignment_edit_->setMaximumWidth(96);
        alignment_edit_->setToolTip(tr("Alignment step in bytes (decimal or 0x hex); blank uses the value size"));
        fast_row->addWidget(fast_scan_check_);
        fast_row->addWidget(alignment_edit_);
        fast_row->addStretch(1);
        options_body->addLayout(fast_row);

        pause_scanning_check_ = new QCheckBox(tr("Pause the game while scanning"));
        pause_scanning_check_->setToolTip(tr("Accepted as a setting; no effect until a plugin can suspend the target"));
        options_body->addWidget(pause_scanning_check_);

        options_body->addWidget(widgets::section_header(tr("Extra options"), options));
        auto* extra_row = new QHBoxLayout();
        extra_row->addWidget(
            unavailable_checkbox(tr("Not"), tr("Disabled: the scan engine does not invert comparisons yet")));
        extra_row->addWidget(unavailable_checkbox(tr("Unrandomizer"), tr("Disabled: requires code injection")));
        extra_row->addStretch(1);
        options_body->addLayout(extra_row);

        status_label_ = new widgets::StatusLabel(this);
        layout->addWidget(status_label_);
        layout->addStretch(1);

        update_value_inputs();
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
                    start_first_scan();
                    refresh();
                });
        connect(next_scan_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    start_next_scan();
                    refresh();
                });
        connect(undo_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    engine_.undo();
                    refresh();
                });
        connect(cancel_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    engine_.cancel();
                    refresh();
                });

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
            return; // Retry once the engine has stopped.
        }

        worker_session_ = process::Session {};
        worker_pid_     = 0;
        worker_plugin_.clear();
        if (!target_.valid())
        {
            clear_memory_map();
            status_.clear();
            status_is_error_ = false;
            return;
        }

        const process::ProcessId pid    = target_.pid;
        const std::string        plugin = target_.plugin_id;
        worker_pid_                     = pid;
        worker_plugin_                  = plugin;

        const process::JobId job_id = worker_.next_job_id();
        handoff_pending_            = job_id;

        const bool submitted = worker_.submit_attach_handoff(
            job_id,
            pid,
            plugin,
            [this, job_id, pid, plugin](process::JobResult&& result)
            {
                if (handoff_pending_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                handoff_pending_.reset();

                auto& attached = std::get<process::AttachResult>(result);
                // Drop a session whose target changed while the handoff ran;
                // worker_pid_ still reflects the request, so the next tick
                // requests the new target.
                if (!target_.valid() || target_.pid != pid || target_.plugin_id != plugin)
                {
                    return;
                }
                if (attached.error)
                {
                    status_ = std::string("Scan session failed: ") + std::string(process::describe(*attached.error));
                    status_is_error_ = true;
                    return;
                }

                worker_session_  = std::move(*attached.handed_session);
                status_          = std::string();
                status_is_error_ = false;
            });
        if (!submitted)
        {
            handoff_pending_.reset();
        }
    }

    void ScannerPanel::request_memory_map()
    {
        const bool changed = target_.pid != map_pid_ || target_.plugin_id != map_plugin_;
        if (!changed || map_pending_.has_value() || !target_.valid() || !worker_.attached())
        {
            return;
        }

        const process::ProcessId pid    = target_.pid;
        const std::string        plugin = target_.plugin_id;
        map_pid_                        = pid;
        map_plugin_                     = plugin;

        const process::JobId job_id = worker_.next_job_id();
        map_pending_                = job_id;

        // Show the in-flight state; the last known ranges stay in the fields.
        range_updating_ = true;
        module_combo_->clear();
        module_combo_->addItem(tr("Loading…"));
        module_combo_->setEnabled(false);
        range_updating_ = false;

        const bool submitted =
            worker_.submit_memory_map(job_id,
                                      [this, job_id, pid, plugin](process::JobResult&& result)
                                      {
                                          if (map_pending_ != job_id)
                                          {
                                              return; // Superseded or shut down.
                                          }
                                          map_pending_.reset();
                                          // Drop a map whose target changed while the job ran; the next
                                          // tick requests the new target.
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
        modules_ = std::move(result.modules);

        // The beginning of the process memory space: its lowest mapped page,
        // readable or not. The end is the user-space ceiling; the engine walks
        // the mapped regions and stops after the last one, so the ceiling costs
        // no reads.
        std::optional<std::uint64_t> lowest;
        for (const auto& region : result.regions)
        {
            if (region.end <= region.start)
            {
                continue;
            }
            lowest = lowest ? std::min(*lowest, region.start) : std::optional {region.start};
        }

        process_bounds_ =
            lowest ? std::optional {std::pair {*lowest, scan::kMaxUserAddress}} : std::nullopt;

        if (process_bounds_)
        {
            apply_range(process_bounds_->first, process_bounds_->second);
        }
        else
        {
            start_edit_->clear();
            stop_edit_->clear();
        }

        rebuild_range_items();
    }

    void ScannerPanel::apply_range(std::uint64_t start, std::uint64_t end)
    {
        start_edit_->setText(QStringLiteral("0x") + QString::number(start, 16).toUpper());
        stop_edit_->setText(QStringLiteral("0x") + QString::number(end, 16).toUpper());
    }

    void ScannerPanel::clear_memory_map()
    {
        map_pending_.reset();
        map_pid_ = 0;
        map_plugin_.clear();
        modules_.clear();
        process_bounds_.reset();
        start_edit_->clear();
        stop_edit_->clear();

        range_updating_ = true;
        module_combo_->clear();
        module_combo_->addItem(tr("All memory"));
        module_combo_->setCurrentIndex(0);
        module_combo_->setEnabled(false);
        range_updating_ = false;
    }

    void ScannerPanel::rebuild_range_items()
    {
        const auto range_text = [](std::uint64_t start, std::uint64_t end)
        {
            return QStringLiteral("0x") + QString::number(start, 16).toUpper() + QStringLiteral("-")
                 + QStringLiteral("0x") + QString::number(end, 16).toUpper();
        };

        range_updating_ = true;
        module_combo_->clear();

        if (process_bounds_)
        {
            module_combo_->addItem(
                tr("All memory  %1").arg(range_text(process_bounds_->first, process_bounds_->second)));
            module_combo_->setItemData(0, QVariant::fromValue<qulonglong>(process_bounds_->first), Qt::UserRole);
            module_combo_->setItemData(0, QVariant::fromValue<qulonglong>(process_bounds_->second), Qt::UserRole + 1);
        }
        else
        {
            module_combo_->addItem(tr("All memory"));
        }

        // Only file-backed modules are listed, ordered by their base address;
        // anonymous mappings stay covered by the whole-process entry.
        std::vector<const process::ModuleInfo*> listed;
        for (const auto& module : modules_)
        {
            if (module.kind == process::ModuleKind::anonymous || module.size == 0)
            {
                continue;
            }
            listed.push_back(&module);
        }
        std::ranges::sort(listed, {}, &process::ModuleInfo::base);

        for (const process::ModuleInfo* module : listed)
        {
            const std::uint64_t start = module->base;
            const std::uint64_t end   = module->base + module->size;
            module_combo_->addItem(
                QStringLiteral("%1  %2").arg(elide_name(to_qstring(module->name), 48), range_text(start, end)));
            const int row = module_combo_->count() - 1;
            module_combo_->setItemData(row, QVariant::fromValue<qulonglong>(start), Qt::UserRole);
            module_combo_->setItemData(row, QVariant::fromValue<qulonglong>(end), Qt::UserRole + 1);
            module_combo_->setItemData(row, to_qstring(module->path), Qt::ToolTipRole);
        }

        module_combo_->setCurrentIndex(0);
        module_combo_->setEnabled(true);
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
        const auto config = build_config();
        if (!config)
        {
            status_          = config.error();
            status_is_error_ = true;
            return;
        }
        if (!worker_session_)
        {
            status_          = "No scan session; attach a process first.";
            status_is_error_ = true;
            return;
        }
        status_.clear();
        status_is_error_ = false;
        engine_.first_scan(*config, scan::make_session_source(worker_session_));
    }

    void ScannerPanel::start_next_scan()
    {
        const auto config = build_config();
        if (!config)
        {
            status_          = config.error();
            status_is_error_ = true;
            return;
        }
        if (!engine_.has_results())
        {
            status_          = "Run a first scan before refining.";
            status_is_error_ = true;
            return;
        }
        status_.clear();
        status_is_error_ = false;
        engine_.next_scan(*config);
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
        const bool               preparing   = handoff_pending_.has_value();
        const bool               attached    = target_.valid() && static_cast<bool>(worker_session_) && !preparing;

        scan_button_->setText(has_results ? tr("New Scan") : tr("First Scan"));
        scan_button_->setEnabled(attached && !running);
        next_scan_button_->setEnabled(attached && !running);
        undo_button_->setEnabled(attached && !running);
        cancel_button_->setVisible(running);

        progress_percent_ = static_cast<int>(std::clamp(snapshot.progress, 0.0f, 1.0f) * 100.0f);

        if (!status_.empty())
        {
            status_label_->set_status(status_is_error_ ? widgets::StatusKind::error : widgets::StatusKind::info,
                                      to_qstring(status_));
        }
        else if (preparing)
        {
            status_label_->set_status(widgets::StatusKind::info, tr("Preparing scan session..."));
        }
        else if (!snapshot.message.empty())
        {
            status_label_->set_status(snapshot.state == scan::ScanState::failed ? widgets::StatusKind::error
                                                                                : widgets::StatusKind::info,
                                      to_qstring(snapshot.message));
        }
        else if (!attached)
        {
            status_label_->set_status(widgets::StatusKind::info, tr("Select a process to enable scanning."));
        }
        else
        {
            status_label_->clear_status();
        }
    }

} // namespace slopkit::ui::panels
