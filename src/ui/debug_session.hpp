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
    class DebugSessionGate : public QObject
    {
        Q_OBJECT

    public:
        using AttachPrompt = std::function<bool(const process::AttachedTarget& target, const QString& reason)>;

        DebugSessionGate(debug::Controller&       controller,
                         process::AttachedTarget& target,
                         QWidget&                 owner,
                         QObject*                 parent = nullptr);

        void set_attach_prompt(AttachPrompt prompt);

        [[nodiscard]] bool session_live() const noexcept;

        void with_session(const QString& reason, std::function<void()> ready);

    signals:
        void progress(const QString& text, bool error);
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

}
