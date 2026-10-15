#pragma once

#include <QWidget>

class QPushButton;

namespace slopkit::ui::panels
{
    // The companion-window buttons sit in a bar of their own across the whole
    // window, rather than in a row inside the found list, so all six labels fit
    // whole at the default window size.
    class CompanionBar : public QWidget
    {
        Q_OBJECT

    public:
        explicit CompanionBar(QWidget* parent = nullptr);

        void set_target_attached(bool attached);

        [[nodiscard]] QWidget* tab_order_first() const noexcept;
        [[nodiscard]] QWidget* tab_order_last() const noexcept;

    signals:
        void memoryViewRequested();
        void logRequested();
        void settingsRequested();
        void breakpointsRequested();
        void accessWatchWindowRequested();
        void reclassRequested();

    private:
        QPushButton* memory_view_button_ {};
        QPushButton* reclass_button_ {};
        QPushButton* breakpoints_button_ {};
        QPushButton* access_watch_button_ {};
        QPushButton* log_button_ {};
        QPushButton* settings_button_ {};
    };

}
