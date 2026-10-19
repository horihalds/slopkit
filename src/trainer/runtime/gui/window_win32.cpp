#include "trainer/runtime/gui/window.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// windows.h comes first: commctrl.h needs the types it defines. The trainer's
// template is a console-subsystem binary, so a run from a terminal keeps its
// printed output and exit codes.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <commctrl.h>

#include "trainer/payload.hpp"
#include "trainer/runtime/console.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/rows.hpp"
#include "trainer/runtime/scripts.hpp"

namespace slopkit::trainer::gui
{
    namespace
    {
        constexpr UINT kSnapshotMessage    = WM_APP + 1;
        constexpr UINT kCloseEditorMessage = WM_APP + 2;

        constexpr int kRefreshCommand = 1001;
        constexpr int kCloseCommand   = 1002;
        constexpr int kHelpCommand    = 1003;
        constexpr int kAboutCommand   = 1004;

        constexpr int kCheckColumn  = 0;
        constexpr int kValueColumn  = 4;
        constexpr int kColumnCount  = 8;
        constexpr int kStatusHeight = 24;
        constexpr int kRowHeight    = 24;

        constexpr std::array<const wchar_t*, kColumnCount> kColumnTitles {
            L"", L"Description", L"Address", L"Type", L"Value", L"State", L"Interval", L"Message"};

        constexpr std::array<bool, kColumnCount> kMonoColumn {false, false, true, true, true, false, true, false};

        constexpr std::array<int, kColumnCount> kColumnWidth {30, 250, 170, 110, 160, 90, 90, 280};

