#pragma once

#include <cstddef>

#include <QDialog>
#include <QString>

class QComboBox;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace slopkit::reclass
{
    class Project;
    struct EnumDescription;
}

namespace slopkit::ui::dialogs
{
    // Creates or edits one enum: its name, its storage width (1, 2, 4 or 8
    // bytes) and its named values. `target_name` names the enum to edit; an
    // empty name creates a new one.
    //
    // Editing is prompt-free on purpose: `apply` validates the fields and writes
    // the enum into the project, so a test drives the editor without a modal
    // box. The OK button runs that same `apply` and only closes when it succeeds.
    class EnumEditorDialog : public QDialog
    {
        Q_OBJECT

    public:
        EnumEditorDialog(reclass::Project& project, QString target_name, QWidget* parent = nullptr);

        [[nodiscard]] QLineEdit*    name_edit() const noexcept;
        [[nodiscard]] QComboBox*    size_combo() const noexcept;
        [[nodiscard]] QTableWidget* values_table() const noexcept;

        void add_value_row(const QString& name = {}, const QString& value = {}, const QString& comment = {});
        void remove_value_row();
        void clear_values();

        // Validates the fields and writes the enum; a refusal says why on
        // statusChanged and leaves the project alone.
        bool apply();

        [[nodiscard]] QString applied_name() const;

    signals:
        void statusChanged(const QString& message, bool error);

    private:
        void build_layout();
        void load();

        [[nodiscard]] bool collect(reclass::EnumDescription& description, QString& error) const;

        reclass::Project& project_;
        QString           target_name_;
        QString           applied_;
        bool              creating_ {false};

        QLineEdit*    name_ {};
        QComboBox*    size_ {};
        QTableWidget* values_ {};
        QPushButton*  add_value_ {};
        QPushButton*  remove_value_ {};
    };

    // Picks one of the project's enums, for an enum node or a bitfield node.
    // The first entry stands for "no enum"; `current` preselects the enum a node
    // already names.
    class EnumChooserDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit EnumChooserDialog(const reclass::Project& project, QString current = {}, QWidget* parent = nullptr);

        [[nodiscard]] QComboBox* combo() const noexcept;

        // The chosen enum's name, or an empty string for "no enum".
        [[nodiscard]] QString chosen_name() const;

    private:
        QComboBox* combo_ {};
    };
}
