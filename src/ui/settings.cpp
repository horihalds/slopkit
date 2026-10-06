#include "ui/settings.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

#include <QDir>
#include <QStandardPaths>
#include <QVariant>

#include "core/log.hpp"
#include "core/log_categories.hpp"

namespace slopkit::ui
{
    namespace
    {
        constexpr auto kDarkThemeKey     = "appearance/dark_theme";
        constexpr auto kDisplayModeKey   = "addresses/display_mode";
        constexpr auto kAutoLoadKey      = "tables/auto_load_last";
        constexpr auto kLastTableKey     = "tables/last_path";
        constexpr auto kLastDirectoryKey = "files/last_directory";
        constexpr auto kLiveEnabledKey   = "liveUpdateEnabled";
        constexpr auto kLiveIntervalKey  = "liveUpdateIntervalMs";
        constexpr auto kModuleRelative   = "module_relative";
        constexpr auto kAbsolute         = "absolute";

        // The INI key of each rememberable window's geometry; the Memory Viewer
        // keeps its historical key so an installed config still finds it.
        constexpr std::array<std::pair<WindowId, std::string_view>, 5> kWindowKeys {
            {
             {WindowId::main, "windows/main_geometry"},
             {WindowId::log, "windows/log_geometry"},
             {WindowId::breakpoints, "windows/breakpoints_geometry"},
             {WindowId::access_watch, "windows/access_watch_geometry"},
             {WindowId::memory_viewer, "windows/memory_view_geometry"},
             }
        };

        constexpr int kMinLiveIntervalMs     = 50;
        constexpr int kMaxLiveIntervalMs     = 5000;
        constexpr int kDefaultLiveIntervalMs = 250;

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

