#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "structs/definition.hpp"
#include "structs/types.hpp"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace slopkit::ui::widgets
{
    class StatusLabel;
}

namespace slopkit::ui::dialogs
{
    struct FieldDraft
    {
        QString            name;
        std::uint64_t      offset {};
        structs::FieldType type;
    };

    class AddFieldDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit AddFieldDialog(QWidget* parent = nullptr);

        // Somewhere to append the new field: the name and offset are seeded from
        // the last field so a fresh row lands after it.
        void begin_add(const structs::Structs& definitions, const QString& default_name, std::uint64_t default_offset);

        // Reopens an existing field for editing.
        void begin_edit(const structs::Structs& definitions, const structs::StructNode& node);

        [[nodiscard]] std::optional<FieldDraft> draft() const;

        void accept() override;

        [[nodiscard]] QLineEdit* name_edit() const noexcept;
        [[nodiscard]] QLineEdit* offset_edit() const noexcept;
        [[nodiscard]] QComboBox* kind_combo() const noexcept;
        [[nodiscard]] QComboBox* width_combo() const noexcept;
        [[nodiscard]] QCheckBox* signed_check() const noexcept;
        [[nodiscard]] QSpinBox*  count_spin() const noexcept;
        [[nodiscard]] QCheckBox* wide_check() const noexcept;
        [[nodiscard]] QComboBox* struct_combo() const noexcept;

    private:
        void populate_structs(const structs::Structs& definitions, const QString& selected);
        void apply_kind();
        void validate();

        QLineEdit*            name_edit_ {};
        QLineEdit*            offset_edit_ {};
        QComboBox*            kind_combo_ {};
        QComboBox*            width_combo_ {};
        QCheckBox*            signed_check_ {};
        QSpinBox*             count_spin_ {};
        QCheckBox*            wide_check_ {};
        QComboBox*            struct_combo_ {};
        widgets::StatusLabel* status_ {};
    };

}
