#include "ui/dialogs/memory_viewer.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QApplication>
#include <QHeaderView>
#include <QShowEvent>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/components/disassembly_view.hpp"
#include "ui/components/input_box.hpp"
#include "ui/components/memory_view.hpp"
#include "ui/components/widgets.hpp"
#include "ui/models/register_model.hpp"

namespace slopkit::ui::dialogs
{

    MemoryViewerDialog::MemoryViewerDialog(process::AccessWorker&   worker,
                                           process::AttachedTarget& target,
                                           QWidget*                 parent)
        : QDialog(parent), worker_(worker), target_(target), document_(worker, target, this),
          disassembly_document_(worker, target, this)
    {
        setWindowTitle(tr("Memory Viewer"));
        resize(1100, 720);
        setMinimumSize(720, 480);

        build_layout();
        set_address(0);
    }

    void MemoryViewerDialog::build_layout()
    {
        // Disassembly over debugger stats, above the byte view. No status row:
        // the viewer reports through the log.
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);

        view_       = new components::MemoryView(document_, this);
        split_      = new QSplitter(Qt::Vertical, this);
        code_split_ = new QSplitter(Qt::Horizontal, this);
        split_->setObjectName(QStringLiteral("viewer_split"));
        code_split_->setObjectName(QStringLiteral("code_split"));
        split_->setChildrenCollapsible(false);
        code_split_->setChildrenCollapsible(false);

        code_split_->addWidget(build_code_pane());
        code_split_->addWidget(build_stats_pane());
        code_split_->setStretchFactor(0, 70);
        code_split_->setStretchFactor(1, 30);

        split_->addWidget(code_split_);
        split_->addWidget(view_);
        split_->setStretchFactor(0, 50);
        split_->setStretchFactor(1, 40);

        layout->addWidget(split_, 1);

