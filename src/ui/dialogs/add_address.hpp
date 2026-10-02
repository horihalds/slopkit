#pragma once

#include "table/address_table.hpp"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The Add Address dialog: description, address, value type, the dynamic
    // types' size and the hex toggle. It appends a table::AddressEntry.
    class AddAddressDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit AddAddressDialog(table::AddressTable& table, QWidget* parent = nullptr);

    private:
        void commit();
        void update_size_row();

        table::AddressTable& table_;

        QLineEdit*            description_edit_ {};
        QLineEdit*            address_edit_ {};
        QComboBox*            type_combo_ {};
        QLineEdit*            size_edit_ {};
        QWidget*              size_row_ {};
        QCheckBox*            hex_check_ {};
        widgets::StatusLabel* status_ {};
    };

} // namespace slopkit::ui::dialogs
