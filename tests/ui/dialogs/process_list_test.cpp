#include <catch2/catch.hpp>

#include "support/ui_helpers.hpp"

TEST_CASE("the process list dialog focuses the filter box and preselects the top result", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));

    // The filter box owns the keyboard focus and the top result is selected.
    CHECK(dialog.focusWidget() == search);
    CHECK(table->currentIndex().row() == 0);

    // Enter in the filter box attaches the preselected (top) process.
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &enter);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.valid();
                    }));
    CHECK(target.pid == 10);

    // A successful attach dismisses the picker.
    CHECK_FALSE(dialog.isVisible());

    dialog.close();
}

TEST_CASE("the process list dialog keeps itself refreshed without controls", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};

    // The picker opens at its compact default size and locks it: it cannot be
    // resized, and it offers no minimize or maximize affordance.
    CHECK(dialog.size() == QSize(600, 440));
    CHECK(dialog.minimumSize() == dialog.maximumSize());
    CHECK_FALSE(dialog.windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
    CHECK_FALSE(dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint));

    dialog.resize(900, 700);
    CHECK(dialog.size() == dialog.minimumSize());

    // The picker blocks the main window while it is open.
    CHECK(dialog.windowModality() == Qt::ApplicationModal);
    CHECK(dialog.isModal());

    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    // Neither the Refresh button nor the Auto-refresh check box exists.
    CHECK(button_labelled(dialog, QStringLiteral("Refresh")) == nullptr);
    CHECK(checkbox_labelled(dialog, QStringLiteral("Auto-refresh")) == nullptr);
    CHECK(dialog.findChildren<QCheckBox*>().isEmpty());

    // The Plugins combo box leads the control row; the filter box takes the
    // remaining width.
    QComboBox* plugin_combo = nullptr;
    for (auto* combo : dialog.findChildren<QComboBox*>())
    {
        if (combo->count() > 0 && combo->itemText(0) == QStringLiteral("All plugins"))
        {
            plugin_combo = combo;
        }
    }
    REQUIRE(plugin_combo != nullptr);

    // The combo sizes itself to its entries instead of the empty box it had at
    // first show, so "All plugins" is never elided.
    CHECK(plugin_combo->sizeHint().width() > plugin_combo->fontMetrics().horizontalAdvance(plugin_combo->itemText(0)));

    QHBoxLayout* controls = nullptr;
    for (auto* row : dialog.findChildren<QHBoxLayout*>())
    {
        if (row->indexOf(plugin_combo) >= 0 && row->indexOf(search) >= 0)
        {
            controls = row;
        }
    }
    REQUIRE(controls != nullptr);
    CHECK(controls->indexOf(plugin_combo) < controls->indexOf(search));
    CHECK(controls->stretch(controls->indexOf(search)) == 1);

    // The control row is the dialog's first layout item: no status strip sits
    // above it.
    REQUIRE(dialog.layout() != nullptr);
    REQUIRE(dialog.layout()->count() > 0);
    CHECK(dialog.layout()->itemAt(0)->layout() == controls);

    // The old "N process(es) shown" counter is gone entirely.
    for (auto* label : dialog.findChildren<slopkit::ui::widgets::StatusLabel*>())
    {
        CHECK_FALSE(label->text().contains(QStringLiteral("process(es)")));
    }

    // Merely leaving the picker up re-lists the processes on its own.
    const int listed = access.list_calls.load();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return access.list_calls.load() > listed;
                    }));

    dialog.close();
}

TEST_CASE("double-clicking a process row attaches it", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* table = dialog.findChild<QTableView*>();
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    // Double-click the second row: a press parks the index, the double-click
    // activates it.
    const QModelIndex second = table->model()->index(1, slopkit::ui::dialogs::ProcessListModel::pid);
    const QRect       rect   = table->visualRect(second);
    REQUIRE_FALSE(rect.isEmpty());
    const QPoint pos = rect.center();

    QMouseEvent press {QEvent::MouseButtonPress,
                       pos,
                       table->viewport()->mapToGlobal(pos),
                       Qt::LeftButton,
                       Qt::LeftButton,
                       Qt::NoModifier};
    QCoreApplication::sendEvent(table->viewport(), &press);
    QMouseEvent dbl {QEvent::MouseButtonDblClick,
                     pos,
                     table->viewport()->mapToGlobal(pos),
                     Qt::LeftButton,
                     Qt::LeftButton,
                     Qt::NoModifier};
    QCoreApplication::sendEvent(table->viewport(), &dbl);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.valid();
                    }));
    CHECK(target.pid == 20);

    dialog.close();
}

