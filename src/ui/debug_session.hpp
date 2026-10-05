#pragma once

#include <functional>
#include <vector>

#include <QObject>
#include <QString>

#include "debug/controller.hpp"
#include "process/attachment.hpp"

class QWidget;

namespace slopkit::ui
{

    // The permission + attach + wait step shared by every command that needs a
    // debug session but may be invoked with none: it asks the user, starts the
    // session through the controller and hands control back once it runs.
    class DebugSessionGate : public QObject
    {
        Q_OBJECT

    public:
        // The permission question: true means the attach may happen. Production
        // uses the themed MessageBox; a test injects an answer so no modal
        // dialog is ever shown.
        using AttachPrompt = std::function<bool(const process::AttachedTarget& target, const QString& reason)>;

        DebugSessionGate(debug::Controller&       controller,
                         process::AttachedTarget& target,
                         QWidget&                 owner,
                         QObject*                 parent = nullptr);

        // Replaces the permission question, the way MainWindow replaces its
        // table conflict prompt.
        void set_attach_prompt(AttachPrompt prompt);

        // True while any session exists, i.e. no attach is needed.
        [[nodiscard]] bool session_live() const noexcept;

        // The whole gate: prompt when the session is dead, attach, wait for it
        // to run, then invoke `ready`. `ready` is not invoked when the prompt is
        // declined, the attach fails, or no target is attached.
        void with_session(const QString& reason, std::function<void()> ready);

    signals:
        // One line for the status area of the window that started the command.
        void progress(const QString& text, bool error);
        // Emitted when with_session() gave up without ever handing control back
        // (declined prompt, failed attach, no target, an attach started
        // elsewhere); the queue has already been dropped.
        void abandoned();

    private:
        void on_state_changed();
        void dispatch_ready();
        void fail_attach();

        debug::Controller&                 controller_;
        process::AttachedTarget&           target_;
        QWidget&                           owner_;
        AttachPrompt                       prompt_;
        std::vector<std::function<void()>> ready_;
        bool                               starting_ {false};
    };

} // namespace slopkit::ui
