#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <QMainWindow>
#include <QString>

#include "trainer/payload.hpp"
#include "trainer/runtime/action.hpp"
#include "trainer/runtime/gui/session_worker.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/rows.hpp"
#include "trainer/runtime/scripts.hpp"

class QCloseEvent;
class QLabel;
class QTableView;

namespace slopkit::trainer::gui
{
    class RowsModel;

    using Pid = std::function<std::uint32_t()>;

    struct Options
    {
        QString       title;
        std::uint32_t interval_ms {250};
    };

    // A display this platform can actually open; main.cpp uses it to choose
    // between the window and the printed rows.
    [[nodiscard]] bool window_available();

    // The trainer's window: one table over the value and script rows, the status
    // bar and the menus. It owns the worker thread and never touches a Target.
    class TrainerWindow : public QMainWindow
    {
        Q_OBJECT

    public:
        TrainerWindow(const Payload&               payload,
                      std::span<const LiveEntry>   live,
                      std::span<const ScriptState> scripts,
                      Pid                          pid,
                      Options                      options,
                      Perform                      perform,
                      Step                         step,
                      QWidget*                     parent = nullptr);

        ~TrainerWindow() override;

        [[nodiscard]] RowsModel* rows_model() const noexcept;
        [[nodiscard]] int        exit_code() const noexcept;

    protected:
        void closeEvent(QCloseEvent* event) override;

    private:
        void show_snapshot(const std::shared_ptr<const std::vector<RowView>>& rows,
                           const QString&                                     header,
                           const QString&                                     message);
        void show_help();
        void show_about();
        void stop_session();

        Options                      options_;
        const Payload&               payload_;
        Pid                          pid_;
        std::span<const LiveEntry>   live_;
        std::span<const ScriptState> scripts_;
        RowsModel*                   model_ {};
        QTableView*                  table_ {};
        QLabel*                      header_label_ {};
        QLabel*                      message_label_ {};
        SessionWorker*               worker_ {};
        int                          exit_code_ {};
    };

    // Builds the window, starts the session and runs the Qt event loop on the
    // calling thread; returns the process exit code.
    [[nodiscard]] int run(const Payload&               payload,
                          std::span<const LiveEntry>   live,
                          std::span<const ScriptState> scripts,
                          Pid                          pid,
                          Options                      options,
                          Perform                      perform,
                          Step                         step);
}
