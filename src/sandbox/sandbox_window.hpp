#pragma once

#include <functional>
#include <vector>

#include <QLineEdit>
#include <QWidget>

class QCheckBox;
class QGridLayout;
class QLabel;
class QTimer;

namespace slopkit::sandbox
{
    class SandboxWindow : public QWidget
    {
        Q_OBJECT

    public:
        explicit SandboxWindow(QWidget* parent = nullptr);

        void refresh();

        void               set_paused(bool paused);
        [[nodiscard]] bool is_paused() const noexcept;

    private:
        struct Row
        {
            QLineEdit*                          edit {nullptr};
            std::function<QString()>            read;
            std::function<void(const QString&)> write;
        };

        void build_body();
        void animate();
        void reset_values();

        QLineEdit* add_row(QGridLayout*                        grid,
                           int                                 row,
                           const QString&                      name,
                           const QString&                      type_name,
                           std::function<QString()>            read,
                           std::function<void(const QString&)> write,
                           bool                                read_only = false);

        std::vector<Row> rows_;
        QLabel*          heap_marker_address_ {nullptr};
        QCheckBox*       pause_check_ {nullptr};
        QTimer*          refresh_timer_ {nullptr};
        QTimer*          animation_timer_ {nullptr};
        bool             paused_ {false};
    };

}
