#include "ui/components/row_menu.hpp"

#include <utility>

#include <QAction>
#include <QCoreApplication>
#include <QMenu>
#include <QObject>

namespace slopkit::ui::widgets
{

    namespace
    {
        // One translation context for the row-menu text, shared by the panels
        // and the listing.
        constexpr const char* kContext = "row_menu";

        QString watch_text(WatchCommand command)
        {
            switch (command)
            {
            case WatchCommand::writes:
                return QCoreApplication::translate(kContext, "Find out what writes this address");
            case WatchCommand::accesses:
                return QCoreApplication::translate(kContext, "Find out what accesses this address");
            }
            std::unreachable();
        }

        QString watch_description(WatchCommand command)
        {
            switch (command)
            {
            case WatchCommand::writes:
                return QCoreApplication::translate(
                    kContext,
                    "Record every instruction that writes this address. The debugger is attached first (after a "
                    "confirmation) when no session is running.");
            case WatchCommand::accesses:
                return QCoreApplication::translate(
                    kContext,
                    "Record every instruction that reads or writes this address. The debugger is attached first "
                    "(after a confirmation) when no session is running.");
            }
            std::unreachable();
        }
    } // namespace

    void show_explanations(QMenu& menu)
    {
        menu.setToolTipsVisible(true);
    }

    QAction* described_action(QMenu& menu, const QString& text, const QString& description)
    {
        QAction* action = menu.addAction(text);
        action->setToolTip(description);
        return action;
    }

    QAction* disabled_action(QMenu& menu, const QString& text, const QString& reason)
    {
        QAction* action = menu.addAction(text);
        action->setEnabled(false);
        action->setToolTip(reason);
        return action;
    }

    void add_watch_commands(QMenu& menu, bool target_attached, const std::function<void(WatchCommand)>& arm)
    {
        menu.addSeparator();
        for (const WatchCommand command : {WatchCommand::writes, WatchCommand::accesses})
        {
            if (!target_attached)
            {
                (void)disabled_action(
                    menu, watch_text(command), QCoreApplication::translate(kContext, "Attach to a target first."));
                continue;
            }
            QAction* action = described_action(menu, watch_text(command), watch_description(command));
            QObject::connect(action,
                             &QAction::triggered,
                             &menu,
                             [arm, command]
                             {
                                 arm(command);
                             });
        }
    }

} // namespace slopkit::ui::widgets
