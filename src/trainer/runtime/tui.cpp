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

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include "trainer/runtime/console.hpp"

namespace slopkit::trainer::tui
{
    Model::Model(std::size_t value_rows, std::span<const ScriptState> scripts) : value_rows_ {value_rows}
    {
        for (std::size_t number = 1; number <= value_rows; ++number)
        {
            rows_.push_back(Row {Row::Kind::value, number});
        }
        set_scripts(scripts);
    }

    void Model::set_scripts(std::span<const ScriptState> scripts)
    {
        rows_.resize(value_rows_);
        script_active_.clear();
        script_active_.reserve(scripts.size());
        for (std::size_t position = 0; position < scripts.size(); ++position)
        {
            script_active_.push_back(scripts[position].active);
            rows_.push_back(Row {Row::Kind::script, position + 1});
        }

        if (!rows_.empty() && selected_ >= rows_.size())
        {
            selected_ = rows_.size() - 1;
        }
        clamp_scroll();
    }

    std::size_t Model::row_count() const noexcept
    {
        return rows_.size();
    }

    const Row& Model::row(std::size_t position) const
    {
        return rows_[position];
    }

    std::optional<std::size_t> Model::row_at_offset(std::size_t offset) const
    {
        if (offset >= visible_)
        {
            return std::nullopt;
        }
        const auto position = scroll_ + offset;
        if (position >= rows_.size())
        {
            return std::nullopt;
        }
        return position;
    }

    std::size_t Model::selected() const noexcept
    {
        return selected_;
    }

    void Model::select(std::size_t position)
    {
        if (rows_.empty())
        {
            selected_ = 0;
            clamp_scroll();
            return;
        }
        selected_ = std::min(position, rows_.size() - 1);
        clamp_scroll();
    }

    void Model::move(std::ptrdiff_t delta)
    {
        if (rows_.empty())
        {
            return;
        }
        const auto last = static_cast<std::ptrdiff_t>(rows_.size() - 1);
        const auto next = std::clamp(static_cast<std::ptrdiff_t>(selected_) + delta, std::ptrdiff_t {0}, last);
        select(static_cast<std::size_t>(next));
    }

    void Model::select_first()
    {
        select(0);
    }

    void Model::select_last()
    {
        if (!rows_.empty())
        {
            select(rows_.size() - 1);
        }
    }

    std::size_t Model::scroll() const noexcept
    {
        return scroll_;
    }

    std::size_t Model::visible_rows() const noexcept
    {
        return visible_;
    }

    void Model::set_visible_rows(std::size_t count)
    {
        visible_ = count;
        clamp_scroll();
    }

    const std::string& Model::input() const noexcept
    {
        return input_;
    }

    void Model::append_input(std::string_view text)
    {
        input_ += text;
    }

    void Model::backspace_input()
    {
        if (!input_.empty())
        {
            input_.pop_back();
        }
    }

    void Model::clear_input()
    {
        input_.clear();
    }

    bool Model::help_shown() const noexcept
    {
        return help_shown_;
    }

    void Model::set_help_shown(bool shown)
    {
        help_shown_ = shown;
    }

    const std::string& Model::message() const noexcept
    {
        return message_;
    }

    void Model::set_message(std::string message)
    {
        message_ = std::move(message);
    }

    Action Model::toggle_row(std::size_t position) const
    {
        if (position >= rows_.size())
        {
            return Action {};
        }

        const Row& line = rows_[position];
        Action     action;
        action.index = line.number;
        if (line.kind == Row::Kind::value)
        {
            action.kind = Action::Kind::freeze;
            return action;
        }

        // A script that failed is inactive, so the same click retries it.
        const bool active = line.number <= script_active_.size() && script_active_[line.number - 1];
        action.kind       = active ? Action::Kind::deactivate_script : Action::Kind::activate_script;
        return action;
    }

    Action Model::submit()
    {
        const auto action = command(input_);
        clear_input();
        return action;
    }

    Action Model::command(std::string_view text)
    {
        const console::Command parsed = console::parse_command(text);

        Action action;
        action.index = parsed.index;
        action.value = parsed.value;

        switch (parsed.kind)
        {
        case console::CommandKind::none:
        case console::CommandKind::list:
            break;
        case console::CommandKind::help:
            help_shown_ = true;
            break;
        case console::CommandKind::invalid:
            message_ = std::format("unrecognized input \"{}\"; press ? for help", parsed.text);
            break;
        case console::CommandKind::quit:
            action.kind = Action::Kind::quit;
            break;
        case console::CommandKind::refresh:
            action.kind = Action::Kind::refresh;
            break;
        case console::CommandKind::freeze:
            action.kind = Action::Kind::freeze;
            break;
        case console::CommandKind::set_value:
            action.kind = Action::Kind::set_value;
            break;
        case console::CommandKind::activate_script:
            action.kind = Action::Kind::activate_script;
            break;
        case console::CommandKind::deactivate_script:
            action.kind = Action::Kind::deactivate_script;
            break;
        }
        return action;
    }

