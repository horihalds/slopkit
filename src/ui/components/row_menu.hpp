#pragma once

#include <functional>

#include <QString>

class QAction;
class QMenu;

namespace slopkit::ui::widgets
{
    enum class WatchCommand
    {
        writes,
        accesses,
    };

    void show_explanations(QMenu& menu);

    [[nodiscard]] QMenu* explained_submenu(QMenu& parent, const QString& title);

    [[nodiscard]] QAction* described_action(QMenu& menu, const QString& text, const QString& description);

    [[nodiscard]] QAction* disabled_action(QMenu& menu, const QString& text, const QString& reason);

    void show_shortcut_hint(QAction& item, const QAction& source);

    void add_watch_commands(QMenu& menu, bool target_attached, const std::function<void(WatchCommand)>& arm);

}
