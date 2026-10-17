#include "trainer/runtime/tui.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include "trainer/runtime/console.hpp"

namespace slopkit::trainer::tui
{
    namespace
    {
        // The same chrome the FTXUI layout reserves: header, two separators, the
        // input line and the status line.
        constexpr SHORT kChromeRows {5};
        constexpr SHORT kBodyRow {2};

        constexpr WORD kDefault {FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE};
        constexpr WORD kHeader {kDefault | FOREGROUND_INTENSITY};
        constexpr WORD kSelected {BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE};
        constexpr WORD kActive {FOREGROUND_GREEN | FOREGROUND_INTENSITY};
        constexpr WORD kFailed {FOREGROUND_RED | FOREGROUND_INTENSITY};

        constexpr std::ptrdiff_t kWheelRows {3};

        struct Geometry
        {
            SHORT left {};
            SHORT top {};
            SHORT width {80};
            SHORT height {25};
        };

        std::wstring widen(std::string_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const int length =
                ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring result(static_cast<std::size_t>(length), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
            return result;
        }

        std::string narrow(wchar_t character)
        {
            char      buffer[8] {};
            const int length =
                ::WideCharToMultiByte(CP_UTF8, 0, &character, 1, buffer, sizeof(buffer), nullptr, nullptr);
            return std::string {buffer, static_cast<std::size_t>(std::max(length, 0))};
        }

        std::string trim_newline(std::string text)
        {
            if (!text.empty() && text.back() == '\n')
            {
                text.pop_back();
            }
            return text;
        }

        std::string flatten(std::string text)
        {
            for (auto position = text.find('\n'); position != std::string::npos; position = text.find('\n', position))
            {
                text.replace(position, 1, " | ");
            }
            return text;
        }

        Geometry geometry_of(HANDLE output)
        {
            Geometry                   result;
            CONSOLE_SCREEN_BUFFER_INFO info {};
            if (::GetConsoleScreenBufferInfo(output, &info) == 0)
            {
                return result;
            }
            result.left   = info.srWindow.Left;
            result.top    = info.srWindow.Top;
            result.width  = static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
            result.height = static_cast<SHORT>(info.srWindow.Bottom - info.srWindow.Top + 1);
            return result;
        }

        // Removes the scrollback the console starts with, so a full frame always
        // lands in the window and console mouse rows mean what they say.
        void fit_buffer(HANDLE output, const Geometry& geometry)
        {
            CONSOLE_SCREEN_BUFFER_INFO info {};
            if (::GetConsoleScreenBufferInfo(output, &info) == 0)
            {
                return;
            }
            if (info.dwSize.X != geometry.width || info.dwSize.Y != geometry.height)
            {
                static_cast<void>(::SetConsoleScreenBufferSize(output, COORD {geometry.width, geometry.height}));
            }
        }

        class Painter
        {
        public:
            Painter(HANDLE output, Geometry geometry)
                : output_ {output}, geometry_ {geometry},
                  cells_(static_cast<std::size_t>(geometry.width) * static_cast<std::size_t>(geometry.height),
                         CHAR_INFO {L' ', kDefault})
            {
            }

            void fill(SHORT row, wchar_t character, WORD attribute)
            {
                for (SHORT column = 0; column < geometry_.width; ++column)
                {
                    at(row, column) = CHAR_INFO {character, attribute};
                }
            }

            void put(SHORT row, SHORT column, std::wstring_view text, WORD attribute, bool pad = false)
            {
                if (row < 0 || row >= geometry_.height)
                {
                    return;
                }
                SHORT position = column;
                for (const wchar_t character : text)
                {
                    if (position >= geometry_.width)
                    {
                        break;
                    }
                    if (position >= 0)
                    {
                        at(row, position) = CHAR_INFO {character, attribute};
                    }
                    ++position;
                }
                if (!pad)
                {
                    return;
                }
                for (; position < geometry_.width; ++position)
                {
                    if (position >= 0)
                    {
                        at(row, position) = CHAR_INFO {L' ', attribute};
                    }
                }
            }

            void flush()
            {
                SMALL_RECT region {geometry_.left,
                                   geometry_.top,
                                   static_cast<SHORT>(geometry_.left + geometry_.width - 1),
                                   static_cast<SHORT>(geometry_.top + geometry_.height - 1)};
                static_cast<void>(::WriteConsoleOutputW(
                    output_, cells_.data(), COORD {geometry_.width, geometry_.height}, COORD {0, 0}, &region));
            }

        private:
            CHAR_INFO& at(SHORT row, SHORT column)
            {
                return cells_[static_cast<std::size_t>(row) * static_cast<std::size_t>(geometry_.width)
                              + static_cast<std::size_t>(column)];
            }

            HANDLE                 output_;
            Geometry               geometry_;
            std::vector<CHAR_INFO> cells_;
        };

        bool read_events(HANDLE input, std::vector<INPUT_RECORD>& events)
        {
            DWORD available = 0;
            if (::GetNumberOfConsoleInputEvents(input, &available) == 0 || available == 0)
            {
                return false;
            }
            events.resize(std::min<DWORD>(available, 32));
            DWORD read = 0;
            if (::ReadConsoleInputW(input, events.data(), static_cast<DWORD>(events.size()), &read) == 0)
            {
                return false;
            }
            events.resize(read);
            return true;
        }
    }

