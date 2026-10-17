#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "trainer/payload.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/scripts.hpp"

namespace slopkit::trainer::tui
{
    // One line of the interactive list: a value entry or a script. The two keep
    // their own numbering spaces, exactly as the printed rows do, and nothing a
    // user does on one row ever addresses the other space.
    struct Row
    {
        enum class Kind
        {
            value,
            script,
        };

        Kind        kind {Kind::value};
        std::size_t number {1}; // 1-based within the row's own space
    };

    // What a click or a typed command asks the session to do. The session applies
    // it and answers with the message to show, so the list's state never lives in
    // two places.
    struct Action
    {
        enum class Kind
        {
            none,
            freeze,
            set_value,
            activate_script,
            deactivate_script,
            refresh,
            quit,
        };

        Kind        kind {Kind::none};
        std::size_t index {}; // 1-based within the action's own space
        std::string value;
    };

    // The list's own state: which rows exist, which one is selected, how the list
    // is scrolled and what the input line holds. It knows neither FTXUI nor the
    // target, so the whole interaction is exercised without a terminal.
    class Model
    {
    public:
        Model(std::size_t value_rows, std::span<const ScriptState> scripts);

        // The script rows, taken from the loop's script states before a draw.
        void set_scripts(std::span<const ScriptState> scripts);

        [[nodiscard]] std::size_t row_count() const noexcept;
        [[nodiscard]] const Row&  row(std::size_t position) const;

        // The row drawn at `offset` within the visible window, which is what a
        // click position is translated through.
        [[nodiscard]] std::optional<std::size_t> row_at_offset(std::size_t offset) const;

        [[nodiscard]] std::size_t selected() const noexcept;
        void                      select(std::size_t position);
        void                      move(std::ptrdiff_t delta);
        void                      select_first();
        void                      select_last();

        [[nodiscard]] std::size_t scroll() const noexcept;
        [[nodiscard]] std::size_t visible_rows() const noexcept;
        void                      set_visible_rows(std::size_t count);

        [[nodiscard]] const std::string& input() const noexcept;
        void                             append_input(std::string_view text);
        void                             backspace_input();
        void                             clear_input();

        [[nodiscard]] bool help_shown() const noexcept;
        void               set_help_shown(bool shown);

        [[nodiscard]] const std::string& message() const noexcept;
        void                             set_message(std::string message);

        // The action a click on `position` implies, and the one the input line
        // implies; a line the grammar does not recognize leaves the message set
        // and asks for nothing.
        [[nodiscard]] Action toggle_row(std::size_t position) const;
        [[nodiscard]] Action submit();
        [[nodiscard]] Action command(std::string_view text);

    private:
        void clamp_scroll();

        std::size_t       value_rows_ {0};
        std::vector<Row>  rows_;
        std::vector<bool> script_active_;
        std::size_t       selected_ {0};
        std::size_t       scroll_ {0};
        std::size_t       visible_ {0};
        std::string       input_;
        std::string       message_;
        bool              help_shown_ {false};
    };

    // A console reports the mouse button's state with every record it queues, and
    // Wine repeats the state it believes on records that are not clicks at all,
    // so a pressed bit alone is not an input event: a held or stale button would
    // toggle the row under the pointer again for every record. Only the release
    // that ends a press is a click, and it lands on the row under the pointer at
    // that moment.
    class ClickTracker
    {
    public:
        // `pressed` is a mouse record's left-button state and `row` the list row
        // the pointer names, if any. Returns the row a click completed on.
        [[nodiscard]] std::optional<std::size_t> release(bool pressed, std::optional<std::size_t> row);

    private:
        bool down_ {false};
    };

    // Runs the full-screen interactive mode until the user quits. `perform`
    // applies one action to the session and answers with the message to show
    // (empty clears it); `step` is the periodic target work and `pid` is read
    // for every draw, since a re-attach changes it. All of them run on the
    // loop's own thread, which is also the one reading the keyboard, so the
    // target is only ever touched from there.
    // `console_window` asks for the list to be painted with the Win32 console
    // API instead of FTXUI, which a Wine console window cannot drive.
    int run_tui(const Payload&                                   payload,
                std::span<const LiveEntry>                       live,
                std::span<const ScriptState>                     scripts,
                const std::function<std::uint32_t()>&            pid,
                std::uint32_t                                    interval_ms,
                const std::function<std::string(const Action&)>& perform,
                const std::function<void()>&                     step,
                bool                                             console_window);

#if defined(_WIN32)
    // The `console_window` variant of `run_tui`, in `tui_win32.cpp`.
    int run_window(const Payload&                                   payload,
                   std::span<const LiveEntry>                       live,
                   std::span<const ScriptState>                     scripts,
                   const std::function<std::uint32_t()>&            pid,
                   std::uint32_t                                    interval_ms,
                   const std::function<std::string(const Action&)>& perform,
                   const std::function<void()>&                     step);
#endif

}
