#include <catch2/catch.hpp>

#include "support/sandbox_helpers.hpp"

TEST_CASE("the sandbox window shows every value with its name and type", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.show();
    QApplication::processEvents();

    CHECK(window.windowTitle() == QStringLiteral("slopkit sandbox"));
    CHECK(window.minimumWidth() > 0);
    CHECK(window.minimumHeight() > 0);

    struct Expected
    {
        const char* name;
        const char* type;
    };

    const Expected expected[] = {
        { "byte_value",          "Byte"},
        {"small_value",       "2 Bytes"},
        {     "health",       "4 Bytes"},
        {      "score",       "8 Bytes"},
        {      "speed",         "Float"},
        {  "precision",        "Double"},
        {     "banner",        "String"},
        {    "pattern", "Array of byte"},
        {      "drift",         "Float"},
        {"heap_marker", "Array of byte"},
    };

    for (const auto& item : expected)
    {
        const QString name = QString::fromUtf8(item.name);

        auto* label = window.findChild<QLabel*>(QStringLiteral("name_%1").arg(name));
        auto* type  = window.findChild<QLabel*>(QStringLiteral("type_%1").arg(name));
        auto* value = window.findChild<QLineEdit*>(QStringLiteral("value_%1").arg(name));

        REQUIRE(label != nullptr);
        REQUIRE(type != nullptr);
        REQUIRE(value != nullptr);
        CHECK(label->text() == name);
        CHECK(type->text() == QString::fromUtf8(item.type));
        CHECK_FALSE(value->text().isEmpty());
    }

    CHECK_FALSE(window.findChild<QLineEdit*>(QStringLiteral("value_health"))->isReadOnly());
    CHECK(window.findChild<QLineEdit*>(QStringLiteral("value_pattern"))->isReadOnly());
    CHECK(window.findChild<QLineEdit*>(QStringLiteral("value_heap_marker"))->isReadOnly());

    auto* address = window.findChild<QLabel*>(QStringLiteral("heap_marker_address"));
    REQUIRE(address != nullptr);
    CHECK(address->text().startsWith(QStringLiteral("0x")));
}

TEST_CASE("a write through the value store becomes visible after one refresh", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.set_paused(true);

    slopkit::sandbox::values().health = 42;
    window.refresh();

    auto* health = window.findChild<QLineEdit*>(QStringLiteral("value_health"));
    REQUIRE(health != nullptr);
    CHECK(health->text() == QStringLiteral("42"));
}

TEST_CASE("a refresh leaves a focused editor alone", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.set_paused(true);
    window.show();
    QApplication::processEvents();

    auto* health = window.findChild<QLineEdit*>(QStringLiteral("value_health"));
    REQUIRE(health != nullptr);
    health->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    REQUIRE(health->hasFocus());

    health->setText(QStringLiteral("123"));
    slopkit::sandbox::values().health = 7;
    window.refresh();

    CHECK(health->text() == QStringLiteral("123"));
}

TEST_CASE("an edited row writes back into the value store", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.set_paused(true);

    auto* health = window.findChild<QLineEdit*>(QStringLiteral("value_health"));
    REQUIRE(health != nullptr);

    health->setText(QStringLiteral("321"));
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(health, &enter);

    CHECK(slopkit::sandbox::values().health == 321);
}

TEST_CASE("pause stops the animation timer and resume restarts it", "[ui][sandbox]")
{
    application();

    slopkit::sandbox::SandboxWindow window;

    auto* animation = window.findChild<QTimer*>(QStringLiteral("animation_timer"));
    auto* refresh   = window.findChild<QTimer*>(QStringLiteral("refresh_timer"));
    REQUIRE(animation != nullptr);
    REQUIRE(refresh != nullptr);
    CHECK(animation->parent() == &window);
    CHECK(refresh->parent() == &window);
    CHECK(animation->isActive());
    CHECK(refresh->isActive());

    window.set_paused(true);
    CHECK(window.is_paused());
    CHECK_FALSE(animation->isActive());

    window.set_paused(false);
    CHECK_FALSE(window.is_paused());
    CHECK(animation->isActive());
}

TEST_CASE("the reset button restores every initial value", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();

    slopkit::sandbox::SandboxWindow window;
    window.set_paused(true);

    slopkit::sandbox::values().health = 5;
    slopkit::sandbox::values().score  = 9999;
    window.refresh();

    auto* reset = window.findChild<QPushButton*>(QStringLiteral("reset_button"));
    REQUIRE(reset != nullptr);
    reset->click();

    const Values expected {};
    CHECK(slopkit::sandbox::values().health == expected.health);
    CHECK(slopkit::sandbox::values().score == expected.score);

    auto* health = window.findChild<QLineEdit*>(QStringLiteral("value_health"));
    REQUIRE(health != nullptr);
    CHECK(health->text() == QString::number(static_cast<int>(expected.health)));
}

TEST_CASE("destroying the window cleans up without a stray animation tick", "[ui][sandbox]")
{
    application();
    slopkit::sandbox::reset();
    const std::int32_t before = slopkit::sandbox::values().health;

    {
        slopkit::sandbox::SandboxWindow window;
        QApplication::processEvents();
    }

    CHECK(slopkit::sandbox::values().health == before);
}
