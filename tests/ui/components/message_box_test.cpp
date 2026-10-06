#include <catch2/catch.hpp>

#include <functional>

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include "ui/components/message_box.hpp"
#include "ui/components/widgets.hpp"
#include "ui/theme.hpp"

// Defined in test_main.cpp; the single QApplication every widget suite shares.
QApplication& slopkit_test_application();

namespace
{
    // A QApplication may only exist once per process, so every case uses the one
    // instance the whole test binary shares.
    void ensure_application()
    {
        static_cast<void>(slopkit_test_application());
    }

    using slopkit::ui::widgets::MessageBox;
    using slopkit::ui::widgets::MessageBoxButton;
    using slopkit::ui::widgets::MessageBoxButtons;
    using slopkit::ui::widgets::MessageBoxIcon;
    using slopkit::ui::widgets::MessageBoxResult;

    // Runs `action` on the next visible message box from inside a nested event
    // loop; used to drive a modal box a test cannot reach directly.
    void on_box(const std::function<void(MessageBox&)>& action)
    {
        QTimer::singleShot(0,
                           [action]
                           {
                               for (QWidget* widget : QApplication::topLevelWidgets())
                               {
                                   auto* box = qobject_cast<MessageBox*>(widget);
                                   if (box != nullptr && box->isVisible())
                                   {
                                       action(*box);
                                       return;
                                   }
                               }
                           });
    }

    // Runs `setup` on a box, then lets `inspect` drive the shown box from inside
    // its nested event loop, and returns the recorded result.
    MessageBoxResult run_box(const std::function<void(MessageBox&)>& setup,
                             const std::function<void(MessageBox&)>& inspect)
    {
        MessageBox box;
        setup(box);
        on_box(inspect);
        box.exec();
        return box.result();
    }

    // Clicks the button with `label`, or dismisses the box when it is missing.
    void click(MessageBox& box, const QString& label)
    {
        for (QPushButton* button : box.findChildren<QPushButton*>())
        {
            if (button->text() == label)
            {
                button->click();
                return;
            }
        }
        box.reject();
    }

    // What a single-OK notice helper showed, captured while it was modal.
    struct NoticeCapture
    {
        int     buttons {0};
        QString title;
        QString text;
    };

    NoticeCapture
    show_notice(void (*notice)(QWidget*, const QString&, const QString&), const QString& title, const QString& message)
    {
        NoticeCapture capture;
        on_box(
            [&capture](MessageBox& box)
            {
                const auto buttons = box.findChildren<QPushButton*>();
                capture.buttons    = static_cast<int>(buttons.size());
                capture.title      = box.windowTitle();
                capture.text       = box.text();
                if (buttons.isEmpty())
                {
                    box.reject();
                }
                else
                {
                    buttons.constFirst()->click();
                }
            });
        notice(nullptr, title, message);
        return capture;
    }

    // The glyph pixmap a kind shows, or a null pixmap when it shows none.
    QPixmap glyph(MessageBoxIcon icon)
    {
        MessageBox box;
        box.set_icon(icon);
        for (QLabel* label : box.findChildren<QLabel*>())
        {
            const QPixmap pixmap = label->pixmap(Qt::ReturnByValue);
            if (!pixmap.isNull())
            {
                return pixmap;
            }
        }
        return {};
    }

    TEST_CASE("the message box reports the clicked standard button", "[ui]")
    {
        ensure_application();

        const auto outcome = [](MessageBoxButtons buttons, const QString& label)
        {
            return run_box(
                [buttons](MessageBox& box)
                {
                    box.set_text(QStringLiteral("message"));
                    box.set_buttons(buttons);
                },
                [label](MessageBox& box)
                {
                    click(box, label);
                });
        };

        CHECK(outcome(MessageBoxButton::ok, QStringLiteral("OK")) == MessageBoxResult::ok);
        CHECK(outcome(MessageBoxButton::yes | MessageBoxButton::no, QStringLiteral("Yes")) == MessageBoxResult::yes);
        CHECK(outcome(MessageBoxButton::yes | MessageBoxButton::no, QStringLiteral("No")) == MessageBoxResult::no);
        CHECK(outcome(MessageBoxButton::ok | MessageBoxButton::cancel, QStringLiteral("OK")) == MessageBoxResult::ok);
        CHECK(outcome(MessageBoxButton::ok | MessageBoxButton::cancel, QStringLiteral("Cancel"))
              == MessageBoxResult::cancel);
        CHECK(outcome(MessageBoxButton::close, QStringLiteral("Close")) == MessageBoxResult::close);
    }

    TEST_CASE("the message box marks the requested default button", "[ui]")
    {
        ensure_application();

        bool       has_default   = false;
        bool       default_is_no = false;
        const auto result        = run_box(
            [](MessageBox& box)
            {
                box.set_text(QStringLiteral("message"));
                box.set_buttons(MessageBoxButton::yes | MessageBoxButton::no);
                box.set_default_button(MessageBoxButton::no);
            },
            [&has_default, &default_is_no](MessageBox& box)
            {
                for (QPushButton* button : box.findChildren<QPushButton*>())
                {
                    if (button->isDefault())
                    {
                        has_default   = true;
                        default_is_no = button->text() == QStringLiteral("No");
                    }
                }
                box.reject();
            });

        CHECK(result == MessageBoxResult::none);
        CHECK(has_default);
        CHECK(default_is_no);
    }