        std::optional<int> parse_int(const QVariant& value)
        {
            bool      ok     = false;
            const int parsed = value.toInt(&ok);
            return ok ? std::optional<int> {parsed} : std::nullopt;
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

        std::optional<QByteArray> parse_geometry(const QVariant& value)
        {
            if (value.metaType().id() != QMetaType::QByteArray)
            {
                return std::nullopt;
            }
            return value.toByteArray();
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

    void SettingsController::set_auto_load_last_table(bool enabled)
    {
        if (values_.auto_load_last_table == enabled)
        {
            return;
        }
        values_.auto_load_last_table = enabled;
        store();
        emit autoLoadLastTableChanged(enabled);
    }

    void SettingsController::set_last_table_path(const QString& path)
    {
        if (values_.last_table_path == path)
        {
            return;
        }
        values_.last_table_path = path;
        store();
    }

    void SettingsController::set_last_directory(const QString& directory)
    {
        if (values_.last_directory == directory)
        {
            return;
        }
        values_.last_directory = directory;
        store();
    }

    void SettingsController::set_live_update_enabled(bool enabled)
    {
        if (values_.live_update_enabled == enabled)
        {
            return;
        }
        values_.live_update_enabled = enabled;
        store();
        emit liveUpdateChanged(enabled);
    }

    void SettingsController::set_live_update_interval_ms(int interval_ms)
    {
        const int clamped = std::clamp(interval_ms, kMinLiveIntervalMs, kMaxLiveIntervalMs);
        if (values_.live_update_interval_ms == clamped)
        {
            return;
        }
        values_.live_update_interval_ms = clamped;
        store();
        emit liveUpdateIntervalChanged(clamped);
    }

    QByteArray SettingsController::window_geometry(WindowId id) const
    {
        const auto it = values_.window_geometry.find(id);
        return it == values_.window_geometry.end() ? QByteArray {} : it->second;
    }

    void SettingsController::set_window_geometry(WindowId id, const QByteArray& geometry)
    {
        const auto it                = values_.window_geometry.find(id);
        const auto existing_geometry = it == values_.window_geometry.end() ? QByteArray {} : it->second;
        if (existing_geometry == geometry)
        {
            return;
        }
        values_.window_geometry[id] = geometry;
        store();
    }

    void SettingsController::load()
    {
        const auto file_path = store_->fileName().toStdString();
        if (store_->status() != QSettings::NoError)
        {
            log::warning(log::category::app, std::format("settings file {} could not be read", file_path));
        }
        else
        {
            log::info(log::category::app, std::format("settings file {}", file_path));
        }

        const auto dark_key = QString::fromLatin1(kDarkThemeKey);
        if (const auto dark = parse_bool(store_->value(dark_key)); dark.has_value())
        {
            values_.dark_theme = *dark;
        }
        else if (store_->contains(dark_key))
        {
            log::warning(log::category::app,
                         std::format("settings file {}: malformed {} value", file_path, kDarkThemeKey));
        }

        const auto mode_key = QString::fromLatin1(kDisplayModeKey);
        if (const auto mode = parse_address_mode(store_->value(mode_key)); mode.has_value())
        {
            values_.address_mode = *mode;
        }
        else if (store_->contains(mode_key))
        {
            log::warning(log::category::app,
                         std::format("settings file {}: malformed {} value", file_path, kDisplayModeKey));
        }

        const auto auto_load_key = QString::fromLatin1(kAutoLoadKey);
        if (const auto auto_load = parse_bool(store_->value(auto_load_key)); auto_load.has_value())
        {
            values_.auto_load_last_table = *auto_load;
        }
        else if (store_->contains(auto_load_key))
        {
            log::warning(log::category::app,
                         std::format("settings file {}: malformed {} value", file_path, kAutoLoadKey));
        }

        const auto live_enabled_key = QString::fromLatin1(kLiveEnabledKey);
        if (const auto enabled = parse_bool(store_->value(live_enabled_key)); enabled.has_value())
        {
            values_.live_update_enabled = *enabled;
        }
        else if (store_->contains(live_enabled_key))
        {
            log::warning(log::category::app,
                         std::format("settings file {}: malformed {} value", file_path, kLiveEnabledKey));
        }

        const auto live_interval_key = QString::fromLatin1(kLiveIntervalKey);
        if (const auto interval = parse_int(store_->value(live_interval_key)); interval.has_value())
        {
            values_.live_update_interval_ms = std::clamp(*interval, kMinLiveIntervalMs, kMaxLiveIntervalMs);
            if (values_.live_update_interval_ms != *interval)
            {
                log::warning(log::category::app,
                             std::format("settings file {}: {} out of range, clamped to {}",
                                         file_path,
                                         kLiveIntervalKey,
                                         values_.live_update_interval_ms));
            }
        }
        else if (store_->contains(live_interval_key))
        {
            values_.live_update_interval_ms = kDefaultLiveIntervalMs;
            log::warning(log::category::app,
                         std::format("settings file {}: malformed {} value", file_path, kLiveIntervalKey));
        }

        for (const auto& [id, key] : kWindowKeys)
        {
            const auto geometry_key = QString::fromLatin1(key);
            if (const auto geometry = parse_geometry(store_->value(geometry_key)); geometry.has_value())
            {
                values_.window_geometry[id] = *geometry;
            }
            else if (store_->contains(geometry_key))
            {
                log::warning(log::category::app, std::format("settings file {}: malformed {} value", file_path, key));
            }
        }

        values_.last_table_path = store_->value(QString::fromLatin1(kLastTableKey)).toString();
        values_.last_directory  = store_->value(QString::fromLatin1(kLastDirectoryKey)).toString();

        log::debug(log::category::app,
                   std::format("settings loaded: dark_theme={}, address_mode={}, auto_load_last={}, "
                               "last_table={}, last_directory={}, live_update={}, live_interval_ms={}, "
                               "saved_window_geometries={}",
                               values_.dark_theme,
                               values_.address_mode == AddressMode::absolute ? "absolute" : "module_relative",
                               values_.auto_load_last_table,
                               values_.last_table_path.toStdString(),
                               values_.last_directory.toStdString(),
                               values_.live_update_enabled,
                               values_.live_update_interval_ms,
                               values_.window_geometry.size()));
    }

    void SettingsController::store()
    {
        // The value is written immediately (and sync'd), so a crash right after
        // a change cannot lose it; a read-only file fails silently.
        store_->setValue(QString::fromLatin1(kDarkThemeKey), values_.dark_theme);
        store_->setValue(
            QString::fromLatin1(kDisplayModeKey),
            QString::fromLatin1(values_.address_mode == AddressMode::absolute ? kAbsolute : kModuleRelative));
        store_->setValue(QString::fromLatin1(kAutoLoadKey), values_.auto_load_last_table);
        store_->setValue(QString::fromLatin1(kLastTableKey), values_.last_table_path);
        store_->setValue(QString::fromLatin1(kLastDirectoryKey), values_.last_directory);
        store_->setValue(QString::fromLatin1(kLiveEnabledKey), values_.live_update_enabled);
        store_->setValue(QString::fromLatin1(kLiveIntervalKey), values_.live_update_interval_ms);
        for (const auto& [id, key] : kWindowKeys)
        {
            const auto it = values_.window_geometry.find(id);
            store_->setValue(QString::fromLatin1(key),
                             it == values_.window_geometry.end() ? QVariant {} : QVariant {it->second});
        }
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
