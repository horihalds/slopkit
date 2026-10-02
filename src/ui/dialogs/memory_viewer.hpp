#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"

#include <QAbstractTableModel>
#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTimer;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // Address / Hex / ASCII rows of one 16-byte-per-row page.
    class MemoryDumpModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        // Rows of 16 bytes shown per page.
        static constexpr std::size_t kRowBytes = 16;
        static constexpr std::size_t kRows     = 24;

        enum Column
        {
            address,
            hex,
            ascii,
            column_count,
        };

        explicit MemoryDumpModel(QObject* parent = nullptr);

        [[nodiscard]] int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

        // Replaces the page; rows past the returned bytes are unreadable.
        void set_page(std::uint64_t base, std::vector<std::byte> bytes);
        // Marks every row unreadable (no page loaded yet).
        void clear();

        [[nodiscard]] bool any_unreadable() const noexcept;

    private:
        std::uint64_t           base_ {0};
        std::vector<std::byte>  bytes_;
        std::array<bool, kRows> unreadable_ {};
    };

    // A hex dump of the attached target's memory. Reads run on the access worker
    // and the viewer renders one cached page, never touching a session. The
    // disassembler pane is out of scope, so this ships the byte and ASCII view.
    class MemoryViewerDialog : public QDialog
    {
        Q_OBJECT

    public:
        MemoryViewerDialog(process::AccessWorker& worker, process::AttachedTarget& target, QWidget* parent = nullptr);

        // Opens the viewer at `address`, aligned down to a row boundary.
        void set_address(std::uint64_t address);

    protected:
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;

    private:
        void build_layout();
        void go_to_address();
        void previous_page();
        void next_page();
        // Submits one page read unless a request is already in flight.
        void request_page();
        void update_state();

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;

        std::uint64_t         base_ {};
        QLineEdit*            address_edit_ {};
        QPushButton*          go_button_ {};
        QPushButton*          previous_button_ {};
        QPushButton*          next_button_ {};
        QPushButton*          refresh_button_ {};
        QLabel*               loading_label_ {};
        QTableView*           dump_view_ {};
        MemoryDumpModel*      dump_model_ {};
        widgets::StatusLabel* status_ {};
        QTimer*               refresh_timer_ {};

        std::optional<process::JobId> pending_;
        std::uint64_t                 requested_base_ {0};
        process::ProcessId            requested_pid_ {0};
        bool                          refresh_requested_ {false};
        bool                          ever_requested_ {false};
    };

} // namespace slopkit::ui::dialogs
