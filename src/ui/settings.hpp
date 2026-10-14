#pragma once

#include <map>
#include <memory>

#include <QByteArray>
#include <QObject>
#include <QSettings>
#include <QString>

#include "ui/address_format.hpp"

namespace slopkit::ui
{
    enum class WindowId
    {
        main,
        log,
        breakpoints,
        access_watch,
        memory_viewer,
        reclass
    };

    struct Settings
    {
        bool                           dark_theme {true};
        AddressMode                    address_mode {AddressMode::module_relative};
        bool                           auto_load_last_table {false};
        QString                        last_table_path;
        QString                        last_directory;
        bool                           live_update_enabled {true};
        int                            live_update_interval_ms {250};
        std::map<WindowId, QByteArray> window_geometry;

        bool operator==(const Settings&) const = default;
    };

    class SettingsController : public QObject
    {
        Q_OBJECT

    public:
        explicit SettingsController(QObject* parent = nullptr);
        explicit SettingsController(const QString& file_path, QObject* parent = nullptr);

        [[nodiscard]] const Settings& values() const noexcept;

        void set_dark_theme(bool dark);
        void set_address_mode(AddressMode mode);
        void set_auto_load_last_table(bool enabled);
        void set_last_table_path(const QString& path);
        void set_last_directory(const QString& directory);
        void set_live_update_enabled(bool enabled);
        void set_live_update_interval_ms(int interval_ms);

        [[nodiscard]] QByteArray window_geometry(WindowId id) const;
        void                     set_window_geometry(WindowId id, const QByteArray& geometry);

    signals:
        void darkThemeChanged(bool dark);
        void addressModeChanged(slopkit::ui::AddressMode mode);
        void autoLoadLastTableChanged(bool enabled);
        void liveUpdateChanged(bool enabled);
        void liveUpdateIntervalChanged(int interval_ms);

    private:
        void                         load();
        void                         store();
        [[nodiscard]] static QString default_file_path();

        std::unique_ptr<QSettings> store_;
        Settings                   values_;
    };

}
