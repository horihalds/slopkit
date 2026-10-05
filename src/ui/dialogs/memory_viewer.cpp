#include "ui/dialogs/memory_viewer.hpp"

#include <cstdint>
#include <format>
#include <utility>
#include <vector>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/memory_view.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    MemoryViewerDialog::MemoryViewerDialog(process::AccessWorker&   worker,
                                           process::AttachedTarget& target,
                                           QWidget*                 parent)
        : QDialog(parent), target_(target), document_(worker, target, this)
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
        address_edit_->setObjectName(QStringLiteral("address_edit"));
        address_edit_->setFont(mono_font());
        address_edit_->setPlaceholderText(tr("address or module+RVA"));
        address_edit_->setToolTip(tr("Absolute address (0x1040) or module-relative (libc.so.6+1A2B)"));
        address_edit_->setMaximumWidth(240);
        controls->addWidget(address_edit_);

        go_button_      = widgets::secondary_button(tr("Go"), this);
        refresh_button_ = widgets::secondary_button(tr("Refresh"), this);
        controls->addWidget(go_button_);
        controls->addWidget(refresh_button_);
        controls->addStretch(1);
        layout->addLayout(controls);

        auto* header_row = new QHBoxLayout();
        header_row->addWidget(widgets::section_header(tr("Hex dump"), this));
        loading_label_ = new QLabel(tr("Loading..."), this);
        loading_label_->setObjectName(QStringLiteral("loading_label"));
        header_row->addWidget(loading_label_);
        header_row->addStretch(1);
        layout->addLayout(header_row);

        view_ = new components::MemoryView(document_, this);
        layout->addWidget(view_, 1);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        connect(go_button_, &QPushButton::clicked, this, &MemoryViewerDialog::go_to_address);
        connect(address_edit_, &QLineEdit::returnPressed, this, &MemoryViewerDialog::go_to_address);
        connect(refresh_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    request_page();
                });

        // The address box follows free scrolling unless the user is typing.
        connect(view_,
                &components::MemoryView::firstByteChanged,
                this,
                [this](std::uint64_t address)
                {
                    if (!address_edit_->hasFocus())
                    {
                        address_edit_->setText(display_text(address));
                    }
                });

        // A write result or rejection goes to the status line.
        connect(&document_,
                &components::MemoryViewDocument::statusChanged,
                this,
                [this](const QString& message, bool is_error)
                {
                    status_->set_status(is_error ? widgets::StatusKind::error : widgets::StatusKind::success, message);
                });
    }

    void MemoryViewerDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        // The coordinator only submits the window while the dialog is visible, so
        // showing it is enough to bring the page up to date.
        document_.set_visible(true);
        page_loaded_ = false;
        request_page();
    }

    void MemoryViewerDialog::set_address(std::uint64_t address)
    {
        view_->set_first_byte(address);
        address_edit_->setText(display_text(view_->first_byte()));

        // The cached window no longer matches the top byte.
        page_loaded_ = false;
        update_state();
        if (isVisible())
        {
            request_page();
        }
    }

    void MemoryViewerDialog::set_modules(std::vector<process::ModuleInfo> modules)
    {
        document_.set_modules(std::move(modules));
        address_edit_->setText(display_text(view_->first_byte()));
    }

    void MemoryViewerDialog::set_address_mode(ui::AddressMode mode)
    {
        document_.set_address_mode(mode);
        address_edit_->setText(display_text(view_->first_byte()));
    }

    QString MemoryViewerDialog::display_text(std::uint64_t address) const
    {
        return document_.display_text(address);
    }

    void MemoryViewerDialog::go_to_address()
    {
        const auto parsed = ui::parse_address_text(address_edit_->text().toStdString(), document_.module_spans());
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

    void MemoryViewerDialog::request_page()
    {
        manual_request_ = true;
        update_state();
        emit liveRefreshRequested();
    }

    std::vector<ui::LiveRequest> MemoryViewerDialog::next_live_request()
    {
        return document_.next_live_request();
    }

    void MemoryViewerDialog::apply_live_readings(std::span<const ui::LiveReading> readings)
    {
        if (readings.empty())
        {
            return;
        }
        document_.apply_live_readings(readings);
        page_loaded_    = true;
        manual_request_ = false;
        update_state();
    }

    void MemoryViewerDialog::update_state()
    {
        const bool loading = target_.valid() && (manual_request_ || !page_loaded_);
        loading_label_->setVisible(loading);
        refresh_button_->setEnabled(target_.valid());
        go_button_->setEnabled(target_.valid());

        if (!target_.valid())
        {
            status_->set_status(widgets::StatusKind::info, tr("No process attached."));
            return;
        }
        if (document_.any_unreadable() && !loading)
        {
            status_->set_status(widgets::StatusKind::warning, tr("Some bytes could not be read."));
            return;
        }
        status_->clear_status();
    }

} // namespace slopkit::ui::dialogs
