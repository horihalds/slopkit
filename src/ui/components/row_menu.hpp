#pragma once

#include <functional>

#include <QString>

class QAction;
class QMenu;

namespace slopkit::ui::widgets
{

    // The two hardware-watch commands the address list and the found list share.
    enum class WatchCommand
    {
        writes,
        accesses,
    };

    // Qt shows an action's tooltip only when the menu asks for it, and every row
    // menu here explains a disabled entry through it (docs/UI_DESIGN.md).
    void show_explanations(QMenu& menu);

    // Enabled entry whose tooltip describes what it does.
    [[nodiscard]] QAction* described_action(QMenu& menu, const QString& text, const QString& description);

    // Disabled entry whose tooltip gives the reason it is unavailable.
    [[nodiscard]] QAction* disabled_action(QMenu& menu, const QString& text, const QString& reason);

    // The `Find out what writes/accesses this address` pair, after a separator:
    // shared wording, shared reason, added disabled as a pair while no target is
    // attached. `arm` receives the command the user picked.
    void add_watch_commands(QMenu& menu, bool target_attached, const std::function<void(WatchCommand)>& arm);

} // namespace slopkit::ui::widgets
