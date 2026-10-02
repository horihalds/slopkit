#include "ui/dialogs/memory_viewer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "scan/value.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        bool is_printable(std::byte value)
        {
            const auto character = std::to_integer<unsigned>(value);
            return character >= 0x20 && character < 0x7F;
        }

        QString hex_column(const std::vector<std::byte>& bytes, std::size_t offset, std::size_t count)
        {
            QString text;
            for (std::size_t index = 0; index < count; ++index)
            {
                if (offset + index < bytes.size())
                {
                    text += QString::number(std::to_integer<unsigned>(bytes[offset + index]), 16)
                                .rightJustified(2, QLatin1Char('0'))
                                .toUpper();
                    text += QLatin1Char(' ');
                }
                else
                {
                    text += QStringLiteral("   ");
                }
            }
            return text;
        }
    } // namespace

    MemoryDumpModel::MemoryDumpModel(QObject* parent) : QAbstractTableModel(parent)
    {
        unreadable_.fill(true);
    }

    int MemoryDumpModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(kRows);
    }

    int MemoryDumpModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : column_count;
    }

    QVariant MemoryDumpModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(kRows))
        {
            return {};
        }

        const auto row    = static_cast<std::size_t>(index.row());
        const auto offset = row * kRowBytes;

        switch (role)
        {
        case Qt::DisplayRole:
            switch (index.column())
            {
            case address:
                return QStringLiteral("0x")
                     + QString::number(base_ + offset, 16).rightJustified(16, QLatin1Char('0')).toUpper();
            case hex:
                return unreadable_[row] ? QString(kRowBytes * 3, QLatin1Char(' '))
                                        : hex_column(bytes_, offset, kRowBytes);
            case ascii:
                if (unreadable_[row])
                {
                    return QString(kRowBytes, QLatin1Char('?'));
                }
                {
                    QString text;
                    for (std::size_t index_in_row = 0; index_in_row < kRowBytes; ++index_in_row)
                    {
                        if (offset + index_in_row < bytes_.size())
                        {
                            text += is_printable(bytes_[offset + index_in_row])
                                      ? QLatin1Char(
                                            static_cast<char>(std::to_integer<unsigned>(bytes_[offset + index_in_row])))
                                      : QLatin1Char('.');
                        }
                        else
                        {
                            text += QLatin1Char('.');
                        }
                    }
                    return text;
                }
            default:
                break;
            }
            break;
        case Qt::FontRole:
            return QVariant::fromValue(mono_font());
        case Qt::TextAlignmentRole:
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        default:
            break;
        }
        return {};
    }

    QVariant MemoryDumpModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
        {
            switch (section)
            {
            case address:
                return tr("Address");
            case hex:
                return tr("Bytes");
            case ascii:
                return tr("ASCII");
            default:
                break;
            }
        }
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    void MemoryDumpModel::set_page(std::uint64_t base, std::vector<std::byte> bytes)
    {
        beginResetModel();
        base_  = base;
        bytes_ = std::move(bytes);
        for (std::size_t row = 0; row < kRows; ++row)
        {
            unreadable_[row] = row * kRowBytes >= bytes_.size();
        }
        endResetModel();
    }

    void MemoryDumpModel::clear()
    {
        beginResetModel();
        bytes_.clear();
        unreadable_.fill(true);
        endResetModel();
    }

    bool MemoryDumpModel::any_unreadable() const noexcept
    {
        return std::ranges::any_of(unreadable_,
                                   [](bool unreadable)
                                   {
                                       return unreadable;
                                   });
    }

    MemoryViewerDialog::MemoryViewerDialog(process::AccessWorker&   worker,
                                           process::AttachedTarget& target,
                                           QWidget*                 parent)
        : QDialog(parent), worker_(worker), target_(target)
    {
        setWindowTitle(tr("Memory Viewer"));
        resize(760, 560);

        build_layout();
        set_address(0);

        refresh_timer_ = new QTimer(this);
        refresh_timer_->setInterval(500);
        connect(refresh_timer_,
                &QTimer::timeout,
                this,
                [this]
                {
                    request_page();
                });
    }

    void MemoryViewerDialog::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto* controls = new QHBoxLayout();
        address_edit_  = new QLineEdit(this);
        address_edit_->setFont(mono_font());
        address_edit_->setPlaceholderText(tr("address"));
        address_edit_->setMaximumWidth(240);
        controls->addWidget(address_edit_);

        go_button_       = widgets::secondary_button(tr("Go"), this);
        previous_button_ = widgets::secondary_button(tr("<"), this);
        next_button_     = widgets::secondary_button(tr(">"), this);
        refresh_button_  = widgets::secondary_button(tr("Refresh"), this);
        controls->addWidget(go_button_);
        controls->addWidget(previous_button_);
        controls->addWidget(next_button_);
        controls->addWidget(refresh_button_);
        controls->addStretch(1);
        layout->addLayout(controls);

        auto* header_row = new QHBoxLayout();
        header_row->addWidget(widgets::section_header(tr("Hex dump"), this));
        loading_label_ = new QLabel(tr("Loading..."), this);
        header_row->addWidget(loading_label_);
        header_row->addStretch(1);
        layout->addLayout(header_row);

        dump_model_ = new MemoryDumpModel(this);
        dump_view_  = new QTableView(this);
        dump_view_->setModel(dump_model_);
        dump_view_->setSelectionMode(QAbstractItemView::NoSelection);
        dump_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        dump_view_->verticalHeader()->setVisible(false);
        dump_view_->horizontalHeader()->setSectionResizeMode(MemoryDumpModel::address, QHeaderView::ResizeToContents);
        dump_view_->horizontalHeader()->setSectionResizeMode(MemoryDumpModel::hex, QHeaderView::ResizeToContents);
        dump_view_->horizontalHeader()->setSectionResizeMode(MemoryDumpModel::ascii, QHeaderView::Stretch);
        layout->addWidget(dump_view_, 1);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        connect(go_button_, &QPushButton::clicked, this, &MemoryViewerDialog::go_to_address);
        connect(address_edit_, &QLineEdit::returnPressed, this, &MemoryViewerDialog::go_to_address);
        connect(previous_button_, &QPushButton::clicked, this, &MemoryViewerDialog::previous_page);
        connect(next_button_, &QPushButton::clicked, this, &MemoryViewerDialog::next_page);
        connect(refresh_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    refresh_requested_ = true;
                    request_page();
                });
    }

    void MemoryViewerDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        refresh_timer_->start();
        refresh_requested_ = true;
        request_page();
    }

    void MemoryViewerDialog::hideEvent(QHideEvent* event)
    {
        QDialog::hideEvent(event);
        refresh_timer_->stop();
    }

    void MemoryViewerDialog::set_address(std::uint64_t address)
    {
        base_ = address & ~static_cast<std::uint64_t>(MemoryDumpModel::kRowBytes - 1);
        address_edit_->setText(QStringLiteral("0x") + QString::number(base_, 16).toUpper());

        // The cached page belongs to the previous base.
        dump_model_->clear();
        ever_requested_    = false;
        refresh_requested_ = true;
        update_state();
        if (isVisible())
        {
            request_page();
        }
    }

    void MemoryViewerDialog::go_to_address()
    {
        if (const auto parsed = scan::parse_address(address_edit_->text().toStdString()); parsed.has_value())
        {
            set_address(*parsed);
        }
    }

    void MemoryViewerDialog::previous_page()
    {
        const std::uint64_t page = MemoryDumpModel::kRowBytes * MemoryDumpModel::kRows;
        set_address(base_ >= page ? base_ - page : 0);
    }

    void MemoryViewerDialog::next_page()
    {
        set_address(base_ + MemoryDumpModel::kRowBytes * MemoryDumpModel::kRows);
    }

    void MemoryViewerDialog::request_page()
    {
        if (pending_.has_value() || !target_.valid())
        {
            update_state();
            return;
        }
        if (ever_requested_ && !refresh_requested_ && target_.pid == requested_pid_ && base_ == requested_base_)
        {
            update_state();
            return;
        }

        const std::uint64_t      base = base_;
        const process::ProcessId pid  = target_.pid;

        const process::JobId job_id = worker_.next_job_id();
        pending_                    = job_id;
        requested_base_             = base;
        requested_pid_              = pid;
        refresh_requested_          = false;
        ever_requested_             = true;
        update_state();

        const bool submitted = worker_.submit_read(job_id,
                                                   base,
                                                   MemoryDumpModel::kRowBytes * MemoryDumpModel::kRows,
                                                   [this, base, pid, job_id](process::JobResult&& result)
                                                   {
                                                       if (pending_ != job_id)
                                                       {
                                                           return; // Superseded or shut down.
                                                       }
                                                       pending_.reset();

                                                       // Drop a page whose target or base changed while it was in
                                                       // flight.
                                                       if (!target_.valid() || target_.pid != pid || base_ != base)
                                                       {
                                                           update_state();
                                                           return;
                                                       }

                                                       auto& read = std::get<process::ReadResult>(result);
                                                       if (read.error || read.bytes.empty())
                                                       {
                                                           dump_model_->clear();
                                                       }
                                                       else
                                                       {
                                                           dump_model_->set_page(base, std::move(read.bytes));
                                                       }
                                                       update_state();
                                                   });
        if (!submitted)
        {
            pending_.reset();
            update_state();
        }
    }

    void MemoryViewerDialog::update_state()
    {
        loading_label_->setVisible(pending_.has_value());
        refresh_button_->setEnabled(target_.valid() && !pending_.has_value());
        previous_button_->setEnabled(base_ > 0);
        next_button_->setEnabled(target_.valid());
        go_button_->setEnabled(target_.valid());

        if (!target_.valid())
        {
            status_->set_status(widgets::StatusKind::info, tr("No process attached."));
            return;
        }
        if (dump_model_->any_unreadable() && !pending_.has_value())
        {
            status_->set_status(widgets::StatusKind::warning,
                                tr("Some rows could not be read; their bytes are marked with '?'."));
            return;
        }
        status_->clear_status();
    }

} // namespace slopkit::ui::dialogs
