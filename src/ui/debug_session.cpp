#include "ui/debug_session.hpp"

#include <utility>

#include <QTimer>

#include "ui/components/message_box.hpp"

namespace slopkit::ui
{
    namespace
    {
        // "<name> (<pid>)", the identity the prompt names.
        QString target_label(const process::AttachedTarget& target)
        {
            return QStringLiteral("%1 (%2)").arg(QString::fromStdString(target.name), QString::number(target.pid));
        }

        // The production permission question: themed, question icon, Yes/No and
        // No as the default, so Enter or Esc never starts the tracer.
        bool ask_attach(QWidget& owner, const process::AttachedTarget& target, const QString& reason)
        {
            widgets::MessageBox box(&owner);
            box.set_icon(widgets::MessageBoxIcon::question);
            box.set_title(QStringLiteral("Attach the debugger?"));
            box.set_text(QStringLiteral("Attach the debugger to %1 to %2?").arg(target_label(target), reason));
            box.set_informative_text(
                QStringLiteral("The process will see the tracer (TracerPid in /proc/%1/status). Stop Debugging in "
                               "the Memory Viewer ends the session.")
                    .arg(target.pid));
            box.set_buttons(widgets::MessageBoxButton::yes | widgets::MessageBoxButton::no);
            box.set_default_button(widgets::MessageBoxButton::no);
            box.exec();
            return box.result() == widgets::MessageBoxResult::yes;
        }
    } // namespace

    DebugSessionGate::DebugSessionGate(debug::Controller&       controller,
                                       process::AttachedTarget& target,
                                       QWidget&                 owner,
                                       QObject*                 parent)
        : QObject(parent), controller_(controller), target_(target), owner_(owner),
          prompt_(
              [&owner](const process::AttachedTarget& target, const QString& reason)
              {
                  return ask_attach(owner, target, reason);
              })
    {
        connect(&controller_, &debug::Controller::stateChanged, this, &DebugSessionGate::on_state_changed);
    }

    void DebugSessionGate::set_attach_prompt(AttachPrompt prompt)
    {
        prompt_ = std::move(prompt);
    }

    bool DebugSessionGate::session_live() const noexcept
    {
        return controller_.state() != debug::Controller::State::idle;
    }

    void DebugSessionGate::with_session(const QString& reason, std::function<void()> ready)
    {
        if (!target_.valid())
        {
            emit progress(QStringLiteral("Attach to a target before %1.").arg(reason), true);
            emit abandoned();
            return;
        }

        switch (controller_.state())
        {
        case debug::Controller::State::running:
        case debug::Controller::State::stopped:
            // A session is already up: nothing to attach, nothing to ask.
            ready();
            return;
        case debug::Controller::State::starting:
            if (!starting_)
            {
                // The Debugger pane (or another window's gate) is already
                // attaching; this gate did not start it and cannot wait for it
                // without stealing its completion.
                emit progress(
                    QStringLiteral("An attach is already under way; repeat the command once the session is up."), true);
                emit abandoned();
                return;
            }
            ready_.push_back(std::move(ready));
            return;
        case debug::Controller::State::idle:
            break;
        }

        if (!prompt_(target_, reason))
        {
            emit progress(QStringLiteral("Attach cancelled; %1 was not started.").arg(reason), false);
            emit abandoned();
            return;
        }

        starting_ = true;
        emit progress(QStringLiteral("Attaching the debugger to %1\u2026").arg(QString::fromStdString(target_.label())),
                      false);
        ready_.push_back(std::move(ready));
        controller_.start(target_.pid, target_.plugin_id);
    }

    void DebugSessionGate::on_state_changed()
    {
        if (controller_.state() == debug::Controller::State::running)
        {
            if (!starting_ && ready_.empty())
            {
                return;
            }
            starting_ = false;
            emit progress(QStringLiteral("Debugger attached to %1.").arg(QString::fromStdString(target_.label())),
                          false);
            dispatch_ready();
            return;
        }

        if (controller_.state() == debug::Controller::State::idle && (starting_ || !ready_.empty()))
        {
            fail_attach();
        }
    }

    void DebugSessionGate::dispatch_ready()
    {
        std::vector<std::function<void()>> queued = std::move(ready_);
        ready_.clear();
        for (std::function<void()>& callback : queued)
        {
            // Deferred one turn, so the work cannot race the resume that
            // begin_run() issues right after its stateChanged() emission.
            QTimer::singleShot(0,
                               this,
                               [callback = std::move(callback)]()
                               {
                                   callback();
                               });
        }
    }

    void DebugSessionGate::fail_attach()
    {
        starting_ = false;
        ready_.clear();
        emit progress(QStringLiteral("The debugger could not attach (see the log)."), true);
        emit abandoned();
    }

} // namespace slopkit::ui
