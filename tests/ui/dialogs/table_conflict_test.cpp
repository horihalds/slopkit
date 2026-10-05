#include <catch2/catch.hpp>

#include <functional>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include "ui/dialogs/table_conflict.hpp"

namespace
{
    // A QApplication may only exist once per process; Catch2 normally runs each
    // case in its own process, but the binary accepts several.
    void ensure_application()
    {
        if (QCoreApplication::instance() != nullptr)
        {
            return;
        }
        static int          argc      = 1;
        static char         program[] = "slopkit_tests";
        static char*        argv[]    = {program, nullptr};
        static QApplication instance(argc, argv);
    }

    using slopkit::ui::dialogs::TableConflictChoice;
    using slopkit::ui::dialogs::TableConflictDialog;
    using slopkit::ui::dialogs::TableConflictInfo;

    TableConflictInfo sample_info()
    {
        return TableConflictInfo {
            .incoming_path    = QStringLiteral("/tmp/incoming.skt"),
            .incoming_entries = 3,
            .current_path     = QStringLiteral("/tmp/current.skt"),
            .current_entries  = 5,
        };
    }

    // Runs `action` on the next visible conflict prompt from inside a nested
    // event loop; used to drive the modal dialog a test cannot reach directly.
    void on_dialog(const std::function<void(TableConflictDialog&)>& action)
    {
        QTimer::singleShot(0,
                           [action]
                           {
                               for (QWidget* widget : QApplication::topLevelWidgets())
                               {
                                   auto* dialog = qobject_cast<TableConflictDialog*>(widget);
                                   if (dialog != nullptr && dialog->isVisible())
                                   {
                                       action(*dialog);
                                       return;
                                   }
                               }
                           });
    }

    // Runs the prompt and clicks the button with `label`, or rejects it when the
    // button is missing.
    TableConflictChoice choose(const QString& label)
    {
        TableConflictDialog dialog {sample_info()};
        on_dialog(
            [label](TableConflictDialog& shown)
            {
                for (QPushButton* button : shown.findChildren<QPushButton*>())
                {
                    if (button->text() == label)
                    {
                        button->click();
                        return;
                    }
                }
                shown.reject();
            });
        dialog.exec();
        return dialog.choice();
    }

    // Concatenated label texts, so a test can look for a path or a count without
    // caring which line it landed on.
    QString all_label_text(const TableConflictDialog& dialog)
    {
        QString text;
        for (const QLabel* label : dialog.findChildren<QLabel*>())
        {
            text += label->text();
            text += QLatin1Char('\n');
        }
        return text;
    }

    TEST_CASE("the conflict prompt returns the clicked choice", "[ui]")
    {
        ensure_application();

        CHECK(choose(QStringLiteral("Cancel")) == TableConflictChoice::cancel);
        CHECK(choose(QStringLiteral("Overwrite")) == TableConflictChoice::overwrite);
        CHECK(choose(QStringLiteral("Merge")) == TableConflictChoice::merge);
    }

    TEST_CASE("the conflict prompt defaults to Merge and Escape cancels", "[ui]")
    {
        ensure_application();

        bool                merge_is_default = false;
        TableConflictDialog dialog {sample_info()};
        on_dialog(
            [&merge_is_default](TableConflictDialog& shown)
            {
                for (QPushButton* button : shown.findChildren<QPushButton*>())
                {
                    if (button->text() == QStringLiteral("Merge"))
                    {
                        merge_is_default = button->isDefault();
                    }
                }
                shown.reject(); // the window close button and Esc map to Cancel
            });
        dialog.exec();

        CHECK(dialog.choice() == TableConflictChoice::cancel);
        CHECK(merge_is_default);
    }

    TEST_CASE("the conflict prompt offers exactly Cancel, Overwrite and Merge", "[ui]")
    {
        ensure_application();

        TableConflictDialog dialog {sample_info()};
        const auto          buttons = dialog.findChildren<QPushButton*>();
        REQUIRE(buttons.size() == 3);

        QStringList labels;
        for (const QPushButton* button : buttons)
        {
            labels << button->text();
        }
        CHECK(labels.contains(QStringLiteral("Cancel")));
        CHECK(labels.contains(QStringLiteral("Overwrite")));
        CHECK(labels.contains(QStringLiteral("Merge")));
    }

    TEST_CASE("the conflict prompt names both files and both counts", "[ui]")
    {
        ensure_application();

        const TableConflictDialog dialog {sample_info()};
        const QString             text = all_label_text(dialog);

        CHECK(text.contains(QStringLiteral("/tmp/incoming.skt")));
        CHECK(text.contains(QStringLiteral("/tmp/current.skt")));
        CHECK(text.contains(QStringLiteral("3")));
        CHECK(text.contains(QStringLiteral("5")));
    }

    TEST_CASE("the conflict prompt marks a table that was never saved", "[ui]")
    {
        ensure_application();

        TableConflictInfo info = sample_info();
        info.current_path.clear();

        const TableConflictDialog dialog {info};
        CHECK(all_label_text(dialog).contains(QStringLiteral("never saved")));
    }

} // namespace