TEST_CASE("the process list dialog ignores Enter when the list is empty", "[ui]")
{
    application();

    FakeAccess                       access; // Empty listing.
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    REQUIRE(search != nullptr);

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};

    // No selection yet: Enter must not submit an attach.
    QCoreApplication::sendEvent(search, &enter);
    QCoreApplication::processEvents();
    CHECK(access.attach_calls.load() == 0);
    CHECK_FALSE(target.valid());

    // ...nor once the empty listing has landed.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return access.list_calls.load() >= 1;
                    }));
    QCoreApplication::processEvents();
    QCoreApplication::sendEvent(search, &enter);
    QCoreApplication::processEvents();
    CHECK(access.attach_calls.load() == 0);
    CHECK_FALSE(target.valid());

    dialog.close();
}

TEST_CASE("a failed attach leaves the process list dialog open", "[ui]")
{
    application();

    FakeAccess access;
    access.processes    = sample_processes();
    access.attach_fails = true;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &enter);

    // The failure is surfaced in the status area and the picker stays up.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        for (auto* label : dialog.findChildren<slopkit::ui::widgets::StatusLabel*>())
                        {
                            if (label->text().contains(QStringLiteral("attach failed")))
                            {
                                return true;
                            }
                        }
                        return false;
                    }));

    CHECK_FALSE(target.valid());
    CHECK(dialog.isVisible());

    // The message is not tied to the detail pane: it survives losing the
    // selection.
    table->selectionModel()->clearCurrentIndex();
    QCoreApplication::processEvents();
    bool message_still_shown = false;
    for (auto* label : dialog.findChildren<slopkit::ui::widgets::StatusLabel*>())
    {
        if (label->text().contains(QStringLiteral("attach failed")))
        {
            message_still_shown = true;
        }
    }
    CHECK(message_still_shown);

    dialog.close();
}

TEST_CASE("detaching does not close the process list dialog", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;
    target.pid          = 10;
    target.name         = "alpha";
    target.plugin_id    = "fake";
    target.method       = slopkit::process::AccessMethod::procfs_mem;
    target.session_live = true;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* table = dialog.findChild<QTableView*>();
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    // Row 0 (pid 10) is the attached process, so the button detaches it.
    QPushButton* attach_button = nullptr;
    for (auto* button : dialog.findChildren<QPushButton*>())
    {
        if (button->text() == QStringLiteral("Detach"))
        {
            attach_button = button;
        }
    }
    REQUIRE(attach_button != nullptr);
    attach_button->click();

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return !target.valid();
                    }));
    CHECK(dialog.isVisible());

    dialog.close();
}

TEST_CASE("the process model keeps desktop applications in the processes view", "[ui]")
{
    application();

    slopkit::ui::dialogs::ProcessListModel model;

    slopkit::process::ProcessInfo mumble;
    mumble.pid      = 100;
    mumble.name     = "Mumble";
    mumble.exe_path = "/usr/bin/mumble";

    slopkit::process::ProcessInfo shell;
    shell.pid      = 200;
    shell.name     = "bash";
    shell.exe_path = "/usr/bin/bash";

    model.set_processes({mumble, shell});
    model.set_application_index({"mumble"});

    // Only the PID and Name columns survive.
    CHECK(model.columnCount() == 2);
    CHECK(model.headerData(slopkit::ui::dialogs::ProcessListModel::pid, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("PID"));
    CHECK(model.headerData(slopkit::ui::dialogs::ProcessListModel::name, Qt::Horizontal, Qt::DisplayRole).toString()
          == QStringLiteral("Name"));

    // Sorting by Name still orders the visible rows.
    model.sort(slopkit::ui::dialogs::ProcessListModel::name, Qt::DescendingOrder);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 200);

    // Restore the default PID ordering for the filtering checks below.
    model.sort(slopkit::ui::dialogs::ProcessListModel::pid, Qt::AscendingOrder);

    // The Processes view lists everything, desktop application included.
    CHECK(model.rowCount() == 2);
    CHECK(model.row_for_pid(100) >= 0);
    CHECK(model.row_for_pid(200) >= 0);

    // ...so a name filter finds the application there too.
    model.set_search(QStringLiteral("mumble"));
    CHECK(model.rowCount() == 1);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 100);

    model.set_search(QString());

    // The Applications view restricts to desktop entries.
    model.set_applications_only(true);
    CHECK(model.rowCount() == 1);
    REQUIRE(model.process_at(0) != nullptr);
    CHECK(model.process_at(0)->pid == 100);
}

