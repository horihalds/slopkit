#include "trainer/runtime/gui/window_qt.hpp"

#include <memory>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTableView>
#include <QWidget>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "trainer/runtime/console.hpp"
#include "trainer/runtime/gui/rows_model.hpp"
#include "trainer/runtime/gui/session_worker.hpp"
#include "ui/components/message_box.hpp"
#include "ui/components/validating_delegate.hpp"
#include "ui/fonts.hpp"
#include "ui/platform.hpp"
#include "ui/settings.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

namespace slopkit::trainer::gui
{
    bool window_available()
    {
#if defined(Q_OS_WIN)
        return true;
#else
        // A platform the caller chose explicitly (the offscreen run in the test
        // suite, mostly) counts as available; otherwise a display must exist.
        if (!qgetenv("QT_QPA_PLATFORM").isEmpty())
        {
            return true;
        }
        return !qgetenv("DISPLAY").isEmpty() || !qgetenv("WAYLAND_DISPLAY").isEmpty();
#endif
    }

    TrainerWindow::TrainerWindow(const Payload&               payload,
                                 std::span<const LiveEntry>   live,
                                 std::span<const ScriptState> scripts,
                                 Pid                          pid,
                                 Options                      options,
                                 Perform                      perform,
                                 Step                         step,
                                 QWidget*                     parent)
        : QMainWindow(parent), options_(std::move(options)), payload_(payload), pid_(std::move(pid)), live_(live),
          scripts_(scripts)
    {
        setWindowTitle(QString::fromStdString(options_.title));
        resize(1024, 480);

        model_ = new RowsModel(payload_, this);

        table_ = new QTableView(this);
        table_->setModel(model_);
        table_->setItemDelegateForColumn(RowsModel::value, new ui::components::ValidatingDelegate(table_));
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setWordWrap(false);
        table_->setAlternatingRowColors(true);
        table_->verticalHeader()->hide();
        table_->verticalHeader()->setDefaultSectionSize(28);

        auto* columns = table_->horizontalHeader();
        columns->setSectionResizeMode(QHeaderView::ResizeToContents);
        columns->setSectionResizeMode(RowsModel::description, QHeaderView::Interactive);
        columns->setStretchLastSection(true);
        setCentralWidget(table_);

        header_label_ = new QLabel(this);
        header_label_->setFont(ui::ui_font());
        message_label_ = new QLabel(this);
        message_label_->setFont(ui::ui_font());
        message_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusBar()->addWidget(header_label_, 1);
        statusBar()->addPermanentWidget(message_label_);

        auto* file_menu = menuBar()->addMenu(tr("&File"));
        auto* refresh   = file_menu->addAction(tr("&Refresh now"));
        refresh->setShortcut(QKeySequence::Refresh);
        connect(refresh,
                &QAction::triggered,
                this,
                [this]
                {
                    worker_->submit(Action {ActionKind::refresh, 0, {}});
                });
        file_menu->addSeparator();
        auto* close = file_menu->addAction(tr("&Close"));
        close->setShortcut(QKeySequence::Close);
        connect(close, &QAction::triggered, this, &QWidget::close);

        auto* help_menu = menuBar()->addMenu(tr("&Help"));
        auto* help      = help_menu->addAction(tr("&Help"));
        help->setShortcut(QKeySequence::HelpContents);
        connect(help, &QAction::triggered, this, &TrainerWindow::show_help);
        auto* about = help_menu->addAction(tr("&About"));
        connect(about, &QAction::triggered, this, &TrainerWindow::show_about);

        worker_ = new SessionWorker(
            std::move(perform),
            std::move(step),
            [this]
            {
                return std::make_shared<const std::vector<RowView>>(build_rows(payload_, live_, scripts_));
            },
            [this]
            {
                const auto pid = pid_();
                pid_cache_.store(pid);
                return ui::to_qstring(console::render_header(payload_, pid));
            },
            options_.interval_ms,
            this);

        connect(model_,
                &RowsModel::submitRequested,
                this,
                [this](const Action& action)
                {
                    worker_->submit(action);
                });
        connect(model_,
                &RowsModel::statusChanged,
                this,
                [this](const QString& text)
                {
                    message_label_->setText(text);
                });
        connect(worker_, &SessionWorker::rowsChanged, this, &TrainerWindow::show_snapshot);

        worker_->start();
    }

    TrainerWindow::~TrainerWindow()
    {
        stop_session();
    }

    RowsModel* TrainerWindow::rows_model() const noexcept
    {
        return model_;
    }

    int TrainerWindow::exit_code() const noexcept
    {
        return exit_code_;
    }

    void TrainerWindow::closeEvent(QCloseEvent* event)
    {
        stop_session();
        QMainWindow::closeEvent(event);
    }

    void TrainerWindow::show_snapshot(const std::shared_ptr<const std::vector<RowView>>& rows,
                                      const QString&                                     header,
                                      const QString&                                     message)
    {
        model_->set_rows(rows);
        header_label_->setText(header.trimmed());
        message_label_->setText(message);
    }

    void TrainerWindow::show_help()
    {
        ui::widgets::information(this, tr("Help"), QString::fromStdString(console::render_help()));
    }

    void TrainerWindow::show_about()
    {
        const auto pid    = pid_cache_.load();
        const auto target = pid == 0 ? tr("waiting for the target to appear") : tr("target pid %1").arg(pid);
        const auto values = payload_.entries.size();
        const auto count  = payload_.scripts.size();
        ui::widgets::information(this,
                                 tr("About"),
                                 tr("%1\n\n%2 value entr%3, %4 script%5\n%6\n%7-byte pointers")
                                     .arg(QString::fromStdString(options_.title))
                                     .arg(values)
                                     .arg(values == 1 ? tr("y") : tr("ies"))
                                     .arg(count)
                                     .arg(count == 1 ? QString {} : QStringLiteral("s"))
                                     .arg(target)
                                     .arg(payload_.pointer_size));
    }

    void TrainerWindow::stop_session()
    {
        if (worker_ != nullptr)
        {
            worker_->stop();
        }
    }

    int run(const Payload&               payload,
            std::span<const LiveEntry>   live,
            std::span<const ScriptState> scripts,
            Pid                          pid,
            Options                      options,
            Perform                      perform,
            Step                         step)
    {
        std::unique_ptr<QApplication> application;
        if (QCoreApplication::instance() == nullptr)
        {
            ui::prefer_platform_with_remembered_positions();

            // QApplication keeps these alive for the whole run, so they must
            // outlive it; the trainer takes no Qt options of its own.
            static int   argc   = 1;
            static char  name[] = "slopkit-trainer";
            static char* argv[] = {name, nullptr};

            application = std::make_unique<QApplication>(argc, argv);
            QApplication::setApplicationName(QStringLiteral("slopkit-trainer"));
            QApplication::setApplicationDisplayName(QString::fromStdString(options.title));
            QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

            if (!ui::register_embedded_fonts())
            {
                log::warning(log::category::ui, "the embedded fonts could not be loaded");
            }

            const ui::SettingsController settings;
            ui::apply_theme(settings.values().dark_theme ? ui::dark_theme() : ui::light_theme());
        }

        TrainerWindow window(
            payload, live, scripts, std::move(pid), std::move(options), std::move(perform), std::move(step));
        window.show();

        QApplication::exec();
        return window.exit_code();
    }

}
