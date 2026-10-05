#include "ui/dialogs/memory_viewer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/address_format.hpp"
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
                return address_text(base_ + offset);
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

    void MemoryDumpModel::set_modules(std::vector<process::ModuleInfo> modules)
    {
        ui::ModuleSpans spans;
        spans.set_modules(modules);
        if (spans == module_spans_)
        {
            return;
        }
        module_spans_ = std::move(spans);
        emit dataChanged(index(0, address), index(static_cast<int>(kRows) - 1, address), {Qt::DisplayRole});
    }

    void MemoryDumpModel::set_address_mode(ui::AddressMode mode)
    {
        if (address_mode_ == mode)
        {
            return;
        }
        address_mode_ = mode;
        emit dataChanged(index(0, address), index(static_cast<int>(kRows) - 1, address), {Qt::DisplayRole});
    }

    std::optional<QString> MemoryDumpModel::relative_address_text(std::uint64_t address) const
    {
        return ui::module_relative_text(address_mode_, module_spans_, address);
    }

    QString MemoryDumpModel::address_text(std::uint64_t address) const
    {
        if (const auto relative = relative_address_text(address); relative.has_value())
        {
            return *relative;
        }
        return QStringLiteral("0x") + QString::number(address, 16).rightJustified(16, QLatin1Char('0')).toUpper();
    }

    const ui::ModuleSpans& MemoryDumpModel::module_spans() const
    {
        return module_spans_;
    }

    MemoryViewerDialog::MemoryViewerDialog(process::AttachedTarget& target, QWidget* parent)
        : QDialog(parent), target_(target)
    {
        setWindowTitle(tr("Memory Viewer"));
        resize(760, 560);

        build_layout();
        set_address(0);
    }

    void MemoryViewerDialog::build_layout()
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto* controls = new QHBoxLayout();
        address_edit_  = new QLineEdit(this);
        address_edit_->setFont(mono_font());
        address_edit_->setPlaceholderText(tr("address or module+RVA"));
        address_edit_->setToolTip(tr("Absolute address (0x1040) or module-relative (libc.so.6+1A2B)"));
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
                    request_page();
                });
    }

    void MemoryViewerDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        // The coordinator only submits the page while the dialog is visible, so
        // showing it is enough to bring the page up to date.
        page_loaded_ = false;
        request_page();
    }

    void MemoryViewerDialog::set_modules(std::vector<process::ModuleInfo> modules)
    {
        dump_model_->set_modules(std::move(modules));
        address_edit_->setText(display_text(base_));
    }

    void MemoryViewerDialog::set_address_mode(ui::AddressMode mode)
    {
        dump_model_->set_address_mode(mode);
        address_edit_->setText(display_text(base_));
    }

    QString MemoryViewerDialog::display_text(std::uint64_t address) const
    {
        if (const auto relative = dump_model_->relative_address_text(address); relative.has_value())
        {
            return *relative;
        }
        return QStringLiteral("0x") + QString::number(address, 16).toUpper();
    }

    void MemoryViewerDialog::set_address(std::uint64_t address)
    {
        base_ = address & ~static_cast<std::uint64_t>(MemoryDumpModel::kRowBytes - 1);
        address_edit_->setText(display_text(base_));

        // The cached page belongs to the previous base.
        dump_model_->clear();
        page_loaded_ = false;
        update_state();
        if (isVisible())
        {
            request_page();
        }
    }

    void MemoryViewerDialog::go_to_address()
    {
        const auto parsed = ui::parse_address_text(address_edit_->text().toStdString(), dump_model_->module_spans());
        if (parsed.has_value())
        {
            log::debug(log::category::ui, std::format("memory viewer go to 0x{:X}", *parsed));
            set_address(*parsed);
        }
        else
        {
            log::warning(log::category::ui, "memory viewer got an unparseable address");
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
        manual_request_ = true;
        update_state();
        emit liveRefreshRequested();
    }

    std::vector<ui::LiveRequest> MemoryViewerDialog::next_live_request()
    {
        if (!isVisible() || !target_.valid())
        {
            return {};
        }
        requested_base_ = base_;
        requested_pid_  = target_.pid;
        return {
            ui::LiveRequest {.id = 0, .address = base_, .size = MemoryDumpModel::kRowBytes * MemoryDumpModel::kRows}
        };
    }

    void MemoryViewerDialog::apply_live_readings(std::span<const ui::LiveReading> readings)
    {
        if (readings.empty())
        {
            return;
        }
        if (!target_.valid() || target_.pid != requested_pid_ || base_ != requested_base_)
        {
            return; // The target or the page moved on while the pass was in flight.
        }
        const ui::LiveReading& reading = readings.front();
        apply_page(requested_base_, reading.bytes, reading.readable);
    }

    void MemoryViewerDialog::apply_page(std::uint64_t base, const std::vector<std::byte>& bytes, bool readable)
    {
        if (!readable || bytes.empty())
        {
            log::debug(log::category::ui, std::format("memory page at 0x{:X} could not be read", base));
            dump_model_->clear();
        }
        else
        {
            log::debug(log::category::ui, std::format("memory page at 0x{:X} loaded ({} byte(s))", base, bytes.size()));
            dump_model_->set_page(base, bytes);
        }
        page_loaded_    = true;
        manual_request_ = false;
        update_state();
    }

    void MemoryViewerDialog::update_state()
    {
        const bool loading = target_.valid() && (manual_request_ || !page_loaded_);
        loading_label_->setVisible(loading);
        refresh_button_->setEnabled(target_.valid());
        previous_button_->setEnabled(base_ > 0);
        next_button_->setEnabled(target_.valid());
        go_button_->setEnabled(target_.valid());

        if (!target_.valid())
        {
            status_->set_status(widgets::StatusKind::info, tr("No process attached."));
            return;
        }
        if (dump_model_->any_unreadable() && !loading)
        {
            status_->set_status(widgets::StatusKind::warning,
                                tr("Some rows could not be read; their bytes are marked with '?'."));
            return;
        }
        status_->clear_status();
    }

} // namespace slopkit::ui::dialogs
