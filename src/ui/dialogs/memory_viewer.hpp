#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "debug/controller.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "ui/access_watch.hpp"
#include "ui/address_format.hpp"
#include "ui/components/code_patch.hpp"
#include "ui/components/disassembly_document.hpp"
#include "ui/components/memory_view_document.hpp"
#include "ui/debug_session.hpp"
#include "ui/live_values.hpp"

#include <QByteArray>
#include <QDialog>
#include <QString>

class QHideEvent;
class QSplitter;

namespace slopkit::ui::components
{
    class DisassemblyView;
    class MemoryView;
} // namespace slopkit::ui::components

namespace slopkit::ui::panels
{
    class DebugControls;
    class DebuggerPanel;
    class ViewerMenu;
} // namespace slopkit::ui::panels

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The investigation window of the attached target: live disassembly over
    // live debugger stats, above the byte view. Reads run on the access worker
    // and the panes render cached bytes, never touching a session; Go To
    // failures go to the log. A companion window: built without a parent so the
    // compositor stacks it normally, and not one of the windows that own the
    // process lifetime, so closing the shell still ends slopkit.
    class MemoryViewerDialog : public QDialog, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        MemoryViewerDialog(process::AccessWorker&   worker,
                           process::AttachedTarget& target,
                           debug::Controller&       debug,
                           QWidget*                 parent = nullptr);

        // Opens the viewer at `address`, seeding both panes' cursors.
        void set_address(std::uint64_t address);

        // The registers/call-stack pane, so the window can open the Breakpoints
        // dialog from the pane's button.
        [[nodiscard]] panels::DebuggerPanel* debugger_panel() const noexcept;

        // The one-line debug control bar above the listing, so the window can
        // open the Breakpoints dialog from its button.
        [[nodiscard]] panels::DebugControls* debug_controls() const noexcept;

        // The Memory Viewer's own menu bar, above the panes.
        [[nodiscard]] panels::ViewerMenu* viewer_menu() const noexcept;

        // The debugger read-out on the dialog's bottom status line.
        [[nodiscard]] QString status_text() const;

        // Jumps the byte view to an address written as absolute ("1040"), a
        // bare module name ("libc.so.6"), or module+RVA ("libc.so.6+1A2B");
        // false when the text does not name an address.
        bool go_to(const QString& text);
        // The same for the disassembly listing's own cursor.
        bool go_to_disassembly(const QString& text);

        // Resolves a listing row's memory operands against the controller's
        // register context and emits instructionAccessesResolved; nothing in the
        // target is touched. Attaches the debug session on demand first, and
        // captures the registers of the running target once.
        void instruction_accesses(std::size_t row);

        // The on-demand attach gate behind the listing command; also the test seam.
        [[nodiscard]] DebugSessionGate& debug_gate() noexcept;

        // Sets the module image spans used to render module-relative addresses.
        void set_modules(std::vector<process::ModuleInfo> modules);
        // Chooses how static addresses are shown in both panes' address columns.
        void set_address_mode(ui::AddressMode mode);

        // LiveSurface: the byte view's blocks followed by the listing's window
        // while the dialog is shown and a target is attached.
        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

    signals:
        // Asks the window's live coordinator for an immediate pass (Go To / show).
        void liveRefreshRequested();
        // The window's saved geometry (QWidget::saveGeometry), emitted whenever
        // it is hidden so MainWindow can persist it for the next run.
        void geometryChanged(QByteArray geometry);

        // The resolved operands of a listing row, for the Access Watch window.
        void instructionAccessesResolved(std::uint64_t                   instruction,
                                         std::size_t                     instruction_length,
                                         std::vector<ui::ResolvedAccess> accesses);
        // The attach and capture steps of the listing command, for the status line.
        void instructionAccessesProgress(const QString& text, bool error);

    protected:
        void showEvent(QShowEvent* event) override;
        // Hide is what a close becomes, so this single hook covers the menu,
        // Ctrl+W and the window manager's close button; a minimise is not a hide.
        void hideEvent(QHideEvent* event) override;

    private:
        // Which pane a Go To aimed at: the listing when the focus is inside it,
        // the byte view otherwise (including when nothing is focused).
        enum class GoToTarget
        {
            memory,
            disassembly,
        };

        [[nodiscard]] GoToTarget go_to_target() const;

        void     build_layout();
        QWidget* build_code_pane();
        QWidget* build_stats_pane();
        // Seeds the vertical 70/30 split once, on the first show.
        void     apply_ratios();

        // Asks for an address expression and jumps the focused pane to it.
        void                      prompt_go_to();
        void                      prompt_go_to(GoToTarget target);
        // Validates one Go To expression: an empty string when acceptable.
        [[nodiscard]] std::string validate_go_to(std::string_view text, GoToTarget target) const;
        // Submits a pointer-chain expression to the worker and jumps on completion.
        void                      resolve_go_to(const std::string& expression, GoToTarget target);

        [[nodiscard]] std::uint64_t          pane_address(GoToTarget target) const;
        [[nodiscard]] QString                pane_display_text(GoToTarget target, std::uint64_t address) const;
        [[nodiscard]] const ui::ModuleSpans& pane_module_spans(GoToTarget target) const;
        [[nodiscard]] bool                   pane_go_to(const QString& text, GoToTarget target);
        void                                 apply_pane_address(std::uint64_t address, GoToTarget target);

        // The listing command's continuation: capture a live register snapshot
        // when the target runs, then decode and resolve the remembered row.
        void resolve_instruction_accesses(std::size_t row);
        void finish_instruction_accesses(std::size_t row);

        // After a stop, jumps the listing to RIP and asks for a fresh live pass.
        void follow_stop();

        // The listing's row commands, shared by its context menu and the menu
        // bar's Tools menu: replace/restore/edit the selected instruction.
        void nop_instruction_row(std::size_t row);
        void restore_instruction_row(std::size_t row);
        void prompt_edit_instruction(std::size_t row);

        // The decoded listing row the viewer commands act on, or null when
        // nothing is selected.
        [[nodiscard]] std::optional<std::size_t>   selected_row() const;
        // The first address the selected instruction references, for Follow.
        [[nodiscard]] std::optional<std::uint64_t> selected_reference() const;
        // `View > Back`: returns the listing when the focus is inside it, the
        // byte view otherwise.
        void                                       back_focused_pane();
        void                                       follow_selected_instruction();
        void                                       follow_selected_in_memory_view();
        // Pushes the selection and the panes' history onto the menu bar's own
        // View/Tools enablement.
        void                                       refresh_menu_command_state();

        // Adds a software breakpoint at `address` when none is set there, and
        // removes the existing one otherwise. `expression` is the listing's own
        // rendered text for the row, which the controller resolves.
        void toggle_breakpoint(std::uint64_t address, const QString& expression);

        // Writes an error on the bottom status line, the same read-out the
        // controller's own failures use (the controller stays silent on a refused
        // `add_breakpoint`).
        void report_error(const QString& text);

        // Asks the coordinator for an immediate pass.
        void request_page();

        process::AccessWorker&          worker_;
        process::AttachedTarget&        target_;
        debug::Controller&              debug_;
        DebugSessionGate                gate_;
        components::CodePatchTable      patches_;
        components::MemoryViewDocument  document_;
        components::MemoryView*         view_ {};
        components::DisassemblyDocument disassembly_document_;
        components::DisassemblyView*    disassembly_ {};
        panels::DebugControls*          controls_ {};
        panels::ViewerMenu*             menu_ {};
        panels::DebuggerPanel*          debugger_ {};
        widgets::StatusLabel*           status_ {};
        QSplitter*                      split_ {};
        QSplitter*                      code_split_ {};
        bool                            ratios_applied_ {false};
        std::optional<process::JobId>   resolve_job_;
    };

} // namespace slopkit::ui::dialogs