        std::wstring widen(std::string_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring result(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
            return result;
        }

        std::string narrow(std::wstring_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const int length = WideCharToMultiByte(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(length), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
            return result;
        }

        struct WindowState;

        // The Win32 twin of SessionWorker: every perform/step call and every
        // target access they make happens on this thread.
        class SessionThread
        {
        public:
            SessionThread(WindowState& state, Perform perform, Step step, std::uint32_t interval_ms)
                : state_(state), perform_(std::move(perform)), step_(std::move(step)), interval_ms_(interval_ms)
            {
            }

            ~SessionThread()
            {
                stop();
            }

            SessionThread(const SessionThread&)            = delete;
            SessionThread& operator=(const SessionThread&) = delete;

            void start();
            void stop();
            void submit(Action action);

        private:
            void loop();
            void pass();

            WindowState&            state_;
            Perform                 perform_;
            Step                    step_;
            std::uint32_t           interval_ms_ {};
            std::mutex              mutex_;
            std::condition_variable wake_;
            std::deque<Action>      pending_;
            std::string             message_;
            std::thread             thread_;
            bool                    running_ {false};
        };

        struct WindowState
        {
            WindowState(const Payload&               payload,
                        std::span<const LiveEntry>   live,
                        std::span<const ScriptState> scripts,
                        Pid                          pid,
                        Options                      options,
                        Perform                      perform,
                        Step                         step)
                : payload_(payload), live_(live), scripts_(scripts), pid_(std::move(pid)), options_(std::move(options)),
                  perform_(std::move(perform)), step_(std::move(step))
            {
            }

            WindowState(const WindowState&)            = delete;
            WindowState& operator=(const WindowState&) = delete;

            void create_children();
            void layout();
            void apply_snapshot();
            void publish(std::shared_ptr<const std::vector<RowView>> rows, std::string header, std::string message);
            void set_message(std::string message);
            void submit(Action action);
            void toggle(std::size_t row);
            void begin_edit(std::size_t row);
            void apply_edit();
            void close_editor();
            void show_help();
            void show_about();

            [[nodiscard]] const RowView* row_at(std::size_t row) const;
            [[nodiscard]] std::wstring   cell_text(const RowView& row, int column) const;
            [[nodiscard]] static bool    checked(const RowView& row);

            HINSTANCE                      instance_ {};
            HWND                           window_ {};
            HWND                           list_ {};
            HWND                           editor_ {};
            HWND                           header_ {};
            HWND                           message_ {};
            HFONT                          ui_font_ {};
            HFONT                          mono_font_ {};
            HIMAGELIST                     row_images_ {};
            std::unique_ptr<SessionThread> session_;

            const Payload&               payload_;
            std::span<const LiveEntry>   live_;
            std::span<const ScriptState> scripts_;
            Pid                          pid_;
            Options                      options_;
            Perform                      perform_;
            Step                         step_;

            std::mutex                                  mutex_;
            std::shared_ptr<const std::vector<RowView>> pending_;
            std::shared_ptr<const std::vector<RowView>> displayed_;
            std::string                                 header_text_;
            std::string                                 message_text_;
            std::atomic<std::uint32_t>                  pid_cache_ {0};

            bool        editing_ {false};
            std::size_t edit_row_ {};
            int         exit_code_ {};
        };

        WindowState* g_state         = nullptr;
        WNDPROC      g_old_edit_proc = nullptr;

        void SessionThread::start()
        {
            running_ = true;
            thread_  = std::thread(&SessionThread::loop, this);
        }

        void SessionThread::stop()
        {
            {
                std::scoped_lock lock {mutex_};
                running_ = false;
            }
            wake_.notify_all();
            if (thread_.joinable())
            {
                thread_.join();
            }
        }

        void SessionThread::submit(Action action)
        {
            {
                std::scoped_lock lock {mutex_};
                pending_.push_back(std::move(action));
            }
            wake_.notify_one();
        }

        void SessionThread::loop()
        {
            // The first pass fills the window before the user sees it; a target
            // that is not running yet simply leaves the rows unresolved.
            pass();

            while (true)
            {
                std::deque<Action> actions;
                {
                    std::unique_lock lock {mutex_};
                    wake_.wait_for(lock,
                                   std::chrono::milliseconds {interval_ms_},
                                   [this]
                                   {
                                       return !running_ || !pending_.empty();
                                   });
                    if (!running_)
                    {
                        return;
                    }
                    actions.swap(pending_);
                }
                for (const auto& action : actions)
                {
                    const auto message = perform_(action);
                    if (!message.empty())
                    {
                        message_ = message;
                    }
                }
                pass();
            }
        }

        void SessionThread::pass()
        {
            step_();

            auto rows = std::make_shared<const std::vector<RowView>>(
                build_rows(state_.payload_, state_.live_, state_.scripts_));
            const auto pid = state_.pid_();
            state_.pid_cache_.store(pid);
            auto header = console::render_header(state_.payload_, pid);
            state_.publish(std::move(rows), std::move(header), message_);
        }

        LRESULT CALLBACK edit_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            if (g_state != nullptr)
            {
                if (message == WM_KEYDOWN && wparam == VK_RETURN)
                {
                    g_state->apply_edit();
                    return 0;
                }
                if (message == WM_KEYDOWN && wparam == VK_ESCAPE)
                {
                    g_state->close_editor();
                    return 0;
                }
                if (message == WM_CHAR && (wparam == L'\r' || wparam == L'\n' || wparam == 27))
                {
                    return 0;
                }
                if (message == WM_KILLFOCUS)
                {
                    // Destroying the editor inside its own focus message is
                    // fragile, so the window closes it after the fact.
                    PostMessageW(g_state->window_, kCloseEditorMessage, 0, 0);
                    return 0;
                }
            }
            return CallWindowProcW(g_old_edit_proc, hwnd, message, wparam, lparam);
        }

        void WindowState::create_children()
        {
            NONCLIENTMETRICSW metrics {};
            metrics.cbSize = sizeof(NONCLIENTMETRICSW);
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            {
                ui_font_ = CreateFontIndirectW(&metrics.lfMessageFont);
            }
            if (ui_font_ == nullptr)
            {
                ui_font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            }
            mono_font_ = CreateFontW(-12,
                                     0,
                                     0,
                                     0,
                                     FW_NORMAL,
                                     FALSE,
                                     FALSE,
                                     FALSE,
                                     DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS,
                                     DEFAULT_QUALITY,
                                     FIXED_PITCH | FF_MODERN,
                                     L"Consolas");

            list_ = CreateWindowExW(WS_EX_CLIENTEDGE,
                                    L"SysListView32",
                                    L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                    0,
                                    0,
                                    0,
                                    0,
                                    window_,
                                    nullptr,
                                    instance_,
                                    nullptr);
            ListView_SetExtendedListViewStyle(list_, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP);
            SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);

            row_images_ = ImageList_Create(1, kRowHeight, ILC_COLOR32, 0, 1);
            ListView_SetImageList(list_, row_images_, LVSIL_SMALL);

            for (int column = 0; column < kColumnCount; ++column)
            {
                LVCOLUMNW info {};
                info.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                info.fmt     = LVCFMT_LEFT;
                info.cx      = kColumnWidth[column];
                info.pszText = const_cast<wchar_t*>(kColumnTitles[column]);
                ListView_InsertColumn(list_, column, &info);
            }

            header_ = CreateWindowExW(0,
                                      L"STATIC",
                                      L"",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                      0,
                                      0,
                                      0,
                                      0,
                                      window_,
                                      nullptr,
                                      instance_,
                                      nullptr);
            SendMessageW(header_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);

            message_ = CreateWindowExW(0,
                                       L"STATIC",
                                       L"",
                                       WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
                                       0,
                                       0,
                                       0,
                                       0,
                                       window_,
                                       nullptr,
                                       instance_,
                                       nullptr);
            SendMessageW(message_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);

            SetWindowTextW(window_, widen(options_.title).c_str());
            layout();
            SetFocus(list_);
        }

