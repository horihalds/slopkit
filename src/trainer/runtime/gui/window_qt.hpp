#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <QMainWindow>
#include <QString>

#include "trainer/payload.hpp"
#include "trainer/runtime/gui/window.hpp"
#include "trainer/runtime/resolve.hpp"
#include "trainer/runtime/rows.hpp"
#include "trainer/runtime/scripts.hpp"

class QCloseEvent;
class QLabel;
class QTableView;

namespace slopkit::trainer::gui
{
    class LogFeed;
    class LogPane;
    class RowsModel;
    class SessionWorker;

    // The Linux trainer's window: one table over the value and script rows, the
    // status bar and the menus. It owns the worker thread and never touches a
    // Target.
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
        void drain_log();

        Options                      options_;
        const Payload&               payload_;
        Pid                          pid_;
        std::span<const LiveEntry>   live_;
        std::span<const ScriptState> scripts_;
        RowsModel*                   model_ {};
        QTableView*                  table_ {};
        LogPane*                     log_pane_ {};
        QLabel*                      header_label_ {};
        QLabel*                      message_label_ {};
        SessionWorker*               worker_ {};
        std::unique_ptr<LogFeed>     feed_;
        std::atomic<bool>            wake_queued_ {false};
        std::atomic<std::uint32_t>   pid_cache_ {0};
        int                          exit_code_ {};
    };
}
