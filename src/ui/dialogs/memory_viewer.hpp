#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "ui/address_format.hpp"
#include "ui/components/disassembly_document.hpp"
#include "ui/components/memory_view_document.hpp"
#include "ui/live_values.hpp"

#include <QDialog>
#include <QString>

class QSplitter;

namespace slopkit::ui::components
{
    class DisassemblyView;
    class MemoryView;
} // namespace slopkit::ui::components

namespace slopkit::ui::models
{
    class RegisterModel;
} // namespace slopkit::ui::models

namespace slopkit::ui::dialogs
{

    // The investigation window of the attached target: live disassembly over
    // live debugger stats, above the byte view. Reads run on the access worker
    // and the panes render cached bytes, never touching a session; Go To
    // failures go to the log.
    class MemoryViewerDialog : public QDialog, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        MemoryViewerDialog(process::AccessWorker& worker, process::AttachedTarget& target, QWidget* parent = nullptr);

        // Opens the viewer at `address`, seeding both panes' cursors.
        void set_address(std::uint64_t address);

        // Jumps the byte view to an address written as absolute ("0x1040"), a
        // bare module name ("libc.so.6"), or module+RVA ("libc.so.6+1A2B");
        // false when the text does not name an address.
        bool go_to(const QString& text);
        // The same for the disassembly listing's own cursor.
        bool go_to_disassembly(const QString& text);

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

    protected:
        void showEvent(QShowEvent* event) override;

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

        // Asks the coordinator for an immediate pass.
        void request_page();

        process::AccessWorker&          worker_;
        process::AttachedTarget&        target_;
        components::MemoryViewDocument  document_;
        components::MemoryView*         view_ {};
        components::DisassemblyDocument disassembly_document_;
        components::DisassemblyView*    disassembly_ {};
        models::RegisterModel*          registers_ {};
        QSplitter*                      split_ {};
        QSplitter*                      code_split_ {};
        bool                            ratios_applied_ {false};
        std::optional<process::JobId>   resolve_job_;
    };

} // namespace slopkit::ui::dialogs