        // "Go To..." in the byte view's menu (or Ctrl+G) asks for an address here;
        // the focused pane decides which cursor it moves.
        connect(view_,
                &components::MemoryView::gotoRequested,
                this,
                [this]
                {
                    prompt_go_to();
                });
    }

    QWidget* MemoryViewerDialog::build_code_pane()
    {
        auto* panel  = new widgets::Panel(tr("Disassembly"), this);
        disassembly_ = new components::DisassemblyView(disassembly_document_, panel);
        panel->body()->addWidget(disassembly_);

        // The listing's own menu entry always targets the listing.
        connect(disassembly_,
                &components::DisassemblyView::gotoRequested,
                this,
                [this]
                {
                    prompt_go_to(GoToTarget::disassembly);
                });
        return panel;
    }

    QWidget* MemoryViewerDialog::build_stats_pane()
    {
        auto* panel = new widgets::Panel(tr("Debugger"), this);
        panel->body()->addWidget(widgets::hint_text(tr("Register values arrive with the debugger."), panel));

        registers_  = new models::RegisterModel(this);
        auto* table = new QTableView(panel);
        table->setObjectName(QStringLiteral("register_table"));
        table->setModel(registers_);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setFocusPolicy(Qt::NoFocus);
        table->setShowGrid(false);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        panel->body()->addWidget(table, 1);
        return panel;
    }

    void MemoryViewerDialog::apply_ratios()
    {
        if (ratios_applied_)
        {
            return;
        }
        const int total = split_->height();
        if (total <= 0)
        {
            return; // Retry on a later show/geometry update.
        }
        ratios_applied_ = true;

        // 50 : 40 reads as "50 of 90 units above, 40 below"; the stretch
        // factors keep the ratio on every later resize.
        const int upper = std::max(1, total * 50 / 90);
        split_->setSizes({upper, total - upper});
    }

    void MemoryViewerDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        // The coordinator only submits a pane's window while it is visible, so
        // showing the dialog is enough to bring every pane up to date.
        document_.set_visible(true);
        disassembly_document_.set_visible(true);
        apply_ratios();
        request_page();
    }

    void MemoryViewerDialog::set_address(std::uint64_t address)
    {
        view_->set_first_byte(address);
        disassembly_->set_first_address(address);
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
        if (isVisible())
        {
            request_page();
        }
        return true;
    }

    bool MemoryViewerDialog::go_to_disassembly(const QString& text)
    {
        if (!disassembly_->go_to(text))
        {
            return false;
        }
        if (isVisible())
        {
            request_page();
        }
        return true;
    }

    MemoryViewerDialog::GoToTarget MemoryViewerDialog::go_to_target() const
    {
        const QWidget* focused = QApplication::focusWidget();
        if (focused != nullptr && (focused == disassembly_ || disassembly_->isAncestorOf(focused)))
        {
            return GoToTarget::disassembly;
        }
        return GoToTarget::memory;
    }

    std::uint64_t MemoryViewerDialog::pane_address(GoToTarget target) const
    {
        return target == GoToTarget::disassembly ? disassembly_->first_address() : view_->first_byte();
    }

    QString MemoryViewerDialog::pane_display_text(GoToTarget target, std::uint64_t address) const
    {
        return target == GoToTarget::disassembly ? disassembly_document_.address_text(address)
                                                 : document_.display_text(address);
    }

    const ui::ModuleSpans& MemoryViewerDialog::pane_module_spans(GoToTarget target) const
    {
        return target == GoToTarget::disassembly ? disassembly_document_.module_spans() : document_.module_spans();
    }

    bool MemoryViewerDialog::pane_go_to(const QString& text, GoToTarget target)
    {
        return target == GoToTarget::disassembly ? go_to_disassembly(text) : go_to(text);
    }

    void MemoryViewerDialog::apply_pane_address(std::uint64_t address, GoToTarget target)
    {
        if (target == GoToTarget::disassembly)
        {
            disassembly_->set_first_address(address);
        }
        else
        {
            view_->set_first_byte(address);
        }
        if (isVisible())
        {
            request_page();
        }
    }

    void MemoryViewerDialog::prompt_go_to()
    {
        prompt_go_to(go_to_target());
    }

    void MemoryViewerDialog::prompt_go_to(GoToTarget target)
    {
        widgets::InputBoxOptions options;
        options.title       = tr("Go To");
        options.label       = tr("Address or expression:");
        options.initial     = pane_display_text(target, pane_address(target));
        options.placeholder = QStringLiteral("module, module+0x10 or module+0d+5d+44");
        options.monospace   = true;
        options.validate    = [this, target](const QString& text)
        {
            return QString::fromStdString(validate_go_to(text.toStdString(), target));
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
            if (pane_go_to(*accepted, target))
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
        resolve_go_to(accepted->toStdString(), target);
    }

    std::string MemoryViewerDialog::validate_go_to(std::string_view text, GoToTarget target) const
    {
        const auto expression = expr::parse(text);
        if (!expression)
        {
            return expression.error().message;
        }
        if (ui::module_base(pane_module_spans(target), expression->base).has_value())
        {
            return {};
        }
        if (expr::parse_literal(expression->base).has_value())
        {
            return {};
        }
        return std::format("unknown module or literal '{}'", expression->base);
    }

    void MemoryViewerDialog::resolve_go_to(const std::string& expression, GoToTarget target)
    {
        const process::JobId id = worker_.next_job_id();
        resolve_job_            = id;

        const bool submitted = worker_.submit_resolve_expressions(
            id,
            std::vector<process::ResolveRequest> {
                process::ResolveRequest {.key = 0, .expression = expression}
        },
            ui::module_refs(pane_module_spans(target)),
            8,
            [this, id, expression, target](process::JobResult&& result)
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
                    apply_pane_address(*resolved.items.front().address, target);
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
        document_.set_modules(modules);
        disassembly_document_.set_modules(std::move(modules));
    }

    void MemoryViewerDialog::set_address_mode(ui::AddressMode mode)
    {
        document_.set_address_mode(mode);
        disassembly_document_.set_address_mode(mode);
    }

    void MemoryViewerDialog::request_page()
    {
        emit liveRefreshRequested();
    }

    std::vector<ui::LiveRequest> MemoryViewerDialog::next_live_request()
    {
        std::vector<ui::LiveRequest>       requests = document_.next_live_request();
        const std::vector<ui::LiveRequest> code     = disassembly_document_.next_live_request();
        requests.insert(requests.end(), code.begin(), code.end());
        return requests;
    }

    void MemoryViewerDialog::apply_live_readings(std::span<const ui::LiveReading> readings)
    {
        if (readings.empty())
        {
            return;
        }
        // The ids identify their owner, so one merged span serves both panes.
        document_.apply_live_readings(readings);
        disassembly_document_.apply_live_readings(readings);
    }

} // namespace slopkit::ui::dialogs
