#include "ui/settings.hpp"

#include <optional>

#include <QDir>
#include <QStandardPaths>
#include <QVariant>

namespace slopkit::ui
{
    namespace
    {
        constexpr auto kDarkThemeKey   = "appearance/dark_theme";
        constexpr auto kDisplayModeKey = "addresses/display_mode";
        constexpr auto kModuleRelative = "module_relative";
        constexpr auto kAbsolute       = "absolute";

        std::optional<bool> parse_bool(const QVariant& value)
        {
            if (value.metaType().id() == QMetaType::Bool)
            {
                return value.toBool();
            }
            if (value.metaType().id() == QMetaType::QString)
            {
                const QString text = value.toString().trimmed().toLower();
                if (text == QStringLiteral("true"))
                {
                    return true;
                }
                if (text == QStringLiteral("false"))
                {
                    return false;
                }
            }
            return std::nullopt;
        }

        std::optional<AddressMode> parse_address_mode(const QVariant& value)
        {
            const QString text = value.toString();
            if (text == QString::fromLatin1(kModuleRelative))
            {
                return AddressMode::module_relative;
            }
            if (text == QString::fromLatin1(kAbsolute))
            {
                return AddressMode::absolute;
            }
            return std::nullopt;
        }
    } // namespace

    SettingsController::SettingsController(QObject* parent) : SettingsController(default_file_path(), parent) {}

    SettingsController::SettingsController(const QString& file_path, QObject* parent)
        : QObject(parent), store_(std::make_unique<QSettings>(file_path, QSettings::IniFormat))
    {
        load();
    }

    const Settings& SettingsController::values() const noexcept
    {
        return values_;
    }

    void SettingsController::set_dark_theme(bool dark)
    {
        if (values_.dark_theme == dark)
        {
            return;
        }
        values_.dark_theme = dark;
        store();
        emit darkThemeChanged(dark);
    }

    void SettingsController::set_address_mode(AddressMode mode)
    {
        if (values_.address_mode == mode)
        {
            return;
        }
        values_.address_mode = mode;
        store();
        emit addressModeChanged(mode);
    }

    void SettingsController::load()
    {
        if (const auto dark = parse_bool(store_->value(QString::fromLatin1(kDarkThemeKey))); dark.has_value())
        {
            values_.dark_theme = *dark;
        }

        if (const auto mode = parse_address_mode(store_->value(QString::fromLatin1(kDisplayModeKey))); mode.has_value())
        {
            values_.address_mode = *mode;
        }
    }

    void SettingsController::store()
    {
        // The value is written immediately (and sync'd), so a crash right after
        // a change cannot lose it; a read-only file fails silently.
        store_->setValue(QString::fromLatin1(kDarkThemeKey), values_.dark_theme);
        store_->setValue(
            QString::fromLatin1(kDisplayModeKey),
            QString::fromLatin1(values_.address_mode == AddressMode::absolute ? kAbsolute : kModuleRelative));
        store_->sync();
    }

    QString SettingsController::default_file_path()
    {
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        if (directory.isEmpty())
        {
            return QStringLiteral("slopkit.ini");
        }
        return QDir(directory).filePath(QStringLiteral("slopkit/slopkit.ini"));
    }

} // namespace slopkit::ui