TEST_CASE("typing in the filter selects the first result", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    // Park the selection on the second row...
    table->setCurrentIndex(table->model()->index(1, slopkit::ui::dialogs::ProcessListModel::pid));
    REQUIRE(table->currentIndex().row() == 1);

    // ...then type a filter that still matches both rows.
    search->setText(QStringLiteral("usr/bin"));

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));
    CHECK(table->currentIndex().row() == 0);

    dialog.close();
}

TEST_CASE("Up and Down step the process list highlight with wrap-around", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));

    const auto step = [&](int key)
    {
        QKeyEvent event {QEvent::KeyPress, key, Qt::NoModifier};
        QCoreApplication::sendEvent(search, &event);
        QCoreApplication::processEvents();
    };

    // Down walks to the next row while the filter box keeps the focus.
    step(Qt::Key_Down);
    CHECK(table->currentIndex().row() == 1);
    CHECK(dialog.focusWidget() == search);

    // Down on the last row wraps to the first.
    step(Qt::Key_Down);
    CHECK(table->currentIndex().row() == 0);

    // Up on the first row wraps to the last.
    step(Qt::Key_Up);
    CHECK(table->currentIndex().row() == 1);

    // Up walks back.
    step(Qt::Key_Up);
    CHECK(table->currentIndex().row() == 0);
    CHECK(dialog.focusWidget() == search);

    dialog.close();
}

TEST_CASE("the process list highlight stays put on a single filtered result", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2;
                    }));

    search->setText(QStringLiteral("alpha"));
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 1
                            && table->currentIndex().row() == 0;
                    }));

    const auto step = [&](int key)
    {
        QKeyEvent event {QEvent::KeyPress, key, Qt::NoModifier};
        QCoreApplication::sendEvent(search, &event);
        QCoreApplication::processEvents();
    };

    // Wrapping around a single row leaves it current and attaches nothing: a
    // successful attach would have cleared the target's invalidity and closed
    // the picker.
    step(Qt::Key_Down);
    CHECK(table->currentIndex().row() == 0);
    step(Qt::Key_Up);
    CHECK(table->currentIndex().row() == 0);
    CHECK_FALSE(target.valid());
    CHECK(dialog.isVisible());

    dialog.close();
}

TEST_CASE("the process list dialog ignores arrows when the list is empty", "[ui]")
{
    application();

    FakeAccess                       access; // Empty listing.
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 0;
                    }));

    const auto step = [&](int key)
    {
        QKeyEvent event {QEvent::KeyPress, key, Qt::NoModifier};
        QCoreApplication::sendEvent(search, &event);
        QCoreApplication::processEvents();
    };

    // No rows: the arrows are no-ops, no attach is submitted and nothing crashes.
    step(Qt::Key_Down);
    step(Qt::Key_Up);
    CHECK(table->currentIndex().row() < 0);
    CHECK(access.attach_calls.load() == 0);
    CHECK_FALSE(target.valid());

    dialog.close();
}

TEST_CASE("the process list filter box keeps its other keys", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));

    // Ctrl+Down is left for the line edit: the highlight does not move.
    QKeyEvent ctrl_down {QEvent::KeyPress, Qt::Key_Down, Qt::ControlModifier};
    QCoreApplication::sendEvent(search, &ctrl_down);
    QCoreApplication::processEvents();
    CHECK(table->currentIndex().row() == 0);

    // Typing reaches the line edit; both rows still match via their path.
    QKeyEvent typed {QEvent::KeyPress, Qt::Key_U, Qt::NoModifier, QStringLiteral("u")};
    QCoreApplication::sendEvent(search, &typed);
    QCoreApplication::processEvents();
    CHECK(search->text() == QStringLiteral("u"));
    CHECK(search->cursorPosition() == 1);

    // ...and caret movement is not swallowed.
    QKeyEvent left {QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &left);
    QCoreApplication::processEvents();
    CHECK(search->cursorPosition() == 0);

    dialog.close();
}

TEST_CASE("Enter attaches the row the arrow keys highlighted", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));

    // Down highlights pid 20 (beta)...
    QKeyEvent down {QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &down);
    QCoreApplication::processEvents();
    REQUIRE(table->currentIndex().row() == 1);

    // ...and Enter in the filter box attaches it and dismisses the picker.
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &enter);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.valid();
                    }));
    CHECK(target.pid == 20);
    CHECK_FALSE(dialog.isVisible());

    dialog.close();
}

