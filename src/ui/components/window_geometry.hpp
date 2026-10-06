#pragma once

#include <QObject>

#include "ui/settings.hpp"

class QWidget;

namespace slopkit::ui
{
    // Remembers one top-level window's geometry: puts the saved blob back when the
    // keeper is built and writes a fresh one whenever the window is hidden or
    // closed, so the owner only has to keep the keeper alive. A keeper parented to
    // the window it tracks needs no owning member at all.
    class WindowGeometryKeeper : public QObject
    {
        Q_OBJECT

    public:
        WindowGeometryKeeper(QWidget& window, SettingsController& settings, WindowId id, QObject* parent = nullptr);

    protected:
        // Saves on Hide and Close; a minimise is not a hide, so a minimised window
        // keeps the geometry it was restored with.
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        QWidget&            window_;
        SettingsController& settings_;
        WindowId            id_;
    };

} // namespace slopkit::ui