    int run_window(const Payload&                                   payload,
                   std::span<const LiveEntry>                       live,
                   std::span<const ScriptState>                     scripts,
                   const std::function<std::uint32_t()>&            pid,
                   std::uint32_t                                    interval_ms,
                   const std::function<std::string(const Action&)>& perform,
                   const std::function<void()>&                     step)
    {
        // Wine starts the window child with its streams still on the terminal it
        // was launched from, so point them at the console window.
        static_cast<void>(std::freopen("CONIN$", "r", stdin));
        static_cast<void>(std::freopen("CONOUT$", "w", stdout));
        static_cast<void>(std::freopen("CONOUT$", "w", stderr));

        const HANDLE input  = ::GetStdHandle(STD_INPUT_HANDLE);
        const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (input == nullptr || output == nullptr)
        {
            return kExitOk;
        }

        DWORD input_mode = 0;
        if (::GetConsoleMode(input, &input_mode) != 0)
        {
            static_cast<void>(
                ::SetConsoleMode(input,
                                 (input_mode | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS)
                                     & ~ENABLE_QUICK_EDIT_MODE));
        }
        CONSOLE_CURSOR_INFO cursor {1, FALSE};
        static_cast<void>(::SetConsoleCursorInfo(output, &cursor));
        ::SetConsoleTitleW(L"slopkit-trainer");

        Geometry geometry = geometry_of(output);
        fit_buffer(output, geometry);

        Model model {payload.entries.size(), scripts};

        bool finished = false;
        bool dirty    = true;

        const auto apply = [&](const Action& action)
        {
            if (action.kind == Action::Kind::quit)
            {
                finished = true;
                return;
            }
            if (action.kind != Action::Kind::none)
            {
                model.set_message(perform(action));
            }
        };

        const auto row_line = [&](const Row& row) -> std::string
        {
            if (row.kind == Row::Kind::value && row.number <= payload.entries.size() && row.number <= live.size())
            {
                return console::render_value_row(payload.entries[row.number - 1], live[row.number - 1], row.number);
            }
            if (row.kind == Row::Kind::script && row.number <= payload.scripts.size() && row.number <= scripts.size())
            {
                return console::render_script_row(payload.scripts[row.number - 1], scripts[row.number - 1], row.number);
            }
            return {};
        };

        const auto row_attribute = [&](const Row& row, std::size_t position) -> WORD
        {
            if (position == model.selected())
            {
                return kSelected;
            }
            if (row.kind == Row::Kind::script && row.number <= scripts.size())
            {
                if (scripts[row.number - 1].failed)
                {
                    return kFailed;
                }
                if (scripts[row.number - 1].active)
                {
                    return kActive;
                }
            }
            return kDefault;
        };

        const auto draw = [&]
        {
            geometry = geometry_of(output);
            model.set_visible_rows(static_cast<std::size_t>(std::max<SHORT>(geometry.height - kChromeRows, 1)));

            Painter painter {output, geometry};

            painter.put(0, 0, widen(trim_newline(console::render_header(payload, pid()))), kHeader);
            painter.fill(1, L'-', kDefault);

            const auto body_rows = static_cast<SHORT>(std::max<SHORT>(geometry.height - kChromeRows, 1));
            if (model.help_shown())
            {
                SHORT       row = kBodyRow;
                std::string line;
                for (const char character : console::render_help())
                {
                    if (character == '\n')
                    {
                        painter.put(row, 0, widen(line), kDefault);
                        line.clear();
                        ++row;
                        continue;
                    }
                    line.push_back(character);
                }
                painter.put(row, 0, widen(line), kDefault);
            }
            else
            {
                for (SHORT offset = 0; offset < body_rows; ++offset)
                {
                    const auto position = static_cast<std::size_t>(model.scroll()) + static_cast<std::size_t>(offset);
                    const auto row      = kBodyRow + offset;
                    if (position >= model.row_count())
                    {
                        painter.fill(row, L' ', kDefault);
                        continue;
                    }
                    const Row& line = model.row(position);
                    painter.put(row, 0, widen(row_line(line)), row_attribute(line, position), true);
                }
            }

            const SHORT separator = static_cast<SHORT>(kBodyRow + body_rows);
            painter.fill(separator, L'-', kDefault);
            painter.put(static_cast<SHORT>(separator + 1), 0, widen("slopkit-trainer> " + model.input()), kDefault);
            painter.put(static_cast<SHORT>(separator + 2), 0, widen(flatten(model.message())), kDefault);
            painter.flush();
            dirty = false;
        };

        const auto handle_key = [&](const KEY_EVENT_RECORD& key)
        {
            if (key.bKeyDown == 0)
            {
                return;
            }
            if (model.help_shown())
            {
                model.set_help_shown(false);
                return;
            }
            switch (key.wVirtualKeyCode)
            {
            case VK_UP:
                model.move(-1);
                return;
            case VK_DOWN:
                model.move(1);
                return;
            case VK_PRIOR:
                model.move(-static_cast<std::ptrdiff_t>(model.visible_rows()));
                return;
            case VK_NEXT:
                model.move(static_cast<std::ptrdiff_t>(model.visible_rows()));
                return;
            case VK_HOME:
                if (model.input().empty())
                {
                    model.select_first();
                }
                return;
            case VK_END:
                if (model.input().empty())
                {
                    model.select_last();
                }
                return;
            case VK_BACK:
                model.backspace_input();
                return;
            case VK_ESCAPE:
                model.clear_input();
                return;
            case VK_RETURN:
                apply(model.input().empty() ? model.toggle_row(model.selected()) : model.submit());
                return;
            default:
                break;
            }

            if (key.uChar.UnicodeChar == 3)
            {
                finished = true;
                return;
            }
            if (key.uChar.UnicodeChar >= L' ' && key.uChar.UnicodeChar != 0x7f)
            {
                model.append_input(narrow(key.uChar.UnicodeChar));
            }
        };

        const auto handle_mouse = [&](const MOUSE_EVENT_RECORD& mouse)
        {
            if (mouse.dwEventFlags == MOUSE_WHEELED)
            {
                const auto delta = static_cast<SHORT>(HIWORD(mouse.dwButtonState));
                model.move(delta > 0 ? -kWheelRows : kWheelRows);
                return;
            }
            if (mouse.dwEventFlags != 0 || (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) == 0)
            {
                return;
            }
            const SHORT offset = static_cast<SHORT>(mouse.dwMousePosition.Y - geometry.top - kBodyRow);
            if (offset < 0 || offset >= static_cast<SHORT>(model.visible_rows()))
            {
                return;
            }
            const auto position = static_cast<std::size_t>(model.scroll()) + static_cast<std::size_t>(offset);
            if (position >= model.row_count())
            {
                return;
            }
            model.select(position);
            apply(model.toggle_row(position));
        };

        std::vector<INPUT_RECORD> events;
        auto                      next_step = std::chrono::steady_clock::now();
        const auto                interval  = std::chrono::milliseconds {std::max<std::uint32_t>(interval_ms, 16)};

        while (!finished)
        {
            bool had_input = false;
            while (read_events(input, events))
            {
                had_input = true;
                for (const INPUT_RECORD& record : events)
                {
                    if (record.EventType == KEY_EVENT)
                    {
                        handle_key(record.Event.KeyEvent);
                    }
                    else if (record.EventType == MOUSE_EVENT)
                    {
                        handle_mouse(record.Event.MouseEvent);
                    }
                    else if (record.EventType == WINDOW_BUFFER_SIZE_EVENT)
                    {
                        dirty = true;
                    }
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_step)
            {
                step();
                next_step = now + interval;
                dirty     = true;
            }

            if (dirty)
            {
                model.set_scripts(scripts);
                draw();
            }
            if (!had_input)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds {10});
            }
        }

        return kExitOk;
    }
}
