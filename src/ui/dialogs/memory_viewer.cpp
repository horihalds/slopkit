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
#include <QHideEvent>
#include <QMenu>
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
#include "ui/fonts.hpp"
#include "ui/panels/debug_controls.hpp"
#include "ui/panels/debugger_panel.hpp"
#include "ui/panels/viewer_menu.hpp"

namespace slopkit::ui::dialogs
{
    namespace
    {
        widgets::StatusKind status_kind(debug::Controller::MessageKind kind)
        {
            switch (kind)
            {
            case debug::Controller::MessageKind::success:
                return widgets::StatusKind::success;
            case debug::Controller::MessageKind::warning:
                return widgets::StatusKind::warning;
            case debug::Controller::MessageKind::error:
                return widgets::StatusKind::error;
            case debug::Controller::MessageKind::info:
            default:
                return widgets::StatusKind::info;
            }
        }
    } // namespace

    MemoryViewerDialog::MemoryViewerDialog(process::AccessWorker&   worker,
                                           process::AttachedTarget& target,
                                           debug::Controller&       debug,
                                           QWidget*                 parent)
        : QDialog(parent), worker_(worker), target_(target), debug_(debug), gate_(debug, target, *this, this),
          document_(worker, target, this), disassembly_document_(worker, target, patches_, this)
    {
        setWindowTitle(tr("Memory Viewer"));
        resize(1280, 960);
        setMinimumSize(720, 480);
        // Built without a parent (docs/UI_DESIGN.md#wayland) so the compositor
        // may stack it normally. That also makes it a top-level window of its
        // own, and a top-level window owns the process lifetime by default: it
        // must not, or closing the shell would leave slopkit running with no way
        // back to it.
        setAttribute(Qt::WA_QuitOnClose, false);

        build_layout();

        // Every stop moves the listing to the stopping instruction and asks for a
        // fresh pass, so the bytes around RIP are current.
        connect(&debug_, &debug::Controller::stopped, this, &MemoryViewerDialog::follow_stop);

        set_address(0);
    }

    void MemoryViewerDialog::build_layout()
    {
        // Disassembly over debugger stats, above the byte view, with the
        // debugger read-out on a status line at the bottom.
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);

        view_       = new components::MemoryView(document_, this);
        split_      = new QSplitter(Qt::Vertical, this);
        code_split_ = new QSplitter(Qt::Horizontal, this);
        split_->setObjectName(QStringLiteral("viewer_split"));
        code_split_->setObjectName(QStringLiteral("code_split"));
        split_->setChildrenCollapsible(false);
        code_split_->setChildrenCollapsible(false);

        // The control bar and the listing share the left column of the code
        // split, so the bar acts on the listing and its width is that column's
        // width instead of reaching across the registers pane.
        auto* code_pane        = new QWidget(this);
        auto* code_pane_layout = new QVBoxLayout(code_pane);
        code_pane_layout->setContentsMargins(0, 0, 0, 0);
        code_pane_layout->setSpacing(6);
        controls_ = new panels::DebugControls(debug_, target_, code_pane);
        // The bar sits inside the listing column, so it must not raise that
        // column's minimum width and skew the 70/30 code split: it takes the
        // column's width and its row shares it.
        controls_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        code_pane_layout->addWidget(controls_);
        code_pane_layout->addWidget(build_code_pane(), 1);

        // The viewer's own menu bar sits above the split: File, View and Tools
        // carry the viewer's commands, Debug mirrors the one-line control bar
        // below it.
        menu_ = new panels::ViewerMenu(debug_, target_, *view_, this);
        layout->setMenuBar(menu_);

        code_split_->addWidget(code_pane);
        code_split_->addWidget(build_stats_pane());
        code_split_->setStretchFactor(0, 70);
        code_split_->setStretchFactor(1, 30);

        // The control bar comes first in the tab order, then the pane's tables.
        widgets::chain_tab_order(
            {controls_->breakpoints_button(), debugger_->register_table(), debugger_->call_stack_table()});

        // The toggle acts on the selected listing row: the controller resolves
        // the row's own rendered address text, so the two never disagree. The
        // menu bar's toggle reads the same pair.
        connect(
            controls_, &panels::DebugControls::toggleBreakpointRequested, this, &MemoryViewerDialog::toggle_breakpoint);
        connect(disassembly_,
                &components::DisassemblyView::selectionChanged,
                this,
                [this](std::optional<std::uint64_t> address)
                {
                    const QString text = address.has_value() ? disassembly_document_.address_text(*address) : QString();
                    controls_->set_selected_instruction(address, text);
                    menu_->set_selected_instruction(address, text);
                    refresh_menu_command_state();
                });

