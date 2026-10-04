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

    // The practice target's window: a live view over `values()` and the heap
    // marker. Every row reads its memory location on each refresh, so a write
    // made from slopkit shows up within a tick instead of being a private copy.
    class SandboxWindow : public QWidget
    {
        Q_OBJECT

    public:
        explicit SandboxWindow(QWidget* parent = nullptr);

        // Re-reads the live values into the rows. The ~100 ms refresh timer
        // calls it; the tests call it directly so a write can be observed
        // without waiting.
        void refresh();

        // Pauses or resumes the ~500 ms animation and keeps the checkbox in
        // step.
        void               set_paused(bool paused);
        [[nodiscard]] bool is_paused() const noexcept;

    private:
        // One value cell: its editor, how to read the live value into it and how
        // to parse an edited one back. `write` is empty for the read-only hex
        // rows.
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

} // namespace slopkit::sandbox