        void WindowState::layout()
        {
            RECT client {};
            GetClientRect(window_, &client);
            const int width      = client.right - client.left;
            const int height     = client.bottom - client.top;
            const int listHeight = height > kStatusHeight ? height - kStatusHeight : 0;
            const int split      = width * 65 / 100;

            MoveWindow(list_, 0, 0, width, listHeight, TRUE);
            MoveWindow(header_, 0, listHeight, split, kStatusHeight, TRUE);
            MoveWindow(message_, split, listHeight, width - split, kStatusHeight, TRUE);
        }

        void
        WindowState::publish(std::shared_ptr<const std::vector<RowView>> rows, std::string header, std::string message)
        {
            {
                std::scoped_lock lock {mutex_};
                pending_      = std::move(rows);
                header_text_  = std::move(header);
                message_text_ = std::move(message);
            }
            PostMessageW(window_, kSnapshotMessage, 0, 0);
        }

        void WindowState::set_message(std::string message)
        {
            std::string text;
            {
                std::scoped_lock lock {mutex_};
                message_text_ = std::move(message);
                text          = message_text_;
            }
            SetWindowTextW(message_, widen(text).c_str());
        }

        void WindowState::apply_snapshot()
        {
            std::shared_ptr<const std::vector<RowView>> rows;
            std::string                                 header;
            std::string                                 message;
            {
                std::scoped_lock lock {mutex_};
                if (pending_)
                {
                    displayed_ = std::move(pending_);
                }
                rows    = displayed_;
                header  = header_text_;
                message = message_text_;
            }

            if (rows)
            {
                const int count = static_cast<int>(rows->size());
                if (ListView_GetItemCount(list_) != count)
                {
                    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
                    ListView_DeleteAllItems(list_);
                    for (int row = 0; row < count; ++row)
                    {
                        LVITEMW item {};
                        item.mask    = LVIF_TEXT;
                        item.iItem   = row;
                        item.pszText = const_cast<wchar_t*>(L"");
                        ListView_InsertItem(list_, &item);
                    }
                    for (int row = 0; row < count; ++row)
                    {
                        for (int column = 0; column < kColumnCount; ++column)
                        {
                            auto text = cell_text((*rows)[static_cast<std::size_t>(row)], column);
                            ListView_SetItemText(list_, row, column, text.data());
                        }
                    }
                    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
                    InvalidateRect(list_, nullptr, TRUE);
                }
                else
                {
                    for (int row = 0; row < count; ++row)
                    {
                        for (int column = 0; column < kColumnCount; ++column)
                        {
                            auto text = cell_text((*rows)[static_cast<std::size_t>(row)], column);
                            ListView_SetItemText(list_, row, column, text.data());
                        }
                    }
                }
            }

            SetWindowTextW(header_, widen(header).c_str());
            SetWindowTextW(message_, widen(message).c_str());
        }

