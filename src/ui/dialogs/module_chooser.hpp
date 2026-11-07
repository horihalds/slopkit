#pragma once

#include <optional>
#include <vector>

#include <QDialog>

#include "process/types.hpp"

class QComboBox;

namespace slopkit::ui::dialogs
{
    class ModuleChooserDialog : public QDialog
    {
        Q_OBJECT

    public:
        ModuleChooserDialog(std::vector<process::ModuleInfo> modules, QWidget* parent = nullptr);

        [[nodiscard]] int count() const noexcept;
        void              set_current_index(int index);
        [[nodiscard]] int current_index() const noexcept;

        [[nodiscard]] std::optional<process::ModuleInfo> selected_module() const;

    private:
        std::vector<process::ModuleInfo> modules_;
        QComboBox*                       combo_ {};
    };

    [[nodiscard]] QString module_choice_text(const process::ModuleInfo& module);

}
