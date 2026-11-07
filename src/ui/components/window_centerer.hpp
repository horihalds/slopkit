#pragma once

#include <QObject>

class QWidget;

namespace slopkit::ui
{
    class WindowCenterer : public QObject
    {
        Q_OBJECT

    public:
        explicit WindowCenterer(QWidget& window, QObject* parent = nullptr);

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        void center_on_parent();

        QWidget& window_;
    };

}
