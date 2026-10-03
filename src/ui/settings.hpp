#pragma once

#include <memory>

#include <QObject>
#include <QSettings>
#include <QString>

#include "ui/address_format.hpp"

namespace slopkit::ui
{

    // The persisted application settings: the appearance and the address
    // display mode, with the defaults applied when the file is missing.
    struct Settings
    {
        bool        dark_theme {true};
        AddressMode address_mode {AddressMode::module_relative};

        bool operator==(const Settings&) const = default;
    };

    // Owns the settings store: loads the values once, writes every change and
    // publishes typed signals, so no other component reads the file directly.
    class SettingsController : public QObject
    {
        Q_OBJECT

    public:
        // Uses the per-user INI file under the XDG config directory.
        explicit SettingsController(QObject* parent = nullptr);
        // Uses `file_path`; tests point this at a scratch file.
        explicit SettingsController(const QString& file_path, QObject* parent = nullptr);

        [[nodiscard]] const Settings& values() const noexcept;

        void set_dark_theme(bool dark);
        void set_address_mode(AddressMode mode);

    signals:
        void darkThemeChanged(bool dark);
        void addressModeChanged(slopkit::ui::AddressMode mode);

    private:
        void                         load();
        void                         store();
        [[nodiscard]] static QString default_file_path();

        std::unique_ptr<QSettings> store_;
        Settings                   values_;
    };

} // namespace slopkit::ui