TEST_CASE("rapid arrow steps still attach the final highlighted row", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));

    // Three rapid downs on a two-row list land back on row 1 (pid 20).
    for (int i = 0; i < 3; ++i)
    {
        QKeyEvent down {QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier};
        QCoreApplication::sendEvent(search, &down);
    }
    QCoreApplication::processEvents();
    REQUIRE(table->currentIndex().row() == 1);

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &enter);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.valid();
                    }));
    CHECK(target.pid == 20);
    CHECK_FALSE(dialog.isVisible());

    dialog.close();
}

TEST_CASE("a failed attach of a stepped row keeps the highlight and the picker", "[ui]")
{
    application();

    FakeAccess access;
    access.processes    = sample_processes();
    access.attach_fails = true;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));

    QKeyEvent down {QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &down);
    QCoreApplication::processEvents();
    REQUIRE(table->currentIndex().row() == 1);

    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &enter);

    // The failure is surfaced, the picker stays up and the stepped row stays current.
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        for (auto* label : dialog.findChildren<slopkit::ui::widgets::StatusLabel*>())
                        {
                            if (label->text().contains(QStringLiteral("attach failed")))
                            {
                                return true;
                            }
                        }
                        return false;
                    }));
    CHECK_FALSE(target.valid());
    CHECK(dialog.isVisible());
    CHECK(table->currentIndex().row() == 1);

    dialog.close();
}

TEST_CASE("the process list details pane reports a readable target", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();

    slopkit::process::ModuleInfo main;
    main.base      = 0x1000;
    main.size      = 0x1000;
    main.kind      = slopkit::process::ModuleKind::elf;
    main.is_main   = true;
    main.path      = "/usr/bin/alpha";
    access.modules = {main};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* memory = dialog.findChild<QLabel*>(QStringLiteral("detail_memory"));
    REQUIRE(memory != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return memory->isVisible() && memory->text() == QStringLiteral("Memory: readable");
                    }));
    CHECK(memory->text() == QStringLiteral("Memory: readable"));

    dialog.close();
}

TEST_CASE("the process list details pane explains an unreadable target", "[ui]")
{
    application();

    FakeAccess access;
    access.processes  = sample_processes();
    access.read_error = slopkit::process::AccessError::permission_denied;

    slopkit::process::ModuleInfo main;
    main.base      = 0x1000;
    main.size      = 0x1000;
    main.kind      = slopkit::process::ModuleKind::elf;
    main.is_main   = true;
    main.path      = "/usr/bin/alpha";
    access.modules = {main};

    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* memory = dialog.findChild<QLabel*>(QStringLiteral("detail_memory"));
    REQUIRE(memory != nullptr);

    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return memory->isVisible()
                            && memory->text() == QStringLiteral("Memory: not readable (permission denied)");
                    }));

    // The failure is also visible on the picker's Details message line.
    bool warned = false;
    for (auto* label : dialog.findChildren<slopkit::ui::widgets::StatusLabel*>())
    {
        if (label->text().contains(QStringLiteral("memory not readable")))
        {
            warned = true;
        }
    }
    CHECK(warned);

    dialog.close();
}

TEST_CASE("a stepped process row survives the picker's auto-refresh", "[ui]")
{
    application();

    FakeAccess access;
    access.processes = sample_processes();
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target;

    slopkit::ui::dialogs::ProcessListDialog dialog {worker, target};
    dialog.show();

    auto* search = dialog.findChild<QLineEdit*>();
    auto* table  = dialog.findChild<QTableView*>();
    REQUIRE(search != nullptr);
    REQUIRE(table != nullptr);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return table->model() != nullptr && table->model()->rowCount() == 2
                            && table->currentIndex().row() == 0;
                    }));

    // Step to the last row.
    QKeyEvent down {QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &down);
    QCoreApplication::processEvents();
    REQUIRE(table->currentIndex().row() == 1);

    // The picker re-lists on its own; the stepped row stays current.
    const int listed = access.list_calls.load();
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return access.list_calls.load() > listed && table->model() != nullptr
                            && table->model()->rowCount() == 2 && table->currentIndex().row() == 1;
                    }));
    CHECK(table->currentIndex().row() == 1);

    // ...and Enter still attaches that pid.
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &enter);
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return target.valid();
                    }));
    CHECK(target.pid == 20);

    dialog.close();
}
