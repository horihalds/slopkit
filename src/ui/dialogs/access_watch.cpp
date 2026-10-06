#include "ui/dialogs/access_watch.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <QAbstractTableModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "disasm/decoder.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::dialogs
{
    namespace
    {
        // One row of the recorded-accesses table.
        struct HitRow
        {
            std::uint64_t instruction {}; // the RIP the stop reported (one past the access)
            std::uint64_t recovered {};   // the instruction's own address, once decoded
            QString       text;
            std::uint32_t tid {};
            std::uint64_t count {};
        };

        // One row of the instruction-accesses table.
        struct AccessRow
        {
            std::uint64_t address {};
            QString       operand;
            std::size_t   width {};
            bool          writes {};
            bool          resolved {};
        };

        // The bytes to read ending at a hit's RIP: enough for the longest x86-64
        // instruction, without over-reading.
        constexpr std::size_t kReadWindow     = 32;
        // New rows decoded per update, so a hit storm never queues an unbounded
        // read.
        constexpr std::size_t kMaxRowsPerRead = 32;

        // The hit's RIP is one past the accessing instruction, so the bytes at
        // RIP are read backwards and the longest decode that ends exactly at RIP
        // wins; anything else leaves the text unknown.
        [[nodiscard]] std::optional<disasm::Instruction>
        recover_instruction(const std::vector<std::byte>& bytes, std::uint64_t base, std::uint64_t rip)
        {
            std::optional<disasm::Instruction> best;
            for (std::size_t offset = 0; offset < bytes.size(); ++offset)
            {
                const std::span<const std::byte> window(bytes.data() + offset, bytes.size() - offset);
                const auto decoded = disasm::decode(window, base + offset, disasm::MachineMode::long_64);
                if (!decoded.has_value() || !decoded->valid)
                {
                    continue;
                }
                if (decoded->address + decoded->length == rip)
                {
                    if (!best.has_value() || decoded->length > best->length)
                    {
                        best = decoded;
                    }
                }
            }
            return best;
        }
    } // namespace

    AccessWatchDialog::AccessWatchDialog(debug::Controller&       controller,
                                         process::AccessWorker&   worker,
                                         process::AttachedTarget& target,
                                         QWidget*                 parent)
        : QDialog(parent), controller_(controller), worker_(worker), gate_(controller, target, *this, this)
    {
        setWindowTitle(tr("Access Watch"));
        setWindowFlag(Qt::Window, true);
        resize(760, 480);
        build_layout();

        connect(&controller_, &debug::Controller::watchChanged, this, &AccessWatchDialog::refresh);
        // The gate's own lines (attaching, attached, cancelled, failed) land in
        // the window's status area, and a give-up drops the pending watch.
        connect(&gate_,
                &DebugSessionGate::progress,
                this,
                [this](const QString& text, bool error)
                {
                    status_->set_status(error ? widgets::StatusKind::warning : widgets::StatusKind::info, text);
                });
        connect(&gate_,
                &DebugSessionGate::abandoned,
                this,
                [this]
                {
                    pending_watch_.reset();
                });
        refresh();
    }

    void AccessWatchDialog::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        // Both tables render their addresses through the same formatter the
        // window's header and hint strings use.
        const ui::AddressText address_text = [this](std::uint64_t address)
        {
            return display_address(address);
        };
        // The untruncated address the table hovers reveal when a module name
        // was shortened to fit its column.
        const ui::AddressText full_address_text = [this](std::uint64_t address)
        {
            return ui::format_cell_address_full(controller_.address_mode(), controller_.modules(), address);
        };

        header_ = new QLabel(this);
        header_->setObjectName(QStringLiteral("access_watch_header"));
        header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        // The header quotes the watch address, so it lines up with the tables.
        header_->setFont(mono_font());
        layout->addWidget(header_);

        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("access_watch_status"));
        layout->addWidget(status_);

        // The recorded accesses: one row per instruction, with its hit count.
        layout->addWidget(widgets::section_header(tr("Recorded accesses"), this));
        hits_model_     = new models::WatchHitsModel(this, address_text, full_address_text);
        recorded_table_ = new QTableView(this);
        recorded_table_->setObjectName(QStringLiteral("access_watch_hits"));
        recorded_table_->setModel(hits_model_);
        recorded_table_->setItemDelegate(new widgets::ElidedTooltipDelegate(recorded_table_));
        recorded_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        recorded_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        recorded_table_->setSelectionMode(QAbstractItemView::SingleSelection);
        recorded_table_->setShowGrid(false);
        recorded_table_->verticalHeader()->setVisible(false);
        recorded_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        recorded_table_->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(recorded_table_, 1);

        auto* recorded_buttons = new QHBoxLayout();
        follow_                = widgets::secondary_button(tr("Follow in Memory Viewer"), this);
        stop_                  = widgets::secondary_button(tr("Stop"), this);
        clear_                 = widgets::secondary_button(tr("Clear"), this);
        close_                 = widgets::secondary_button(tr("Close"), this);
        recorded_buttons->addWidget(follow_);
        recorded_buttons->addWidget(stop_);
        recorded_buttons->addWidget(clear_);
        recorded_buttons->addStretch(1);
        recorded_buttons->addWidget(close_);
        layout->addLayout(recorded_buttons);

        // The resolved operands of one listing row, each of which can become the
        // next watch.
        layout->addWidget(widgets::section_header(tr("Instruction accesses"), this));
        instruction_model_ = new models::InstructionAccessModel(this, address_text, full_address_text);
        instruction_table_ = new QTableView(this);
        instruction_table_->setItemDelegate(new widgets::ElidedTooltipDelegate(instruction_table_));
        instruction_table_->setObjectName(QStringLiteral("access_watch_instructions"));
        instruction_table_->setModel(instruction_model_);
        instruction_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        instruction_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        instruction_table_->setSelectionMode(QAbstractItemView::SingleSelection);
        instruction_table_->setShowGrid(false);
        instruction_table_->verticalHeader()->setVisible(false);
        instruction_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        instruction_table_->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(instruction_table_, 1);

        auto* instruction_buttons = new QHBoxLayout();
        watch_writes_             = widgets::secondary_button(tr("Watch writes"), this);
        watch_accesses_           = widgets::secondary_button(tr("Watch accesses"), this);
        instruction_buttons->addWidget(watch_writes_);
        instruction_buttons->addWidget(watch_accesses_);
        instruction_buttons->addStretch(1);
        layout->addLayout(instruction_buttons);

        hint_ = new widgets::StatusLabel(this);
        hint_->setObjectName(QStringLiteral("access_watch_hint"));
        // The hint quotes the resolved address, so it is mono like the header.
        hint_->setFont(mono_font());
        layout->addWidget(hint_);

        connect(follow_, &QPushButton::clicked, this, &AccessWatchDialog::follow_selected);
        connect(stop_, &QPushButton::clicked, &controller_, &debug::Controller::stop_watch);
        connect(clear_, &QPushButton::clicked, &controller_, &debug::Controller::clear_watch_hits);
        connect(close_, &QPushButton::clicked, this, &QDialog::close);
        connect(watch_writes_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    watch_selected(true);
                });
        connect(watch_accesses_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    watch_selected(false);
                });

        connect(recorded_table_->selectionModel(),
                &QItemSelectionModel::currentRowChanged,
                this,
                [this]
                {
                    follow_->setEnabled(recorded_table_->currentIndex().isValid());
                });
        connect(instruction_table_->selectionModel(),
                &QItemSelectionModel::currentRowChanged,
                this,
                [this]
                {
                    update_instruction_buttons();
                });

        widgets::chain_tab_order(
            {recorded_table_, follow_, stop_, clear_, close_, instruction_table_, watch_writes_, watch_accesses_});

        follow_->setEnabled(false);
        hint_->set_status(widgets::StatusKind::info, tr("Select an instruction in the Memory Viewer."));
        update_instruction_buttons();
    }

    QString AccessWatchDialog::display_address(std::uint64_t address) const
    {
        return ui::format_cell_address(controller_.address_mode(), controller_.modules(), address);
    }

    QString AccessWatchDialog::header_text() const
    {
        const debug::AccessWatch& watch = controller_.watch();
        if (watch.state() == debug::WatchState::idle)
        {
            return tr("No watch running.");
        }

        const QString verb  = watch.kind() == debug::Kind::hardware_read_write ? tr("accesses") : tr("writes");
        const QString total = QLocale().toString(static_cast<qlonglong>(watch.hit_count()));
        const QString line  = watch.state() == debug::WatchState::watching
                                ? tr("Watching %1 · %2 · %3 bytes · %4 accesses")
                                : tr("Stopped watching %1 · %2 · %3 bytes · %4 accesses");
        QString       text  = line.arg(display_address(watch.address())).arg(verb).arg(watch.size()).arg(total);
        if (watch.truncated())
        {
            text += tr(" · truncated");
        }
        return text;
    }

    QString AccessWatchDialog::hint_text() const
    {
        return hint_->text();
    }

    QString AccessWatchDialog::status_text() const
    {
        return status_->text();
    }

    void AccessWatchDialog::start_watch(std::uint64_t address, debug::Kind kind, std::size_t size)
    {
        const auto armed = controller_.watch_address(address, kind, size);
        if (!armed.has_value())
        {
            status_->set_status(widgets::StatusKind::warning, QString::fromStdString(armed.error()));
            log::warning(log::category::debug, std::format("access watch refused: {}", armed.error()));
            return;
        }
        status_->clear_status();
        refresh();
    }

    void AccessWatchDialog::arm_watch(std::uint64_t address, debug::Kind kind, std::size_t size)
    {
        pending_watch_ = PendingWatch {address, kind, size};
        if (!gate_.session_live())
        {
            status_->set_status(widgets::StatusKind::info, tr("Waiting for a debug session…"));
        }
        const QString reason = kind == debug::Kind::hardware_read_write ? tr("find out what accesses this address")
                                                                        : tr("find out what writes this address");
        gate_.with_session(reason,
                           [this]
                           {
                               arm_pending_watch();
                           });
    }

    void AccessWatchDialog::arm_pending_watch()
    {
        if (!pending_watch_.has_value())
        {
            return;
        }
        const PendingWatch watch = *pending_watch_;
        pending_watch_.reset();
        show();
        raise();
        activateWindow();
        start_watch(watch.address, watch.kind, watch.size);
    }

    DebugSessionGate& AccessWatchDialog::debug_gate() noexcept
    {
        return gate_;
    }

    bool AccessWatchDialog::watch_pending() const noexcept
    {
        return pending_watch_.has_value();
    }

    void AccessWatchDialog::show_instruction_accesses(std::uint64_t                   instruction,
                                                      std::size_t                     instruction_length,
                                                      std::vector<ui::ResolvedAccess> accesses)
    {
        instruction_        = instruction;
        instruction_length_ = instruction_length;
        instruction_model_->set(std::move(accesses));
        const bool resolved = instruction_model_->any_resolved();

        if (instruction_ == 0)
        {
            hint_->set_status(widgets::StatusKind::info, tr("Select an instruction in the Memory Viewer."));
        }
        else if (instruction_model_->row_count() == 0)
        {
            hint_->set_status(widgets::StatusKind::info, tr("This instruction accesses no memory."));
        }
        else if (!resolved)
        {
            hint_->set_status(widgets::StatusKind::warning,
                              tr("The operands could not be resolved from the register values at this stop."));
        }
        else
        {
            hint_->set_status(widgets::StatusKind::info,
                              tr("Resolved from the registers at %1").arg(display_address(instruction_)));
        }
        update_instruction_buttons();
    }

    void AccessWatchDialog::refresh()
    {
        hits_model_->sync(controller_.watch());
        request_texts();

        header_->setText(header_text());
        stop_->setEnabled(controller_.watch().state() == debug::WatchState::watching);
        clear_->setEnabled(controller_.watch().state() != debug::WatchState::idle);
        recorded_table_->setEnabled(controller_.watch().state() != debug::WatchState::idle);
    }

    void AccessWatchDialog::request_texts()
    {
        std::vector<process::ReadManyItem> items;
        std::vector<int>                   rows;
        for (int row = 0; row < hits_model_->rowCount() && items.size() < kMaxRowsPerRead; ++row)
        {
            if (!hits_model_->needs_text(row))
            {
                continue;
            }
            const std::uint64_t rip  = hits_model_->instruction_at(row);
            const std::uint64_t base = rip > kReadWindow - 1 ? rip - (kReadWindow - 1) : 0;
            items.push_back(process::ReadManyItem {base, static_cast<std::size_t>(rip - base) + 1});
            rows.push_back(row);
        }
        if (items.empty())
        {
            return;
        }

        const process::JobId id = worker_.next_job_id();
        worker_.submit_read_many(id,
                                 std::move(items),
                                 [this, rows = std::move(rows)](process::JobResult&& result)
                                 {
                                     apply_texts(std::move(result), rows);
                                 });
    }

    void AccessWatchDialog::apply_texts(process::JobResult&& result, std::vector<int> rows)
    {
        const auto* batch = std::get_if<process::ReadManyResult>(&result);
        if (batch == nullptr)
        {
            return;
        }

        const std::size_t count = std::min(batch->items.size(), rows.size());
        for (std::size_t index = 0; index < count; ++index)
        {
            const int row = rows[index];
            if (row < 0 || row >= hits_model_->rowCount())
            {
                continue;
            }
            const auto& item = batch->items[index];
            if (!item.has_value())
            {
                hits_model_->set_text(row, 0, QStringLiteral("??"));
                continue;
            }

            const std::uint64_t rip  = hits_model_->instruction_at(row);
            const std::uint64_t base = rip > kReadWindow - 1 ? rip - (kReadWindow - 1) : 0;
            if (const auto recovered = recover_instruction(*item, base, rip))
            {
                hits_model_->set_text(row, recovered->address, to_qstring(recovered->text));
            }
            else
            {
                hits_model_->set_text(row, 0, QStringLiteral("??"));
            }
        }
    }

    void AccessWatchDialog::follow_selected()
    {
        const int row = recorded_table_->currentIndex().row();
        if (row < 0 || row >= hits_model_->rowCount())
        {
            return;
        }
        emit followRequested(hits_model_->display_instruction_at(row));
    }

    void AccessWatchDialog::watch_selected(bool writes)
    {
        const int row = instruction_table_->currentIndex().row();
        if (row < 0 || row >= instruction_model_->row_count())
        {
            status_->set_status(widgets::StatusKind::warning, tr("Select an instruction access first."));
            return;
        }
        if (!instruction_model_->resolved_at(row))
        {
            status_->set_status(widgets::StatusKind::warning,
                                tr("The address could not be resolved from the registers."));
            return;
        }
        start_watch(instruction_model_->address_at(row),
                    writes ? debug::Kind::hardware_write : debug::Kind::hardware_read_write,
                    ui::watch_size(instruction_model_->width_at(row)));
    }

    void AccessWatchDialog::update_instruction_buttons()
    {
        const int  row      = instruction_table_->currentIndex().row();
        const bool resolved = instruction_model_->resolved_at(row);
        watch_writes_->setEnabled(resolved);
        watch_accesses_->setEnabled(resolved);
    }

    QTableView* AccessWatchDialog::recorded_table() const noexcept
    {
        return recorded_table_;
    }

    QTableView* AccessWatchDialog::instruction_table() const noexcept
    {
        return instruction_table_;
    }

    QPushButton* AccessWatchDialog::follow_button() const noexcept
    {
        return follow_;
    }

    QPushButton* AccessWatchDialog::stop_button() const noexcept
    {
        return stop_;
    }

    QPushButton* AccessWatchDialog::clear_button() const noexcept
    {
        return clear_;
    }

    QPushButton* AccessWatchDialog::close_button() const noexcept
    {
        return close_;
    }

    QPushButton* AccessWatchDialog::watch_writes_button() const noexcept
    {
        return watch_writes_;
    }

    QPushButton* AccessWatchDialog::watch_accesses_button() const noexcept
    {
        return watch_accesses_;
    }

    int AccessWatchDialog::recorded_row_count() const
    {
        return hits_model_->rowCount();
    }

    std::uint64_t AccessWatchDialog::recorded_instruction_at(int row) const
    {
        return hits_model_->instruction_at(row);
    }

    std::uint64_t AccessWatchDialog::recorded_count_at(int row) const
    {
        return hits_model_->count_at(row);
    }

    QString AccessWatchDialog::recorded_text_at(int row) const
    {
        return hits_model_->text_at(row);
    }

    int AccessWatchDialog::instruction_row_count() const
    {
        return instruction_model_->row_count();
    }

    void AccessWatchDialog::select_recorded_row(int row)
    {
        recorded_table_->selectRow(row);
    }

    void AccessWatchDialog::select_instruction_row(int row)
    {
        instruction_table_->selectRow(row);
    }

} // namespace slopkit::ui::dialogs
