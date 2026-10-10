#pragma once

#include <cstdint>

#include "plugin/plugin_host.hpp"
#include "scan/engine.hpp"
#include "ui/address_format.hpp"
#include "ui/settings.hpp"

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QRadioButton;
class QSpinBox;
class QStackedWidget;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // The Settings dialog: a category list on the left and the selected category
    // on the right. It is a view over the shared SettingsController, which owns
    // persistence and publishes the changes.
    class SettingsDialog : public QDialog
    {
        Q_OBJECT

    public:
        SettingsDialog(plugin::PluginHost& host,
                       scan::ScanEngine&   engine,
                       SettingsController& settings,
                       QWidget*            parent = nullptr);

        // Selects the About category; used by Help > About slopkit.
        void select_about();

        // Keeps the Appearance selector in step with the active theme.
        void set_dark_theme(bool dark);

        // Keeps the Addresses selector in step without re-emitting.
        void set_address_mode(ui::AddressMode mode);

        // Keeps the Tables switch in step without re-emitting.
        void set_auto_load_last_table(bool enabled);

        // Keeps the live-update switch and interval in step without re-emitting.
        void set_live_update_enabled(bool enabled);
        void set_live_update_interval_ms(int interval_ms);

    signals:
        // The new fast-scan alignment default, forwarded to the scanner panel.
        void alignmentChanged(quint64 alignment);

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        [[nodiscard]] QWidget* build_appearance_page();
        [[nodiscard]] QWidget* build_addresses_page();
        [[nodiscard]] QWidget* build_live_update_page();
        [[nodiscard]] QWidget* build_tables_page();
        [[nodiscard]] QWidget* build_scanning_page();
        [[nodiscard]] QWidget* build_plugins_page();
        [[nodiscard]] QWidget* build_about_page();

        void apply_scanning();
        void refresh_plugins();
        void refresh_last_table();

        plugin::PluginHost& host_;
        scan::ScanEngine&   engine_;
        SettingsController& settings_;

        QListWidget*          categories_ {};
        QStackedWidget*       pages_ {};
        QRadioButton*         dark_button_ {};
        QRadioButton*         light_button_ {};
        QRadioButton*         module_relative_button_ {};
        QRadioButton*         absolute_button_ {};
        QCheckBox*            auto_load_check_ {};
        QCheckBox*            live_update_check_ {};
        QSpinBox*             live_update_interval_ {};
        QLabel*               last_table_label_ {};
        QLineEdit*            alignment_edit_ {};
        QPlainTextEdit*       plugins_view_ {};
        QLabel*               about_title_ {};
        QLabel*               about_plugins_ {};
        widgets::StatusLabel* status_ {};
    };

} // namespace slopkit::ui::dialogs
