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
    // One rememberable top-level window; each keeps its own saved geometry.
    enum class WindowId
    {
        main,
        log,
        breakpoints,
        access_watch,
        memory_viewer
    };

    // The persisted application settings: the appearance, the address display
    // mode and the table/file-dialog memory, with the defaults applied when the
    // file is missing.
    struct Settings
    {
        bool                           dark_theme {true};
        AddressMode                    address_mode {AddressMode::module_relative};
        // Whether the last table is loaded again at the next start.
        bool                           auto_load_last_table {false};
        // The table slopkit last loaded or saved.
        QString                        last_table_path;
        // The directory the file dialogs last used.
        QString                        last_directory;
        // Whether the Value columns follow the attached target's live memory.
        bool                           live_update_enabled {true};
        // The live refresh cadence in milliseconds, clamped to [50, 5000].
        int                            live_update_interval_ms {250};
        // The saved window geometries (QWidget::saveGeometry), one per WindowId;
        // a missing entry means that window has never been hidden.
        std::map<WindowId, QByteArray> window_geometry;

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

} // namespace slopkit::ui
