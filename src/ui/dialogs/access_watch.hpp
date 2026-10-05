#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "debug/controller.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "ui/access_watch.hpp"
#include "ui/debug_session.hpp"

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;
class QTableView;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The non-modal Access Watch window: the recorded accesses of the one
    // hardware watch the controller runs, and the resolved operands of an
    // instruction picked in the Memory Viewer. It only renders what the
    // controller and the access worker produce.
    class AccessWatchDialog : public QDialog
    {
        Q_OBJECT

    public:
        AccessWatchDialog(debug::Controller&       controller,
                          process::AccessWorker&   worker,
                          process::AttachedTarget& target,
                          QWidget*                 parent = nullptr);

        // Arms a watch through the controller, reporting a failure in the status
        // line. The two Watch buttons funnel through here.
        void start_watch(std::uint64_t address, debug::Kind kind, std::size_t size);

        // The context-menu entry point: attach the debug session on demand (after
        // asking) and then arm the watch through start_watch(). The Access Watch
        // window is only shown once the watch is actually armed.
        void arm_watch(std::uint64_t address, debug::Kind kind, std::size_t size);

        [[nodiscard]] DebugSessionGate& debug_gate() noexcept;
        [[nodiscard]] bool              watch_pending() const noexcept;

        // Fills the `Instruction accesses` table from the listing command.
        void show_instruction_accesses(std::uint64_t                   instruction,
                                       std::size_t                     instruction_length,
                                       std::vector<ui::ResolvedAccess> accesses);

        [[nodiscard]] QString header_text() const;
        [[nodiscard]] QString hint_text() const;
        [[nodiscard]] QString status_text() const;

        [[nodiscard]] QTableView*  recorded_table() const noexcept;
        [[nodiscard]] QTableView*  instruction_table() const noexcept;
        [[nodiscard]] QPushButton* follow_button() const noexcept;
        [[nodiscard]] QPushButton* stop_button() const noexcept;
        [[nodiscard]] QPushButton* clear_button() const noexcept;
        [[nodiscard]] QPushButton* close_button() const noexcept;
        [[nodiscard]] QPushButton* watch_writes_button() const noexcept;
        [[nodiscard]] QPushButton* watch_accesses_button() const noexcept;

        [[nodiscard]] int           recorded_row_count() const;
        [[nodiscard]] std::uint64_t recorded_instruction_at(int row) const;
        [[nodiscard]] std::uint64_t recorded_count_at(int row) const;
        [[nodiscard]] QString       recorded_text_at(int row) const;
        [[nodiscard]] int           instruction_row_count() const;

        void select_recorded_row(int row);
        void select_instruction_row(int row);

    signals:
        // "Follow in Memory Viewer" on the selected hit.
        void followRequested(std::uint64_t address);

    private:
        class HitsModel;
        class InstructionModel;

        // A watch the context-menu command asked for while the session was not up
        // yet; armed the moment the session runs.
        struct PendingWatch
        {
            std::uint64_t address {};
            debug::Kind   kind {};
            std::size_t   size {};
        };

        void                  arm_pending_watch();
        void                  build_layout();
        void                  refresh();
        void                  request_texts();
        void                  apply_texts(process::JobResult&& result, std::vector<int> rows);
        void                  follow_selected();
        void                  watch_selected(bool writes);
        void                  update_instruction_buttons();
        [[nodiscard]] QString display_address(std::uint64_t address) const;

        debug::Controller&     controller_;
        process::AccessWorker& worker_;
        DebugSessionGate       gate_;

        QLabel*               header_ {};
        widgets::StatusLabel* status_ {};
        QTableView*           recorded_table_ {};
        QTableView*           instruction_table_ {};
        widgets::StatusLabel* hint_ {};
        QPushButton*          follow_ {};
        QPushButton*          stop_ {};
        QPushButton*          clear_ {};
        QPushButton*          close_ {};
        QPushButton*          watch_writes_ {};
        QPushButton*          watch_accesses_ {};
        HitsModel*            hits_model_ {};
        InstructionModel*     instruction_model_ {};

        std::uint64_t instruction_ {};
        std::size_t   instruction_length_ {};

        std::optional<PendingWatch> pending_watch_;
    };

} // namespace slopkit::ui::dialogs
