#pragma once

#include <QObject>

#include "ui/settings.hpp"

class QWidget;

namespace slopkit::ui
{
    class WindowGeometryKeeper : public QObject
    {
        Q_OBJECT

    public:
        WindowGeometryKeeper(QWidget& window, SettingsController& settings, WindowId id, QObject* parent = nullptr);

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        QWidget&            window_;
        SettingsController& settings_;
        WindowId            id_;
    };

}