    void Model::clamp_scroll()
    {
        if (visible_ == 0 || rows_.size() <= visible_)
        {
            scroll_ = 0;
            return;
        }
        if (selected_ < scroll_)
        {
            scroll_ = selected_;
        }
        else if (selected_ >= scroll_ + visible_)
        {
            scroll_ = selected_ - visible_ + 1;
        }
        scroll_ = std::min(scroll_, rows_.size() - visible_);
    }

    namespace
    {
        // The header, the two separators, the input line and the status line: what
        // the list cannot use, so a click always lands on a row that is on screen.
        constexpr int kChromeRows {5};

        // How often the loop wakes up to pick input up and to notice a due update.
        constexpr std::chrono::milliseconds kPollInterval {20};

        constexpr std::ptrdiff_t kWheelRows {3};

        std::string trim_newline(std::string text)
        {
            if (!text.empty() && text.back() == '\n')
            {
                text.pop_back();
            }
            return text;
        }

        // The status line is one line tall, so a message that carries a hook's
        // output is folded onto it rather than changing the layout.
        std::string flatten(std::string text)
        {
            for (auto position = text.find('\n'); position != std::string::npos; position = text.find('\n', position))
            {
                text.replace(position, 1, " | ");
            }
            return text;
        }

        // The row exactly as the printed list renders it, so the two front ends
        // cannot drift apart.
        std::string row_text(const Payload&               payload,
                             std::span<const LiveEntry>   live,
                             std::span<const ScriptState> scripts,
                             const Row&                   row)
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
        }

