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
#include "script/symbols.hpp"
#include "ui/access_watch.hpp"
#include "ui/address_format.hpp"
#include "ui/components/code_patch.hpp"
#include "ui/components/disassembly_document.hpp"
#include "ui/components/memory_view_document.hpp"
#include "ui/components/referenced_strings.hpp"
#include "ui/debug_session.hpp"
#include "ui/dialogs/referenced_strings.hpp"
#include "ui/live_values.hpp"

#include <QByteArray>
#include <QDialog>
#include <QPointer>
#include <QString>

class QSplitter;

namespace slopkit::ui::components
{
    class DisassemblyView;
    class MemoryView;
}

namespace slopkit::ui::panels
{
    class DebugControls;
    class DebuggerPanel;
    class ViewerMenu;
}

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    class MemoryViewerDialog : public QDialog, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        MemoryViewerDialog(process::AccessWorker&   worker,
                           process::AttachedTarget& target,
                           debug::Controller&       debug,
                           QWidget*                 parent  = nullptr,
                           script::SymbolTable&     symbols = script::default_symbol_table());

        void set_address(std::uint64_t address);

        [[nodiscard]] panels::DebuggerPanel* debugger_panel() const noexcept;

        [[nodiscard]] panels::DebugControls* debug_controls() const noexcept;

        [[nodiscard]] panels::ViewerMenu* viewer_menu() const noexcept;

        void refresh_target_state();

        [[nodiscard]] QString status_text() const;

        bool go_to(const QString& text);
        bool go_to_disassembly(const QString& text);

        void instruction_accesses(std::size_t row);

        void hook_instruction(std::size_t row);

        [[nodiscard]] DebugSessionGate& debug_gate() noexcept;

        void set_modules(std::vector<process::ModuleInfo> modules);
        void set_address_mode(ui::AddressMode mode);

        void analyze_module(const process::ModuleInfo& module);

        [[nodiscard]] const components::ReferencedStringTable& referenced_strings() const noexcept;
        [[nodiscard]] const std::vector<process::ModuleInfo>&  modules() const noexcept;
        [[nodiscard]] ReferencedStringsDialog*                 referenced_strings_dialog() const noexcept;

        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

    signals:
        void liveRefreshRequested();

        void instructionAccessesResolved(std::uint64_t                   instruction,
                                         std::size_t                     instruction_length,
                                         std::vector<ui::ResolvedAccess> accesses);
        void instructionAccessesProgress(const QString& text, bool error);

        void hookScriptRequested(const QString& description, const QString& source);

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        enum class GoToTarget
        {
            memory,
            disassembly,
        };

        [[nodiscard]] GoToTarget go_to_target() const;

        void     build_layout();
        QWidget* build_code_pane();
        QWidget* build_stats_pane();
        void     apply_ratios();

        void                      prompt_go_to();
        void                      prompt_go_to(GoToTarget target);
        void                      prompt_string_analysis();
        void                      show_referenced_strings();
        [[nodiscard]] std::string validate_go_to(std::string_view text, GoToTarget target) const;
        void                      resolve_go_to(const std::string& expression, GoToTarget target);

        [[nodiscard]] std::uint64_t          pane_address(GoToTarget target) const;
        [[nodiscard]] QString                pane_display_text(GoToTarget target, std::uint64_t address) const;
        [[nodiscard]] const ui::ModuleSpans& pane_module_spans(GoToTarget target) const;
        [[nodiscard]] bool                   pane_go_to(const QString& text, GoToTarget target);
        void                                 apply_pane_address(std::uint64_t address, GoToTarget target);

        void resolve_instruction_accesses(std::size_t row);
        void finish_instruction_accesses(std::size_t row);

        void follow_stop();

        void nop_instruction_row(std::size_t row);
        void restore_instruction_row(std::size_t row);
        void prompt_edit_instruction(std::size_t row);

        [[nodiscard]] std::optional<std::size_t>   selected_row() const;
        [[nodiscard]] std::optional<std::uint64_t> selected_reference() const;
        void                                       back_focused_pane();
        void                                       follow_selected_instruction();
        void                                       follow_selected_in_memory_view();
        void                                       refresh_menu_command_state();

        void toggle_breakpoint(std::uint64_t address, const QString& expression);

        void report_error(const QString& text);

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

        std::vector<process::ModuleInfo>  modules_;
        components::ReferencedStringTable strings_;
        std::optional<process::JobId>     strings_job_;
        std::uint64_t                     strings_pid_ {};
        QPointer<ReferencedStringsDialog> strings_dialog_;
    };

}
