#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "analysis/code_targets.hpp"
#include "debug/controller.hpp"
#include "process/types.hpp"
#include "ui/debug_session.hpp"
#include "ui/models/code_filter_model.hpp"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableView;

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    class CodeFilterDialog : public QDialog
    {
        Q_OBJECT

    public:
        CodeFilterDialog(debug::Controller& controller, process::AttachedTarget& target);

        void set_modules(std::vector<process::ModuleInfo> modules);

        // Applies a finished sweep: the filter starts on its targets and the
        // status line keeps its funnel.
        void apply_scan(analysis::CodeTargetScanResult scan, std::string module, std::uint64_t base);
        void report_error(const QString& text);

        void refresh();

        [[nodiscard]] DebugSessionGate& debug_gate() noexcept;

        [[nodiscard]] QString status_text() const;
        [[nodiscard]] QString hint_text() const;

        [[nodiscard]] QComboBox*   module_combo() const noexcept;
        [[nodiscard]] QCheckBox*   call_checkbox() const noexcept;
        [[nodiscard]] QCheckBox*   jump_checkbox() const noexcept;
        [[nodiscard]] QCheckBox*   conditional_checkbox() const noexcept;
        [[nodiscard]] QPushButton* collect_button() const noexcept;
        [[nodiscard]] QPushButton* keep_executed_button() const noexcept;
        [[nodiscard]] QPushButton* keep_not_executed_button() const noexcept;
        [[nodiscard]] QPushButton* clear_button() const noexcept;
        [[nodiscard]] QTableView*  table() const noexcept;

        [[nodiscard]] int           row_count() const;
        [[nodiscard]] std::uint64_t target_at(int row) const;
        [[nodiscard]] QString       cell_text(int row, int column) const;

    signals:
        void collectRequested(QString                   module,
                              std::uint64_t             base,
                              std::uint64_t             end,
                              std::uint32_t             pointer_size,
                              analysis::CodeTargetKinds kinds);

        // The activated candidate's target, so the window's owner can open it.
        void targetActivated(std::uint64_t address);

    private:
        void build_layout();
        void update_controls();

        void request_collect();
        void rebuild_rows();

        [[nodiscard]] const process::ModuleInfo* selected_module() const;

        debug::Controller&       controller_;
        DebugSessionGate         gate_;
        models::CodeFilterModel* model_ {};

        QComboBox*            module_combo_ {};
        QCheckBox*            call_ {};
        QCheckBox*            jump_ {};
        QCheckBox*            conditional_ {};
        QPushButton*          collect_ {};
        QPushButton*          keep_executed_ {};
        QPushButton*          keep_not_executed_ {};
        QPushButton*          clear_ {};
        QTableView*           table_ {};
        QLabel*               header_ {};
        widgets::StatusLabel* status_ {};
        widgets::StatusLabel* hint_ {};

        std::vector<process::ModuleInfo>                            modules_;
        std::string                                                 module_name_;
        std::uint64_t                                               module_base_ {};
        QString                                                     summary_;
        std::unordered_map<std::uint64_t, analysis::CodeTargetKind> kind_by_target_;
    };
}