        const RowView* WindowState::row_at(std::size_t row) const
        {
            if (!displayed_ || row >= displayed_->size())
            {
                return nullptr;
            }
            return &(*displayed_)[row];
        }

        bool WindowState::checked(const RowView& row)
        {
            return row.kind == RowKind::value ? row.frozen : row.active;
        }

        std::wstring WindowState::cell_text(const RowView& row, int column) const
        {
            switch (column)
            {
            case 0:
                return {};
            case 1:
                return std::wstring(static_cast<std::size_t>(row.depth) * 2, L' ') + widen(row.description);
            case 2:
                return widen(row.address);
            case 3:
                return widen(row.type);
            case 4:
                return widen(row.value);
            case 5:
                return widen(row.state);
            case 6:
                return widen(row.interval);
            case 7:
                return widen(row.message);
            default:
                return {};
            }
        }

        void WindowState::submit(Action action)
        {
            if (session_)
            {
                session_->submit(std::move(action));
            }
        }

        void WindowState::toggle(std::size_t row)
        {
            const auto* view = row_at(row);
            if (view == nullptr)
            {
                return;
            }

            Action action;
            action.index = view->number;
            if (view->kind == RowKind::value)
            {
                action.kind = ActionKind::toggle_freeze;
            }
            else
            {
                action.kind = view->active ? ActionKind::deactivate_script : ActionKind::activate_script;
            }
            submit(action);
        }