        // The menu bar's debug side drives the same controller calls as the bar,
        // and Breakpoints... is relayed into the bar's own request so
        // MainWindow's single wiring covers both surfaces.
        connect(menu_, &panels::ViewerMenu::toggleBreakpointRequested, this, &MemoryViewerDialog::toggle_breakpoint);
        connect(
            menu_, &panels::ViewerMenu::breakpointsRequested, controls_, &panels::DebugControls::breakpointsRequested);

        // View/Tools act on the dialog's own selection, focus and panes.
        connect(menu_, &panels::ViewerMenu::closeRequested, this, &MemoryViewerDialog::close);
        connect(menu_, &panels::ViewerMenu::backRequested, this, &MemoryViewerDialog::back_focused_pane);
        connect(menu_, &panels::ViewerMenu::followRequested, this, &MemoryViewerDialog::follow_selected_instruction);
        connect(menu_,
                &panels::ViewerMenu::followInMemoryViewRequested,
                this,
                &MemoryViewerDialog::follow_selected_in_memory_view);
        connect(menu_,
                &panels::ViewerMenu::nopRequested,
                this,
                [this]
                {
                    if (const auto row = selected_row(); row.has_value())
                    {
                        nop_instruction_row(*row);
                    }
                });
        connect(menu_,
                &panels::ViewerMenu::restoreRequested,
                this,
                [this]
                {
                    if (const auto row = selected_row(); row.has_value())
                    {
                        restore_instruction_row(*row);
                    }
                });
        connect(menu_,
                &panels::ViewerMenu::editRequested,
                this,
                [this]
                {
                    if (const auto row = selected_row(); row.has_value())
                    {
                        prompt_edit_instruction(*row);
                    }
                });
        connect(menu_,
                &panels::ViewerMenu::instructionAccessesRequested,
                this,
                [this]
                {
                    if (const auto row = selected_row(); row.has_value())
                    {
                        instruction_accesses(*row);
                    }
                });
        // Back depends on the focused pane's history, which changes without a
        // selection change, so refresh just before the menus open.
        connect(menu_->view_menu(), &QMenu::aboutToShow, this, &MemoryViewerDialog::refresh_menu_command_state);
        connect(menu_->tools_menu(), &QMenu::aboutToShow, this, &MemoryViewerDialog::refresh_menu_command_state);

        split_->addWidget(code_split_);
        split_->addWidget(view_);
        split_->setStretchFactor(0, 70); // upper zone
        split_->setStretchFactor(1, 30); // hex byte view

        layout->addWidget(split_, 1);

        // The debugger read-out: the controller's state and its messages land on
        // one status line spanning the window below the split. It prints
        // addresses, so it is a mono line like the tables.
        status_ = new widgets::StatusLabel(this);
        status_->setObjectName(QStringLiteral("viewer_status"));
        status_->setFont(mono_font());
        layout->addWidget(status_);

        connect(&debug_,
                &debug::Controller::stateChanged,
                this,
                [this]
                {
                    status_->set_status(widgets::StatusKind::info, debug_.state_text());
                });
        connect(&debug_,
                &debug::Controller::message,
                this,
                [this](debug::Controller::MessageKind kind, const QString& text)
                {
                    status_->set_status(status_kind(kind), text);
                });
        status_->set_status(widgets::StatusKind::info, debug_.state_text());

