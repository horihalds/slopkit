#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "analysis/pointer_scan.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "script/symbols.hpp"
#include "ui/address_format.hpp"
#include "ui/models/pointer_scan_model.hpp"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableView;

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    class PointerScanDialog : public QDialog
    {
        Q_OBJECT

    public:
        PointerScanDialog(process::AccessWorker&   worker,
                          process::AttachedTarget& target,
                          script::SymbolTable&     symbols,
                          QWidget*                 parent = nullptr);

        void set_modules(std::vector<process::ModuleInfo> modules);
        void refresh();

        // Seeds the target field from another surface; the text stays editable.
        void set_target(std::uint64_t address);

        [[nodiscard]] QLineEdit*   target_edit() const noexcept;
        [[nodiscard]] QComboBox*   module_combo() const noexcept;
        [[nodiscard]] QSpinBox*    levels_spin() const noexcept;
        [[nodiscard]] QLineEdit*   offset_edit() const noexcept;
        [[nodiscard]] QCheckBox*   negative_check() const noexcept;
        [[nodiscard]] QPushButton* scan_button() const noexcept;
        [[nodiscard]] QTableView*  table() const noexcept;
        [[nodiscard]] QLabel*      empty_state() const noexcept;
        [[nodiscard]] QString      status_text() const;

        [[nodiscard]] int           row_count() const;
        [[nodiscard]] std::uint64_t base_at(int row) const;
        [[nodiscard]] QString       cell_text(int row, int column) const;

    signals:
        // The activated path's address expression, ready for the address table,
        // and the address it resolves to.
        void pathActivated(const QString& expression, quint64 target);

    private:
        void build_layout();
        void request_scan();
        void finish_resolve(process::JobId id, process::JobResult&& result);
        void
        start_scan(std::uint64_t target, const process::ModuleInfo& module, std::int64_t max_offset, bool negative);
        void finish_scan(process::JobId id, process::JobResult&& result);
        void update_controls();

        [[nodiscard]] const process::ModuleInfo* selected_module() const;

        process::AccessWorker&    worker_;
        process::AttachedTarget&  target_;
        script::SymbolTable&      symbols_;
        ui::ModuleSpans           spans_;
        models::PointerScanModel* model_ {};

        std::vector<process::ModuleInfo> modules_;
        std::optional<process::JobId>    resolve_job_;
        std::optional<process::JobId>    scan_job_;
        std::uint64_t                    result_target_ {};
        std::string                      result_module_;
        std::uint64_t                    result_module_base_ {};

        QLineEdit*            target_edit_ {};
        QComboBox*            module_combo_ {};
        QSpinBox*             levels_spin_ {};
        QLineEdit*            offset_edit_ {};
        QCheckBox*            negative_check_ {};
        QPushButton*          scan_ {};
        QTableView*           table_ {};
        QLabel*               empty_state_ {};
        widgets::StatusLabel* hint_ {};
        widgets::StatusLabel* status_ {};
    };
}
