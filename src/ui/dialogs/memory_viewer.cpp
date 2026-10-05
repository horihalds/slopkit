#include "ui/dialogs/memory_viewer.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/input_box.hpp"
#include "ui/components/memory_view.hpp"
#include "ui/components/widgets.hpp"

namespace slopkit::ui::dialogs
{

    MemoryViewerDialog::MemoryViewerDialog(process::AccessWorker&   worker,
                                           process::AttachedTarget& target,
                                           QWidget*                 parent)
        : QDialog(parent), worker_(worker), target_(target), document_(worker, target, this)
    {
        setWindowTitle(tr("Memory Viewer"));
        resize(760, 560);

        build_layout();
        set_address(0);
    }

    void MemoryViewerDialog::build_layout()
    {
        // The byte view is the whole dialog: there is no status or loading line.
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);

        view_ = new components::MemoryView(document_, this);
        layout->addWidget(view_, 1);

        // "Go To..." in the right-click menu (or Ctrl+G) asks for an address here.
        connect(view_, &components::MemoryView::gotoRequested, this, &MemoryViewerDialog::prompt_go_to);
    }

    void MemoryViewerDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        // The coordinator only submits the window while the dialog is visible, so
        // showing it is enough to bring the page up to date.
        document_.set_visible(true);
        request_page();
    }

    void MemoryViewerDialog::set_address(std::uint64_t address)
    {
        view_->set_first_byte(address);
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
        set_address(view_->first_byte());
        return true;
    }

    void MemoryViewerDialog::prompt_go_to()
    {
        widgets::InputBoxOptions options;
        options.title       = tr("Go To");
        options.label       = tr("Address or expression:");
        options.initial     = document_.display_text(view_->first_byte());
        options.placeholder = QStringLiteral("module, module+0x10 or module+0d+5d+44");
        options.monospace   = true;
        options.validate    = [this](const QString& text)
        {
            return QString::fromStdString(validate_go_to(text.toStdString()));
        };

        const auto accepted = widgets::get_text(options, this);
        if (!accepted || accepted->isEmpty())
        {
            return;
        }

        const auto expression = expr::parse(accepted->toStdString());
        if (!expression)
        {
            return; // the live validator already reported it
        }
        if (expression->pointer_levels() == 0)
        {
            if (go_to(*accepted))
            {
                log::info(log::category::ui, std::format("memory viewer go to {}", accepted->toStdString()));
            }
            else
            {
                log::warning(
                    log::category::ui,
                    std::format("memory viewer go to failed: {} (unparseable address)", accepted->toStdString()));
            }
            return;
        }
        resolve_go_to(accepted->toStdString());
    }

    std::string MemoryViewerDialog::validate_go_to(std::string_view text) const
    {
        const auto expression = expr::parse(text);
        if (!expression)
        {
            return expression.error().message;
        }
        if (ui::module_base(document_.module_spans(), expression->base).has_value())
        {
            return {};
        }
        if (expr::parse_literal(expression->base).has_value())
        {
            return {};
        }
        return std::format("unknown module or literal '{}'", expression->base);
    }

    void MemoryViewerDialog::resolve_go_to(const std::string& expression)
    {
        const process::JobId id = worker_.next_job_id();
        resolve_job_            = id;

        const bool submitted = worker_.submit_resolve_expressions(
            id,
            std::vector<process::ResolveRequest> {
                process::ResolveRequest {.key = 0, .expression = expression}
        },
            ui::module_refs(document_.module_spans()),
            8,
            [this, id, expression](process::JobResult&& result)
            {
                if (resolve_job_ != id)
                {
                    return; // superseded by a later prompt
                }
                resolve_job_.reset();

                const auto& resolved = std::get<process::ResolveResult>(result);
                if (!resolved.error.has_value() && !resolved.items.empty()
                    && resolved.items.front().address.has_value())
                {
                    log::info(
                        log::category::ui,
                        std::format("memory viewer go to {} -> {:#x}", expression, *resolved.items.front().address));
                    set_address(*resolved.items.front().address);
                    return;
                }
                const std::string reason = resolved.error.has_value() || resolved.items.empty()
                                             ? std::string("no target attached")
                                             : resolved.items.front().error;
                log::warning(log::category::ui, std::format("memory viewer go to failed: {} ({})", expression, reason));
            });

        if (!submitted)
        {
            resolve_job_.reset();
            log::warning(log::category::ui,
                         std::format("memory viewer go to failed: {} (could not start the resolve)", expression));
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
    }

} // namespace slopkit::ui::dialogs
