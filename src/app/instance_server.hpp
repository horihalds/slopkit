#pragma once

#include <map>
#include <memory>
#include <string>

#include <QObject>
#include <QString>

class QSocketNotifier;

namespace slopkit::app
{
    // Owns the per-user abstract-socket listener and turns each hand-off into a
    // tableRequested() signal on the UI thread. A false is_listening() means the
    // name is already taken: another instance is listening.
    class InstanceServer : public QObject
    {
        Q_OBJECT

    public:
        explicit InstanceServer(QObject* parent = nullptr);
        explicit InstanceServer(std::string socket_name, QObject* parent = nullptr);
        ~InstanceServer() override;
        InstanceServer(const InstanceServer&)            = delete;
        InstanceServer& operator=(const InstanceServer&) = delete;

        [[nodiscard]] bool is_listening() const noexcept;

    signals:
        void tableRequested(const QString& path);

    private:
        struct Connection
        {
            int              fd {-1};
            std::string      buffer;
            QSocketNotifier* notifier {};
        };

        void on_ready_read();
        void on_connection_ready(int fd);
        void close_connection(int fd);

        int                                        listen_fd_ {-1};
        QSocketNotifier*                           listen_notifier_ {};
        std::map<int, std::unique_ptr<Connection>> connections_;
    };

} // namespace slopkit::app
