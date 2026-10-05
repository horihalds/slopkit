#include "ui/dialogs/memory_viewer.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/memory_view.hpp"
#include "ui/components/widgets.hpp"

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
        // The byte view fills the window; only the loading and status lines share
        // the space below it.
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(4);

        view_ = new components::MemoryView(document_, this);
        layout->addWidget(view_, 1);

        // The loading and status rows keep a constant height: hiding or clearing
        // them would resize the byte view, re-align the live window and drop the
        // pass in flight.
        loading_label_ = new QLabel(tr("Loading..."), this);
        loading_label_->setObjectName(QStringLiteral("loading_label"));
        loading_label_->setMinimumHeight(loading_label_->fontMetrics().height());
        layout->addWidget(loading_label_);

        status_ = new widgets::StatusLabel(this);
        status_->setMinimumHeight(status_->fontMetrics().height());
        layout->addWidget(status_);

        // "Go To..." in the right-click menu asks for an address here.
        connect(view_, &components::MemoryView::gotoRequested, this, &MemoryViewerDialog::prompt_go_to);

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

        // The cached window no longer matches the top byte.
        page_loaded_ = false;
        update_state();
        if (isVisible())
        {
            request_page();
        }
    }

    bool MemoryViewerDialog::go_to(const QString& text)
    {
        if (!view_->go_to(text))
        {
            return false;
        }
        page_loaded_ = false;
        update_state();
        if (isVisible())
        {
            request_page();
        }
        return true;
    }

    void MemoryViewerDialog::prompt_go_to()
    {
        bool          ok   = false;
        const QString text = QInputDialog::getText(this,
                                                   tr("Go To"),
                                                   tr("Address or module+RVA:"),
                                                   QLineEdit::Normal,
                                                   document_.display_text(view_->first_byte()),
                                                   &ok);
        if (!ok || text.isEmpty())
        {
            return;
        }
        if (!go_to(text))
        {
            log::warning(log::category::ui, "memory viewer got an unparseable address");
        }
    }

    void MemoryViewerDialog::set_modules(std::vector<process::ModuleInfo> modules)
    {
        document_.set_modules(std::move(modules));
    }

    void MemoryViewerDialog::set_address_mode(ui::AddressMode mode)
    {
        document_.set_address_mode(mode);
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
        loading_label_->setText(loading ? tr("Loading...") : QString());

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