        // "Go To..." in the byte view's menu (or Ctrl+G) asks for an address here;
        // the focused pane decides which cursor it moves.
        connect(view_,
                &components::MemoryView::gotoRequested,
                this,
                [this]
                {
                    prompt_go_to();
                });
        // A navigation the pane performed on its own (Follow / Back) re-reads it.
        connect(view_, &components::MemoryView::navigated, this, &MemoryViewerDialog::request_page);
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
        connect(disassembly_, &components::DisassemblyView::navigated, this, &MemoryViewerDialog::request_page);
        // "Follow in Memory View" inspects the referenced address in the byte
        // view; the listing's own cursor stays put.
        connect(disassembly_,
                &components::DisassemblyView::followInMemoryViewRequested,
                this,
                [this](std::uint64_t address)
                {
                    view_->navigate_to(address);
                });
        connect(disassembly_,
                &components::DisassemblyView::instructionAccessesRequested,
                this,
                &MemoryViewerDialog::instruction_accesses);
        // Rewriting the attached target's code: the document applies the write
        // through the access worker and the byte pane catches up on a fresh pass
        // once it is submitted. The same handlers back the menu bar's Tools menu.
        connect(
            disassembly_, &components::DisassemblyView::nopRequested, this, &MemoryViewerDialog::nop_instruction_row);
        connect(disassembly_,
                &components::DisassemblyView::restoreRequested,
                this,
                &MemoryViewerDialog::restore_instruction_row);
        connect(disassembly_,
                &components::DisassemblyView::editRequested,
                this,
                &MemoryViewerDialog::prompt_edit_instruction);
        return panel;
    }

    void MemoryViewerDialog::nop_instruction_row(std::size_t row)
    {
        if (disassembly_document_.nop_instruction(row))
        {
            request_page();
        }
    }

    void MemoryViewerDialog::restore_instruction_row(std::size_t row)
    {
        if (const components::CodePatch* patch = disassembly_document_.patch_at(row);
            patch != nullptr && disassembly_document_.restore_instruction(patch->begin))
        {
            request_page();
        }
    }

    // Editing assembles the text first (the box validates every keystroke) and
    // refuses anything longer than the instruction it replaces.
    void MemoryViewerDialog::prompt_edit_instruction(std::size_t row)
    {
        const auto source = disassembly_document_.edit_source(row);
        if (!source.has_value())
        {
            return;
        }

        widgets::InputBoxOptions options;
        options.title       = tr("Edit Instruction");
        options.label       = tr("Assembler (addresses are absolute):");
        options.initial     = *source;
        options.placeholder = QStringLiteral("MOV RBP, RSP");
        options.monospace   = true;
        options.validate    = [this, row](const QString& text)
        {
            return QString::fromStdString(disassembly_document_.edit_error(row, text.toStdString()));
        };

        const auto accepted = widgets::get_text(options, this);
        if (!accepted || accepted->isEmpty())
        {
            return;
        }
        if (disassembly_document_.edit_instruction(row, accepted->toStdString()))
        {
            request_page();
        }
    }

    std::optional<std::size_t> MemoryViewerDialog::selected_row() const
    {
        const auto address = disassembly_->selected_address();
        if (!address.has_value())
        {
            return std::nullopt;
        }
        return disassembly_document_.row_at(*address);
    }

    std::optional<std::uint64_t> MemoryViewerDialog::selected_reference() const
    {
        const auto row = selected_row();
        if (!row.has_value())
        {
            return std::nullopt;
        }
        const auto references = disassembly_document_.row_addresses(*row);
        if (references.empty())
        {
            return std::nullopt;
        }
        return references.front().address;
    }

    void MemoryViewerDialog::back_focused_pane()
    {
        if (go_to_target() == GoToTarget::disassembly)
        {
            disassembly_->back();
        }
        else
        {
            view_->back();
        }
    }

    void MemoryViewerDialog::follow_selected_instruction()
    {
        if (const auto reference = selected_reference(); reference.has_value())
        {
            disassembly_->navigate_to(*reference);
        }
    }

    void MemoryViewerDialog::follow_selected_in_memory_view()
    {
        if (const auto reference = selected_reference(); reference.has_value())
        {
            view_->navigate_to(*reference);
        }
    }

    void MemoryViewerDialog::refresh_menu_command_state()
    {
        panels::ViewerMenu::CommandState state;

        if (const auto row = selected_row(); row.has_value())
        {
            state.selected_is_instruction     = disassembly_document_.editable(*row);
            state.selected_has_memory_operand = !disassembly_document_.row_memory(*row).empty();
            state.can_follow                  = !disassembly_document_.row_addresses(*row).empty();
            if (const components::CodePatch* patch = disassembly_document_.patch_at(*row); patch != nullptr)
            {
                state.selected_is_patched = true;
                state.restore_description =
                    tr("Write the instruction this session replaced at %1 (%2) back.")
                        .arg(disassembly_document_.address_text(patch->begin), patch->original_text);
            }
        }

        const bool listing_focused = go_to_target() == GoToTarget::disassembly;
        state.can_go_back          = listing_focused ? disassembly_->can_go_back() : view_->can_go_back();

        menu_->set_command_state(state);
    }

    QWidget* MemoryViewerDialog::build_stats_pane()
    {
        // No panel title: the pane only holds the register table and the call
        // stack, and those two carry their own section headers.
        auto* panel = new widgets::Panel(QString(), this);
        debugger_   = new panels::DebuggerPanel(debug_, panel);
        panel->body()->addWidget(debugger_);
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

        // 70 : 30 reads as "70 of 100 units above, 30 below"; the stretch
        // factors keep the ratio on every later resize.
        const int upper = std::max(1, total * 70 / 100);
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

    void MemoryViewerDialog::hideEvent(QHideEvent* event)
    {
        QDialog::hideEvent(event);
        emit geometryChanged(saveGeometry());
    }

    void MemoryViewerDialog::set_address(std::uint64_t address)
    {
        // A new open-at address starts a clean navigation session.
        view_->clear_history();
        disassembly_->clear_history();
        view_->set_first_byte(address);
        disassembly_->set_first_address(address);
        if (isVisible())
        {
            request_page();
        }
    }

    bool MemoryViewerDialog::go_to(const QString& text)
    {
        return view_->go_to(text);
    }

    bool MemoryViewerDialog::go_to_disassembly(const QString& text)
    {
        return disassembly_->go_to(text);
    }

    void MemoryViewerDialog::instruction_accesses(std::size_t row)
    {
        gate_.with_session(tr("find out what this instruction accesses"),
                           [this, row]
                           {
                               resolve_instruction_accesses(row);
                           });
    }

    DebugSessionGate& MemoryViewerDialog::debug_gate() noexcept
    {
        return gate_;
    }

    void MemoryViewerDialog::resolve_instruction_accesses(std::size_t row)
    {
        if (debug_.state() == debug::Controller::State::stopped)
        {
            // The register file of the last stop is already cached; read nothing.
            finish_instruction_accesses(row);
            return;
        }
        // One invisible stop: the target is put back running before the operands
        // are painted, and nothing about the session changes.
        debug_.capture_registers(
            [this, row]
            {
                finish_instruction_accesses(row);
            });
    }

    void MemoryViewerDialog::finish_instruction_accesses(std::size_t row)
    {
        const components::DisassemblyDocument::Row decoded  = disassembly_document_.row(row);
        const std::span<const disasm::MemoryRef>   operands = disassembly_document_.row_memory(row);
        if (debug_.registers().empty())
        {
            emit instructionAccessesProgress(tr("Could not read the registers; the operands stay unresolved."), true);
        }
        emit instructionAccessesResolved(
            decoded.address,
            decoded.length,
            ui::resolve_accesses(operands, decoded.address, decoded.length, debug_.registers()));
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
        // A pointer-chain jump is an explicit navigation, so it is undoable.
        if (target == GoToTarget::disassembly)
        {
            disassembly_->navigate_to(address);
        }
        else
        {
            view_->navigate_to(address);
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
        options.placeholder = QStringLiteral("module, module+10 or module+0d+5d+44");
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
        debugger_->refresh();
    }

    void MemoryViewerDialog::set_address_mode(ui::AddressMode mode)
    {
        document_.set_address_mode(mode);
        disassembly_document_.set_address_mode(mode);
        debugger_->refresh();
    }

    panels::DebuggerPanel* MemoryViewerDialog::debugger_panel() const noexcept
    {
        return debugger_;
    }

    panels::DebugControls* MemoryViewerDialog::debug_controls() const noexcept
    {
        return controls_;
    }

    panels::ViewerMenu* MemoryViewerDialog::viewer_menu() const noexcept
    {
        return menu_;
    }

    QString MemoryViewerDialog::status_text() const
    {
        return status_->text();
    }

    void MemoryViewerDialog::follow_stop()
    {
        const debug::StopEvent& stop = debug_.last_stop();
        if (stop.address == 0)
        {
            return;
        }
        disassembly_->navigate_to(stop.address);
        // A hit leaves the stopped instruction selected, so the bar's toggle
        // has a target straight away.
        disassembly_->set_selected_address(stop.address);
        emit liveRefreshRequested();
    }

    void MemoryViewerDialog::toggle_breakpoint(std::uint64_t address, const QString& expression)
    {
        if (const debug::Breakpoint* entry = debug_.table().software_at(address); entry != nullptr)
        {
            debug_.remove_breakpoint(entry->id);
            return;
        }
        if (const auto added = debug_.add_breakpoint(expression.toStdString(), debug::Kind::software, 1); !added)
        {
            // The controller reports success through its signal but stays silent
            // on a refusal, so surface the reason on the status line here.
            report_error(QString::fromStdString(added.error()));
        }
    }

    void MemoryViewerDialog::report_error(const QString& text)
    {
        status_->set_status(widgets::StatusKind::error, text);
    }

    void MemoryViewerDialog::request_page()
    {
        emit liveRefreshRequested();
    }

    std::vector<ui::LiveRequest> MemoryViewerDialog::next_live_request()
    {
        // A changed target, or a detach, makes every remembered patch stale.
        patches_.note_target(target_.valid() ? target_.pid : 0);
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
