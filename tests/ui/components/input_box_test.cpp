#include <catch2/catch.hpp>

#include <functional>

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include "ui/components/input_box.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

// Defined in test_main.cpp; one QApplication is shared by every test file.
QApplication& slopkit_test_application();

namespace
{
    using slopkit::ui::widgets::InputBox;
    using slopkit::ui::widgets::InputBoxOptions;
    using slopkit::ui::widgets::StatusLabel;

    // Runs `action` on the next visible InputBox from inside its nested event
    // loop; used to drive the modal box a test cannot reach directly.
    void on_box(const std::function<void(InputBox&)>& action)
    {
        QTimer::singleShot(0,
                           [action]
                           {
                               for (QWidget* widget : QApplication::topLevelWidgets())
                               {
                                   auto* box = qobject_cast<InputBox*>(widget);
                                   if (box != nullptr && box->isVisible())
                                   {
                                       action(*box);
                                       return;
                                   }
                               }
                           });
    }

    // A validator that accepts a non-empty string of hex digits only.
    QString hex_validator(const QString& text)
    {
        if (text.isEmpty())
        {
            return QStringLiteral("a value is required");
        }
        for (const QChar character : text)
        {
            const char value = character.toLatin1();
            const bool is_hex =
                (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
            if (!is_hex)
            {
                return QStringLiteral("only hex digits are allowed");
            }
        }
        return {};
    }
} // namespace

TEST_CASE("an InputBox without a validator accepts any text", "[input_box]")
{
    slopkit_test_application();

    InputBoxOptions options;
    options.title   = QStringLiteral("Go To Address");
    options.label   = QStringLiteral("Address");
    options.initial = QStringLiteral("0x1234");

    InputBox box {options};
    box.show();

    CHECK(box.text() == QStringLiteral("0x1234"));
    CHECK(box.accept_button()->isEnabled());
    CHECK(box.accept_button()->isDefault());

    box.line_edit()->setText(QStringLiteral("0x40"));
    CHECK(box.text() == QStringLiteral("0x40"));

    box.accept();
    CHECK(box.result() == QDialog::Accepted);
}

TEST_CASE("get_text returns the accepted text and nothing on dismissal", "[input_box]")
{
    slopkit_test_application();

    InputBoxOptions options;
    options.initial = QStringLiteral("module+10");

    on_box(
        [](InputBox& box)
        {
            box.accept();
        });
    CHECK(slopkit::ui::widgets::get_text(options, nullptr) == std::optional<QString> {QStringLiteral("module+10")});

    on_box(
        [](InputBox& box)
        {
            box.reject();
        });
    CHECK_FALSE(slopkit::ui::widgets::get_text(options, nullptr).has_value());

    // The window close button is a rejection too.
    on_box(
        [](InputBox& box)
        {
            box.close();
        });
    CHECK_FALSE(slopkit::ui::widgets::get_text(options, nullptr).has_value());
}

TEST_CASE("an InputBox validator gates the OK button and shows the message", "[input_box]")
{
    slopkit_test_application();

    InputBoxOptions options;
    options.validate = hex_validator;
    options.initial  = QStringLiteral("not hex");

    InputBox box {options};
    box.show();

    auto* status = box.findChild<StatusLabel*>();
    REQUIRE(status != nullptr);

    // The initial text is invalid: OK is disabled and the message is shown.
    CHECK_FALSE(box.accept_button()->isEnabled());
    CHECK(status->text() == QStringLiteral("only hex digits are allowed"));

    // accept() is refused while invalid, so the dialog stays open.
    box.accept();
    CHECK(box.isVisible());
    CHECK(box.result() != QDialog::Accepted);

    // A valid edit re-enables OK and clears the message.
    box.line_edit()->setText(QStringLiteral("deadbeef"));
    CHECK(box.accept_button()->isEnabled());
    CHECK(status->text().isEmpty());

    box.accept();
    CHECK(box.result() == QDialog::Accepted);
}

TEST_CASE("Enter in a valid InputBox accepts the value", "[input_box]")
{
    slopkit_test_application();

    InputBoxOptions options;
    options.validate = hex_validator;
    options.initial  = QStringLiteral("deadbeef");

    InputBox box {options};
    box.show();
    box.line_edit()->setFocus();

    QKeyEvent event {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QApplication::sendEvent(box.line_edit(), &event);
    CHECK(box.result() == QDialog::Accepted);
}

TEST_CASE("the monospace option sets the mono font on the field", "[input_box]")
{
    slopkit_test_application();

    InputBoxOptions options;
    options.monospace = true;

    InputBox box {options};
    CHECK(box.line_edit()->font().family() == slopkit::ui::mono_font().family());
}