    TEST_CASE("the message box keeps its configuration", "[ui]")
    {
        ensure_application();

        MessageBox box;
        box.set_icon(MessageBoxIcon::error);
        box.set_buttons(MessageBoxButton::ok | MessageBoxButton::cancel);
        box.set_title(QStringLiteral("Title"));
        box.set_text(QStringLiteral("Text"));
        box.set_informative_text(QStringLiteral("More"));
        box.set_default_button(MessageBoxButton::cancel);

        CHECK(box.icon() == MessageBoxIcon::error);
        CHECK(box.buttons() == (MessageBoxButton::ok | MessageBoxButton::cancel));
        CHECK(box.default_button() == MessageBoxButton::cancel);
        CHECK(box.windowTitle() == QStringLiteral("Title"));
        CHECK(box.text() == QStringLiteral("Text"));
        CHECK(box.informative_text() == QStringLiteral("More"));
        CHECK(box.result() == MessageBoxResult::none);
    }

    TEST_CASE("the message box hides and shows the informative line", "[ui]")
    {
        ensure_application();

        MessageBox box;
        box.set_text(QStringLiteral("message"));
        box.set_informative_text(QStringLiteral("Details"));

        QLabel* details = nullptr;
        for (QLabel* label : box.findChildren<QLabel*>())
        {
            if (label->text() == QStringLiteral("Details"))
            {
                details = label;
            }
        }
        REQUIRE(details != nullptr);
        CHECK_FALSE(details->isHidden());

        box.set_informative_text(QString());
        CHECK(box.informative_text().isEmpty());
        CHECK(details->isHidden());
    }

    TEST_CASE("dismissing the message box reports none", "[ui]")
    {
        ensure_application();

        const auto result = run_box(
            [](MessageBox& box)
            {
                box.set_text(QStringLiteral("message"));
                box.set_buttons(MessageBoxButton::ok | MessageBoxButton::cancel);
            },
            [](MessageBox& box)
            {
                box.reject();
            });
        CHECK(result == MessageBoxResult::none);
    }

    TEST_CASE("the message box with no buttons is dismissed as none", "[ui]")
    {
        ensure_application();

        bool       empty  = false;
        const auto result = run_box(
            [](MessageBox& box)
            {
                box.set_text(QStringLiteral("message"));
                box.set_buttons(MessageBoxButtons {});
            },
            [&empty](MessageBox& box)
            {
                empty = box.findChildren<QPushButton*>().isEmpty();
                box.reject();
            });

        CHECK(empty);
        CHECK(result == MessageBoxResult::none);
    }

    TEST_CASE("the message box maps each icon kind to a glyph", "[ui]")
    {
        ensure_application();

        CHECK(glyph(MessageBoxIcon::none).isNull());
        CHECK_FALSE(glyph(MessageBoxIcon::information).isNull());
        CHECK_FALSE(glyph(MessageBoxIcon::warning).isNull());
        CHECK_FALSE(glyph(MessageBoxIcon::error).isNull());
        CHECK_FALSE(glyph(MessageBoxIcon::success).isNull());

        // No dedicated question asset exists, so it falls back to the info glyph.
        CHECK(glyph(MessageBoxIcon::question).toImage() == glyph(MessageBoxIcon::information).toImage());
        CHECK(glyph(MessageBoxIcon::warning).toImage() != glyph(MessageBoxIcon::error).toImage());
    }

    TEST_CASE("the message box follows a theme switch", "[ui]")
    {
        ensure_application();

        slopkit::ui::apply_theme(slopkit::ui::dark_theme());
        MessageBox box;
        box.set_icon(MessageBoxIcon::warning);
        box.set_text(QStringLiteral("warning"));
        box.set_buttons(MessageBoxButton::ok | MessageBoxButton::cancel);
        box.show();
        QApplication::processEvents();

        slopkit::ui::apply_theme(slopkit::ui::light_theme());
        QApplication::processEvents();
        CHECK(box.isVisible());

        slopkit::ui::apply_theme(slopkit::ui::dark_theme());
        QApplication::processEvents();
        CHECK(box.isVisible());
        box.close();
    }

