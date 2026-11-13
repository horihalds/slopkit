#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>

#include "process/access.hpp"
#include "process/access_worker.hpp"
#include "process/types.hpp"
#include "script/symbols.hpp"
#include "support/fake_process.hpp"
#include "support/ui_helpers.hpp"
#include "ui/dialogs/pointer_scan.hpp"
#include "ui/models/pointer_scan_model.hpp"

namespace
{
    using slopkit::test::application;
    using slopkit::test::fake_target;
    using slopkit::test::FakeAccess;
    using slopkit::test::pump_ui;
    using slopkit::ui::dialogs::PointerScanDialog;

    void attach_session(slopkit::process::AccessWorker& worker)
    {
        bool attached = false;
        worker.submit_attach_app(worker.next_job_id(),
                                 42,
                                 "fake",
                                 [&](slopkit::process::JobResult&&)
                                 {
                                     attached = true;
                                 });
        REQUIRE(pump_ui(worker,
                        [&]
                        {
                            return attached;
                        }));
    }

    void store_pointer(std::shared_ptr<slopkit::test::FakeMemory> memory, std::uint64_t address, std::uint64_t value)
    {
        const std::uint64_t offset = address - memory->base;
        for (std::size_t index = 0; index < 8; ++index)
        {
            memory->flat[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xFF);
        }
    }
}

TEST_CASE("the pointer scan window exposes the scan defaults", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::script::SymbolTable     symbols;

    PointerScanDialog dialog {worker, target, symbols};

    CHECK(dialog.windowTitle() == QStringLiteral("Pointer Scan"));
    CHECK(dialog.parentWidget() == nullptr);
    CHECK(dialog.isWindow());
    CHECK_FALSE(dialog.testAttribute(Qt::WA_QuitOnClose));
    CHECK(dialog.levels_spin()->value() == 3);
    CHECK(dialog.offset_edit()->text() == QStringLiteral("0x1000"));
    CHECK_FALSE(dialog.negative_check()->isChecked());
    CHECK_FALSE(dialog.scan_button()->isEnabled());
    dialog.hide();
}

TEST_CASE("the pointer scan window scans the selected module", "[ui]")
{
    application();

    FakeAccess                       access;
    slopkit::process::AccessWorker   worker {access};
    slopkit::process::AttachedTarget target = fake_target();
    slopkit::script::SymbolTable     symbols;

    access.modules                      = {slopkit::test::module_image("game.so", 0x3000, 0x1000)};
    access.modules.front().pointer_size = 8;
    access.modules.front().is_main      = true;
    access.regions                      = {slopkit::test::region_of(0x3000, 0x1000, "game.so", false, true)};
    access.memory->base                 = 0x1000;
    access.memory->flat.assign(0x8000, std::byte {0});
    store_pointer(access.memory, 0x3010, 0x7000);

    attach_session(worker);

    PointerScanDialog dialog {worker, target, symbols};
    dialog.set_modules(access.modules);
    dialog.set_target(0x7000);
    dialog.levels_spin()->setValue(1);
    CHECK(dialog.scan_button()->isEnabled());
    CHECK(dialog.module_combo()->currentText() == QStringLiteral("game.so"));

    dialog.scan_button()->click();
    INFO(dialog.status_text().toStdString());
    REQUIRE(pump_ui(worker,
                    [&]
                    {
                        return dialog.row_count() == 1;
                    }));
    INFO(dialog.status_text().toStdString());

    CHECK(dialog.cell_text(0, slopkit::ui::models::PointerScanModel::base) == QStringLiteral("game.so+0x10"));
    CHECK(dialog.cell_text(0, slopkit::ui::models::PointerScanModel::offsets) == QStringLiteral("+0x0"));
    CHECK(dialog.status_text().contains(QStringLiteral("found 1 path")));

    QString activated_expression;
    quint64 activated_target = 0;
    QObject::connect(&dialog,
                     &PointerScanDialog::pathActivated,
                     &dialog,
                     [&](const QString& expression, quint64 target)
                     {
                         activated_expression = expression;
                         activated_target     = target;
                     });
    dialog.table()->doubleClicked(dialog.table()->model()->index(0, 0));
    CHECK(activated_expression == QStringLiteral("[game.so+0x10]+0x0"));
    CHECK(activated_target == 0x7000);

    dialog.hide();
}
