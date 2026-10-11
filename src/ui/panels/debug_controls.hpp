#pragma once

#include <cstdint>
#include <optional>

#include <QString>
#include <QWidget>

#include "debug/controller.hpp"
#include "process/attachment.hpp"

class QPushButton;

namespace slopkit::ui::panels
{
    class DebugControls : public QWidget
    {
        Q_OBJECT

    public:
        DebugControls(debug::Controller& controller, const process::AttachedTarget& target, QWidget* parent = nullptr);

        void set_selected_instruction(std::optional<std::uint64_t> address, const QString& expression);

        void refresh_target_state();

        [[nodiscard]] QPushButton* toggle_breakpoint_button() const noexcept;
        [[nodiscard]] QPushButton* start_stop_button() const noexcept;
        [[nodiscard]] QPushButton* resume_button() const noexcept;
        [[nodiscard]] QPushButton* break_button() const noexcept;
        [[nodiscard]] QPushButton* step_into_button() const noexcept;
        [[nodiscard]] QPushButton* step_over_button() const noexcept;
        [[nodiscard]] QPushButton* step_out_button() const noexcept;
        [[nodiscard]] QPushButton* breakpoints_button() const noexcept;

    signals:
        void breakpointsRequested();
        void toggleBreakpointRequested(std::uint64_t address, const QString& expression);

    private:
        void build_layout();
        void apply_state();
        void update_toggle_state();
        void toggle_session();

        debug::Controller&             controller_;
        const process::AttachedTarget& target_;

        QPushButton*                 toggle_breakpoint_button_ {};
        QPushButton*                 start_stop_button_ {};
        QPushButton*                 resume_button_ {};
        QPushButton*                 break_button_ {};
        QPushButton*                 step_into_button_ {};
        QPushButton*                 step_over_button_ {};
        QPushButton*                 step_out_button_ {};
        QPushButton*                 breakpoints_button_ {};
        std::optional<std::uint64_t> selected_address_;
        QString                      selected_expression_;
    };

}
