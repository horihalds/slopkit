#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "ui/address_format.hpp"
#include "ui/components/memory_view_document.hpp"
#include "ui/live_values.hpp"

#include <QDialog>
#include <QString>

class QLabel;

namespace slopkit::ui::widgets
{
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::components
{
    class MemoryView;
} // namespace slopkit::ui::components

namespace slopkit::ui::dialogs
{

    // A hex-editor style live view of the attached target's memory: the
    // ui::components::MemoryView filling the window behind a small status line.
    // Reads and writes run on the access worker and the viewer renders its
    // cached blocks, never touching a session.
    class MemoryViewerDialog : public QDialog, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        MemoryViewerDialog(process::AccessWorker& worker, process::AttachedTarget& target, QWidget* parent = nullptr);

        // Opens the viewer at `address`.
        void set_address(std::uint64_t address);

        // Jumps to an address written as absolute ("0x1040") or module+RVA
        // ("libc.so.6+1A2B"); false when the text does not name an address.
        bool go_to(const QString& text);

        // Sets the module image spans used to render module-relative addresses.
        void set_modules(std::vector<process::ModuleInfo> modules);
        // Chooses how static addresses are shown in the address column and Go To.
        void set_address_mode(ui::AddressMode mode);

        // LiveSurface: the visible window while the dialog is shown and a target
        // is attached; the coordinator submits it with the other surfaces' reads.
        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

    signals:
        // Asks the window's live coordinator for an immediate pass (Go To / show).
        void liveRefreshRequested();

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void build_layout();
        // Asks for an address text and jumps to it; a bad value is only logged.
        void prompt_go_to();
        // Asks the coordinator for an immediate pass and shows the loading state.
        void request_page();
        void update_state();

        process::AttachedTarget&       target_;
        components::MemoryViewDocument document_;
        components::MemoryView*        view_ {};
        QLabel*                        loading_label_ {};
        widgets::StatusLabel*          status_ {};

        // The manual-request/loading state; the page cache lives in the document.
        bool page_loaded_ {false};
        bool manual_request_ {false};
    };

} // namespace slopkit::ui::dialogs