        std::optional<ftxui::Color> row_color(const Row& row, std::span<const ScriptState> scripts)
        {
            if (row.kind != Row::Kind::script || row.number > scripts.size())
            {
                return std::nullopt;
            }
            if (scripts[row.number - 1].failed)
            {
                return ftxui::Color::Red;
            }
            return scripts[row.number - 1].active ? std::optional {ftxui::Color::Green} : std::nullopt;
        }
    }

    int run_tui(const Payload&                                   payload,
                std::span<const LiveEntry>                       live,
                std::span<const ScriptState>                     scripts,
                const std::function<std::uint32_t()>&            pid,
                std::uint32_t                                    interval_ms,
                const std::function<std::string(const Action&)>& perform,
                const std::function<void()>&                     step)
    {
        Model                   model {payload.entries.size(), scripts};
        std::vector<ftxui::Box> boxes;

        ftxui::App screen = ftxui::App::Fullscreen();
        screen.TrackMouse(true);

        bool finished = false;

        const auto apply = [&](const Action& action) -> bool
        {
            if (action.kind == Action::Kind::quit)
            {
                finished = true;
                screen.Exit();
                return true;
            }
            if (action.kind != Action::Kind::none)
            {
                // A line the grammar did not recognize already left its message;
                // everything else answers through `perform`.
                model.set_message(perform(action));
            }
            return true;
        };

        auto content = ftxui::Renderer(
            [&]() -> ftxui::Element
            {
                // The host's states are the truth about which scripts are active, so
                // the rows are rebuilt from them on every frame.
                model.set_scripts(scripts);
                model.set_visible_rows(static_cast<std::size_t>(std::max(screen.dimy() - kChromeRows, 1)));

                const auto first = std::min(model.scroll(), model.row_count());
                const auto drawn = std::min(model.visible_rows(), model.row_count() - first);
                boxes.assign(drawn, ftxui::Box {});

                ftxui::Elements lines;
                lines.reserve(drawn);
                for (std::size_t offset = 0; offset < drawn; ++offset)
                {
                    const auto position = first + offset;
                    const Row& line     = model.row(position);

                    auto element = ftxui::text(row_text(payload, live, scripts, line));
                    if (const auto tint = row_color(line, scripts))
                    {
                        element = element | ftxui::color(*tint);
                    }
                    if (position == model.selected())
                    {
                        element = std::move(element) | ftxui::inverted;
                    }
                    lines.push_back(std::move(element) | ftxui::reflect(boxes[offset]));
                }

                auto header = ftxui::text(trim_newline(console::render_header(payload, pid()))) | ftxui::bold;
                auto list   = ftxui::vbox(std::move(lines)) | ftxui::yflex_grow;
                auto prompt =
                    ftxui::hbox({ftxui::text("slopkit-trainer> "), ftxui::text(model.input()), ftxui::text("_")});
                auto status = ftxui::text(flatten(model.message()));
                auto main   = ftxui::vbox({std::move(header),
                                           ftxui::separator(),
                                           std::move(list),
                                           ftxui::separator(),
                                           std::move(prompt),
                                           std::move(status)});

                if (!model.help_shown())
                {
                    return main;
                }

                auto help =
                    ftxui::window(ftxui::text("help"), ftxui::paragraph(console::render_help())) | ftxui::center;
                return ftxui::dbox({std::move(main), std::move(help)});
            });

        auto root = ftxui::CatchEvent(content,
                                      [&](ftxui::Event event) -> bool
                                      {
                                          // FTXUI's own Ctrl-C handler restores the
                                          // terminal and quits, so the event stays
                                          // unhandled here.
                                          if (event == ftxui::Event::CtrlC)
                                          {
                                              finished = true;
                                              return false;
                                          }
                                          if (event == ftxui::Event::Custom)
                                          {
                                              return true;
                                          }
                                          if (model.help_shown())
                                          {
                                              model.set_help_shown(false);
                                              return true;
                                          }

                                          if (event.is_mouse())
                                          {
                                              auto& mouse = event.mouse();
                                              if (mouse.motion != ftxui::Mouse::Pressed)
                                              {
                                                  return true;
                                              }
                                              if (mouse.button == ftxui::Mouse::WheelUp)
                                              {
                                                  model.move(-kWheelRows);
                                                  return true;
                                              }
                                              if (mouse.button == ftxui::Mouse::WheelDown)
                                              {
                                                  model.move(kWheelRows);
                                                  return true;
                                              }
                                              if (mouse.button == ftxui::Mouse::Left)
                                              {
                                                  for (std::size_t offset = 0; offset < boxes.size(); ++offset)
                                                  {
                                                      if (boxes[offset].Contain(mouse.x, mouse.y))
                                                      {
                                                          return apply(model.toggle_row(model.scroll() + offset));
                                                      }
                                                  }
                                              }
                                              return true;
                                          }

                                          if (event == ftxui::Event::ArrowUp)
                                          {
                                              model.move(-1);
                                              return true;
                                          }
                                          if (event == ftxui::Event::ArrowDown)
                                          {
                                              model.move(1);
                                              return true;
                                          }
                                          if (event == ftxui::Event::PageUp)
                                          {
                                              model.move(-static_cast<std::ptrdiff_t>(model.visible_rows()));
                                              return true;
                                          }
                                          if (event == ftxui::Event::PageDown)
                                          {
                                              model.move(static_cast<std::ptrdiff_t>(model.visible_rows()));
                                              return true;
                                          }
                                          // Home and End belong to the list while the
                                          // input line is empty, and to nothing else:
                                          // the console has no line editing.
                                          if (event == ftxui::Event::Home && model.input().empty())
                                          {
                                              model.select_first();
                                              return true;
                                          }
                                          if (event == ftxui::Event::End && model.input().empty())
                                          {
                                              model.select_last();
                                              return true;
                                          }
                                          if (event == ftxui::Event::Backspace)
                                          {
                                              model.backspace_input();
                                              return true;
                                          }
                                          if (event == ftxui::Event::Escape)
                                          {
                                              model.clear_input();
                                              return true;
                                          }
                                          if (event == ftxui::Event::Return)
                                          {
                                              // Enter runs a typed line, and toggles
                                              // the selected row when there is none.
                                              return apply(model.input().empty() ? model.toggle_row(model.selected())
                                                                                 : model.submit());
                                          }
                                          if (event.is_character())
                                          {
                                              model.append_input(event.character());
                                              return true;
                                          }
                                          return false;
                                      });

        ftxui::Loop loop(&screen, root);
        const auto  interval  = std::chrono::milliseconds {interval_ms == 0 ? 250 : interval_ms};
        auto        next_step = std::chrono::steady_clock::now();
        screen.PostEvent(ftxui::Event::Custom); // the first frame

        while (!finished && !loop.HasQuitted())
        {
            loop.RunOnce();

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_step)
            {
                step();
                next_step = now + interval;
                screen.PostEvent(ftxui::Event::Custom);
                continue;
            }

            // Waking up more often than the interval keeps typing responsive
            // without a second thread touching the target.
            const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_step - now);
            std::this_thread::sleep_for(std::min(kPollInterval, wait));
        }

        return kExitOk;
    }

}