        void WindowState::begin_edit(std::size_t row)
        {
            const auto* view = row_at(row);
            if (view == nullptr || view->kind != RowKind::value)
            {
                return;
            }
            close_editor();

            RECT cell {};
            if (!ListView_GetSubItemRect(list_, static_cast<int>(row), kValueColumn, LVIR_BOUNDS, &cell))
            {
                return;
            }
            POINT corner {cell.left, cell.top};
            MapWindowPoints(list_, window_, &corner, 1);

            editor_ = CreateWindowExW(0,
                                      L"EDIT",
                                      widen(view->value).c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      corner.x,
                                      corner.y,
                                      cell.right - cell.left,
                                      cell.bottom - cell.top,
                                      window_,
                                      nullptr,
                                      instance_,
                                      nullptr);
            if (editor_ == nullptr)
            {
                return;
            }
            SendMessageW(editor_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_font_), TRUE);
            g_old_edit_proc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(editor_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_proc)));
            editing_  = true;
            edit_row_ = row;
            SetFocus(editor_);
            SendMessageW(editor_, EM_SETSEL, 0, -1);
        }

        void WindowState::apply_edit()
        {
            if (!editing_ || editor_ == nullptr)
            {
                return;
            }
            const int    length = GetWindowTextLengthW(editor_);
            std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
            GetWindowTextW(editor_, text.data(), length + 1);
            text.resize(static_cast<std::size_t>(length));

            const auto* view = row_at(edit_row_);
            if (view == nullptr || view->kind != RowKind::value || view->number == 0
                || view->number > payload_.value_count())
            {
                close_editor();
                return;
            }

            const auto reason = parse_value_text(*payload_.values()[view->number - 1], narrow(text));
            if (reason.has_value())
            {
                set_message(std::format("value refused: {}", *reason));
                SendMessageW(editor_, EM_SETSEL, 0, -1);
                return;
            }

            submit(Action {ActionKind::set_value, view->number, narrow(text)});
            close_editor();
        }

        void WindowState::close_editor()
        {
            if (!editing_ && editor_ == nullptr)
            {
                return;
            }
            editing_        = false;
            HWND editor     = editor_;
            editor_         = nullptr;
            g_old_edit_proc = nullptr;
            if (editor != nullptr)
            {
                SetWindowLongPtrW(editor, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DefWindowProcW));
                DestroyWindow(editor);
            }
            SetFocus(list_);
        }

        void WindowState::show_help()
        {
            MessageBoxW(window_, widen(console::render_help()).c_str(), L"Help", MB_OK | MB_ICONINFORMATION);
        }

        void WindowState::show_about()
        {
            const auto pid = pid_cache_.load();
            const auto target =
                pid == 0 ? std::string {"waiting for the target to appear"} : std::format("target pid {}", pid);
            const auto text = std::format("{}\n\n{} value entr{}, {} script{}\n{}\n{}-byte pointers",
                                          options_.title,
                                          payload_.value_count(),
                                          payload_.value_count() == 1 ? "y" : "ies",
                                          payload_.script_count(),
                                          payload_.script_count() == 1 ? "" : "s",
                                          target,
                                          payload_.pointer_size);
            MessageBoxW(window_, widen(text).c_str(), L"About", MB_OK | MB_ICONINFORMATION);
        }

        LRESULT handle_list_notify(const NMHDR& header, LPARAM lparam)
        {
            if (header.code == NM_CUSTOMDRAW)
            {
                auto* draw = reinterpret_cast<LPNMLVCUSTOMDRAW>(lparam);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                {
                    return CDRF_NOTIFYITEMDRAW;
                }
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                {
                    return CDRF_NOTIFYSUBITEMDRAW;
                }
                if ((draw->nmcd.dwDrawStage & CDDS_ITEMPREPAINT) != 0 && (draw->nmcd.dwDrawStage & CDDS_SUBITEM) != 0
                    && g_state != nullptr)
                {
                    const auto  row  = static_cast<std::size_t>(draw->nmcd.dwItemSpec);
                    const auto* view = g_state->row_at(row);
                    if (view == nullptr)
                    {
                        return CDRF_DODEFAULT;
                    }

                    HDC  dc = draw->nmcd.hdc;
                    RECT cell {};
                    ListView_GetSubItemRect(g_state->list_,
                                            static_cast<int>(row),
                                            draw->iSubItem,
                                            draw->iSubItem == 0 ? LVIR_LABEL : LVIR_BOUNDS,
                                            &cell);

                    const bool selected =
                        (ListView_GetItemState(g_state->list_, static_cast<int>(row), LVIS_SELECTED) & LVIS_SELECTED)
                        != 0;
                    FillRect(dc, &cell, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));

                    if (draw->iSubItem == kCheckColumn)
                    {
                        RECT      box  = cell;
                        const int size = 14;
                        box.left += (cell.right - cell.left - size) / 2;
                        box.right = box.left + size;
                        box.top += (cell.bottom - cell.top - size) / 2;
                        box.bottom = box.top + size;
                        DrawFrameControl(
                            dc, &box, DFC_BUTTON, DFCS_BUTTONCHECK | (WindowState::checked(*view) ? DFCS_CHECKED : 0));
                    }
                    else
                    {
                        auto       text      = g_state->cell_text(*view, draw->iSubItem);
                        auto       font      = kMonoColumn[draw->iSubItem] ? g_state->mono_font_ : g_state->ui_font_;
                        const auto previous  = static_cast<HFONT>(SelectObject(dc, font));
                        RECT       text_rect = cell;
                        text_rect.left += 4;
                        text_rect.right -= 4;
                        DrawTextW(dc,
                                  text.c_str(),
                                  static_cast<int>(text.size()),
                                  &text_rect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                        SelectObject(dc, previous);
                    }
                    return CDRF_SKIPDEFAULT;
                }
            }

            if (header.code == LVN_GETINFOTIP)
            {
                auto* info = reinterpret_cast<LPNMLVGETINFOTIPW>(lparam);
                if (info == nullptr || info->iItem < 0 || g_state == nullptr)
                {
                    return 0;
                }
                const auto* view = g_state->row_at(static_cast<std::size_t>(info->iItem));
                if (view == nullptr)
                {
                    return 0;
                }
                const auto text = view->kind == RowKind::value ? std::format("entry {}", view->number)
                                                               : std::format("script S{}", view->number);
                const auto wide = widen(text);
                lstrcpynW(info->pszText, wide.c_str(), info->cchTextMax);
                return 0;
            }

            if (header.code == NM_CLICK || header.code == NM_DBLCLK)
            {
                auto*         click = reinterpret_cast<LPNMITEMACTIVATE>(lparam);
                LVHITTESTINFO hit {};
                hit.pt = click->ptAction;
                if (ListView_SubItemHitTest(g_state->list_, &hit) < 0)
                {
                    return 0;
                }
                if (hit.iSubItem == kCheckColumn && header.code == NM_CLICK)
                {
                    g_state->toggle(static_cast<std::size_t>(hit.iItem));
                }
                else if (header.code == NM_DBLCLK && hit.iSubItem == kValueColumn)
                {
                    g_state->begin_edit(static_cast<std::size_t>(hit.iItem));
                }
            }
            return 0;
        }

        LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
        {
            auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

            switch (message)
            {
            case WM_CREATE:
            {
                auto* created =
                    reinterpret_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
                created->window_ = window;
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
                g_state            = created;
                created->instance_ = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
                created->create_children();
                return 0;
            }
            case WM_SIZE:
                if (state != nullptr)
                {
                    state->close_editor();
                    state->layout();
                }
                return 0;
            case WM_COMMAND:
                if (state != nullptr)
                {
                    switch (LOWORD(wparam))
                    {
                    case kRefreshCommand:
                        state->submit(Action {ActionKind::refresh, 0, {}});
                        return 0;
                    case kCloseCommand:
                        PostMessageW(window, WM_CLOSE, 0, 0);
                        return 0;
                    case kHelpCommand:
                        state->show_help();
                        return 0;
                    case kAboutCommand:
                        state->show_about();
                        return 0;
                    default:
                        break;
                    }
                }
                break;
            case WM_NOTIFY:
                if (state != nullptr)
                {
                    return handle_list_notify(*reinterpret_cast<NMHDR*>(lparam), lparam);
                }
                break;
            case kSnapshotMessage:
                if (state != nullptr)
                {
                    state->apply_snapshot();
                }
                return 0;
            case kCloseEditorMessage:
                if (state != nullptr)
                {
                    state->close_editor();
                }
                return 0;
            case WM_CLOSE:
                if (state != nullptr)
                {
                    state->close_editor();
                }
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                break;
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    bool window_available()
    {
        return true;
    }

    int run(const Payload&               payload,
            std::span<const LiveEntry>   live,
            std::span<const ScriptState> scripts,
            Pid                          pid,
            Options                      options,
            Perform                      perform,
            Step                         step)
    {
        INITCOMMONCONTROLSEX common {sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&common);

        auto* instance = GetModuleHandleW(nullptr);

        HMENU menu = CreateMenu();
        HMENU file = CreatePopupMenu();
        HMENU help = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, kRefreshCommand, L"&Refresh now\tF5");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, kCloseCommand, L"&Close\tCtrl+W");
        AppendMenuW(help, MF_STRING, kHelpCommand, L"&Help\tF1");
        AppendMenuW(help, MF_STRING, kAboutCommand, L"&About");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");

        ACCEL accelerators[] {
            {           FVIRTKEY, VK_F5, kRefreshCommand},
            {           FVIRTKEY, VK_F1,    kHelpCommand},
            {FVIRTKEY | FCONTROL,   'W',   kCloseCommand},
        };
        HACCEL accelerator = CreateAcceleratorTableW(accelerators, 3);

        const wchar_t* class_name = L"SlopkitTrainerWindow";
        WNDCLASSEXW    window_class {};
        window_class.cbSize        = sizeof(WNDCLASSEXW);
        window_class.style         = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc   = window_proc;
        window_class.hInstance     = instance;
        window_class.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        window_class.lpszClassName = class_name;
        RegisterClassExW(&window_class);

        WindowState state {
            payload, live, scripts, std::move(pid), std::move(options), std::move(perform), std::move(step)};

        const auto title  = widen(state.options_.title);
        HWND       window = CreateWindowExW(0,
                                            class_name,
                                            title.c_str(),
                                            WS_OVERLAPPEDWINDOW,
                                            CW_USEDEFAULT,
                                            CW_USEDEFAULT,
                                            748,
                                            560,
                                            nullptr,
                                            menu,
                                            instance,
                                            &state);
        if (window == nullptr)
        {
            return 1;
        }

        state.session_ =
            std::make_unique<SessionThread>(state, state.perform_, state.step_, state.options_.interval_ms);
        state.session_->start();

        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);

        MSG message {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (accelerator == nullptr || !TranslateAcceleratorW(window, accelerator, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        state.session_.reset();
        window_class.hbrBackground = nullptr;
        DestroyAcceleratorTable(accelerator);
        DestroyMenu(menu);
        return state.exit_code_;
    }
}
