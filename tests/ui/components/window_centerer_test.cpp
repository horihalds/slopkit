#include <catch2/catch.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QWidget>

#include "ui/components/window_centerer.hpp"

// Defined in test_main.cpp; the single QApplication every widget suite shares.
QApplication& slopkit_test_application();

namespace
{
    using slopkit::ui::WindowCenterer;

    // Where a window of `window`'s size lands when it is centred on `parent`.
    QPoint centred_position(const QWidget& parent, const QWidget& window)
    {
        const QSize own = window.size();
        return parent.frameGeometry().center() - QPoint(own.width() / 2, own.height() / 2);
    }
} // namespace

TEST_CASE("a centerer puts a child dialog at the centre of its parent", "[window_centerer]")
{
    static_cast<void>(slopkit_test_application());

    QWidget parent;
    parent.resize(800, 600);
    parent.show();
    QCoreApplication::processEvents();

    QDialog dialog {&parent};
    dialog.resize(200, 100);
    WindowCenterer centerer {dialog};

    dialog.show();
    QCoreApplication::processEvents();

    CHECK(dialog.isWindow());
    CHECK(dialog.pos() == centred_position(parent, dialog));
    CHECK(dialog.size() == QSize(200, 100));
}

TEST_CASE("a centerer re-centres a dialog on every show", "[window_centerer]")
{
    static_cast<void>(slopkit_test_application());

    QWidget parent;
    parent.resize(800, 600);
    parent.show();
    QCoreApplication::processEvents();

    QDialog dialog {&parent};
    dialog.resize(240, 120);
    WindowCenterer centerer {dialog};

    dialog.show();
    QCoreApplication::processEvents();
    const QPoint first = dialog.pos();

    dialog.move(first + QPoint(150, 90));
    QCoreApplication::processEvents();
    REQUIRE(dialog.pos() != first);

    dialog.hide();
    QCoreApplication::processEvents();
    dialog.show();
    QCoreApplication::processEvents();

    CHECK(dialog.pos() == centred_position(parent, dialog));
}

TEST_CASE("a centerer leaves a parentless window where it is", "[window_centerer]")
{
    static_cast<void>(slopkit_test_application());

    QDialog window;
    window.resize(200, 100);
    WindowCenterer centerer {window};

    window.move(120, 80);
    window.show();
    QCoreApplication::processEvents();

    CHECK(window.pos() == QPoint(120, 80));
    CHECK(window.size() == QSize(200, 100));
}