    TEST_CASE("confirm confirms only on Yes", "[ui]")
    {
        ensure_application();

        SECTION("confirm confirms only on Yes")
        {
            const auto answer = [](const QString& label)
            {
                on_box(
                    [label](MessageBox& box)
                    {
                        click(box, label);
                    });
                return slopkit::ui::widgets::confirm(nullptr, QStringLiteral("Confirm"), QStringLiteral("Sure?"));
            };

            CHECK(answer(QStringLiteral("Yes")));
            CHECK_FALSE(answer(QStringLiteral("No")));
        }

        SECTION("ok_cancel confirms only on OK")
        {
            const auto answer = [](const QString& label)
            {
                on_box(
                    [label](MessageBox& box)
                    {
                        click(box, label);
                    });
                return slopkit::ui::widgets::ok_cancel(nullptr, QStringLiteral("Proceed"), QStringLiteral("Go on?"));
            };

            CHECK(answer(QStringLiteral("OK")));
            CHECK_FALSE(answer(QStringLiteral("Cancel")));
        }

        SECTION("the notice helpers show a single OK button")
        {
            const auto info =
                show_notice(slopkit::ui::widgets::information, QStringLiteral("Info"), QStringLiteral("Info text."));
            CHECK(info.buttons == 1);
            CHECK(info.title == QStringLiteral("Info"));
            CHECK(info.text == QStringLiteral("Info text."));

            const auto warn =
                show_notice(slopkit::ui::widgets::warning, QStringLiteral("Warning"), QStringLiteral("Warn text."));
            CHECK(warn.buttons == 1);
            CHECK(warn.title == QStringLiteral("Warning"));
            CHECK(warn.text == QStringLiteral("Warn text."));

            const auto err =
                show_notice(slopkit::ui::widgets::error, QStringLiteral("Error"), QStringLiteral("Error text."));
            CHECK(err.buttons == 1);
            CHECK(err.title == QStringLiteral("Error"));
            CHECK(err.text == QStringLiteral("Error text."));
        }

        SECTION("the message_box helper returns the typed result")
        {
            on_box(
                [](MessageBox& box)
                {
                    click(box, QStringLiteral("Cancel"));
                });
            const auto result = slopkit::ui::widgets::message_box(nullptr,
                                                                  MessageBoxIcon::warning,
                                                                  QStringLiteral("Warning"),
                                                                  QStringLiteral("Careful."),
                                                                  MessageBoxButton::ok | MessageBoxButton::cancel,
                                                                  MessageBoxButton::ok);
            CHECK(result == MessageBoxResult::cancel);
        }
    }

    TEST_CASE("confirm uses a question box defaulting to No and treats dismissal as no", "[ui]")
    {
        ensure_application();

        MessageBoxIcon   seen_icon    = MessageBoxIcon::none;
        MessageBoxButton seen_default = MessageBoxButton::ok;
        on_box(
            [&seen_icon, &seen_default](MessageBox& box)
            {
                seen_icon    = box.icon();
                seen_default = box.default_button();
                box.reject();
            });

        CHECK_FALSE(slopkit::ui::widgets::confirm(nullptr, QStringLiteral("Confirm"), QStringLiteral("Sure?")));
        CHECK(seen_icon == MessageBoxIcon::question);
        CHECK(seen_default == MessageBoxButton::no);
    }

    TEST_CASE("the message box hides the application display name while it runs", "[ui]")
    {
        ensure_application();

        const QString original = QStringLiteral("slopkit");
        QGuiApplication::setApplicationDisplayName(original);

        bool title_without_suffix = false;

        MessageBox box;
        box.set_title(QStringLiteral("Delete Entry"));
        box.set_text(QStringLiteral("Delete this entry?"));
        box.set_buttons(MessageBoxButton::yes | MessageBoxButton::no);

        on_box(
            [&title_without_suffix](MessageBox& shown)
            {
                // The platform appends the display name to a title unless it is
                // left empty, so the modal run hides it.
                title_without_suffix = QGuiApplication::applicationDisplayName().isEmpty();
                shown.reject();
            });
        box.exec();

        const bool restored = QGuiApplication::applicationDisplayName() == original;
        CHECK(title_without_suffix);
        CHECK(restored);
    }

    TEST_CASE("the message box is wide enough for its title", "[ui]")
    {
        ensure_application();

        const QString title = QStringLiteral("A long message box title that must not be elided");

        MessageBox box;
        box.set_title(title);
        box.set_text(QStringLiteral("message"));

        QFont title_font = box.font();
        title_font.setBold(true);
        const int needed = QFontMetrics(title_font).horizontalAdvance(title);
        CHECK(box.minimumWidth() >= needed);

        box.show();
        QApplication::processEvents();
        CHECK(box.width() >= needed);
        box.close();

        MessageBox small;
        small.set_title(QStringLiteral("Hi"));
        CHECK(small.minimumWidth() < needed);
    }

    TEST_CASE("the message box keeps its natural width as a minimum", "[ui]")
    {
        ensure_application();

        MessageBox box;
        box.set_title(QStringLiteral("Delete Entry"));
        box.set_text(QStringLiteral("Delete this entry?"));
        box.set_buttons(MessageBoxButton::yes | MessageBoxButton::no);

        // A short window title must not leave the box with a sliver-thin
        // explicit minimum: the minimum follows the natural layout size, so a
        // window stack that sizes to it still shows a full dialog.
        CHECK(box.minimumWidth() >= box.sizeHint().width());

        box.show();
        QApplication::processEvents();
        CHECK(box.width() >= box.minimumWidth());
        box.close();
    }

} // namespace
