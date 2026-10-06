#include <catch2/catch.hpp>

#include <cstdint>
#include <vector>

#include "debug/backend.hpp"
#include "support/fake_process.hpp"
#include "ui/components/elided_tooltip_delegate.hpp"
#include "ui/models/call_stack_model.hpp"

using slopkit::debug::Frame;
using slopkit::ui::models::CallStackModel;

TEST_CASE("call stack model renders frames in the current mode", "[ui][models][call_stack]")
{
    CallStackModel           model;
    slopkit::ui::ModuleSpans spans;

    CHECK(model.rowCount() == 0);
    CHECK(model.columnCount() == 3);

    const std::vector<Frame> frames {
        {0x401000,      0},
        {0x402000, 0x7FFF}
    };
    model.set_frames(frames, spans, slopkit::ui::AddressMode::absolute);

    CHECK(model.rowCount() == 2);
    CHECK(model.data(model.index(0, CallStackModel::frame), Qt::DisplayRole).toString() == QStringLiteral("0"));
    CHECK(model.data(model.index(1, CallStackModel::frame), Qt::DisplayRole).toString() == QStringLiteral("1"));

    // With no module spans, the address column falls back to the absolute text.
    CHECK(model.data(model.index(0, CallStackModel::address), Qt::DisplayRole)
          == model.data(model.index(0, CallStackModel::absolute), Qt::DisplayRole));
    CHECK(model.data(model.index(1, CallStackModel::absolute), Qt::DisplayRole)
              .toString()
              .contains(QStringLiteral("402000")));

    model.clear();
    CHECK(model.rowCount() == 0);
}

TEST_CASE("call stack model answers the full-text role with the untruncated address", "[ui][models][call_stack]")
{
    CallStackModel                                  model;
    slopkit::ui::ModuleSpans                        spans;
    const std::vector<slopkit::process::ModuleInfo> modules {
        slopkit::test::module_image("DyingLightGame_TheBeast_x64_rwdi.exe", 0x1000, 0x10000)};
    spans.set_modules(modules);

    model.set_frames(
        std::vector {
            Frame {0x1010, 0}
    },
        spans,
        slopkit::ui::AddressMode::module_relative);

    const QModelIndex address = model.index(0, CallStackModel::address);
    CHECK(model.data(address, Qt::DisplayRole).toString() == QStringLiteral("DyingL..._rwdi.exe+10"));
    CHECK(model.data(address, slopkit::ui::widgets::kFullTextRole).toString()
          == QStringLiteral("DyingLightGame_TheBeast_x64_rwdi.exe+10"));
    // Only the address column carries the full text.
    CHECK(model.data(model.index(0, CallStackModel::frame), slopkit::ui::widgets::kFullTextRole).toString().isEmpty());
}

TEST_CASE("call stack model keeps only the top frame of an empty stack", "[ui][models][call_stack]")
{
    CallStackModel           model;
    slopkit::ui::ModuleSpans spans;

    model.set_frames(
        std::vector {
            Frame {0x1000, 0}
    },
        spans,
        slopkit::ui::AddressMode::module_relative);
    CHECK(model.rowCount() == 1);
    CHECK_FALSE(model.data(model.index(0, CallStackModel::address), Qt::DisplayRole).toString().isEmpty());
}
