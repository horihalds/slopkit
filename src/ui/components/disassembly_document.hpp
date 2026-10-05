#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "disasm/decoder.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "ui/address_format.hpp"
#include "ui/live_values.hpp"

#include <QObject>
#include <QString>

namespace slopkit::ui::components
{

    // The live state behind the disassembly listing: one aligned code window and
    // a lazily grown cache of the instructions decoded from it. It owns no
    // widget, so the risky part is testable headlessly; DisassemblyView only
    // paints it. The window is the scroll boundary: the cursor may move inside
    // it, and a new address elsewhere asks for a new window.
    class DisassemblyDocument : public QObject, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        static constexpr std::size_t kWindowSize  = 8192;   // bytes per live window
        static constexpr std::size_t kDecodeSlack = 32;     // rows decoded past the viewport
        static constexpr std::size_t kIdBase      = 0x1000; // this pane's LiveRequest id

        DisassemblyDocument(process::AccessWorker& worker, process::AttachedTarget& target, QObject* parent = nullptr);

        // The cursor the listing starts at and how many rows fit.
        void                        set_view(std::uint64_t first_address, std::size_t visible_rows);
        [[nodiscard]] std::uint64_t first_address() const noexcept;
        [[nodiscard]] std::size_t   visible_rows() const noexcept;
        [[nodiscard]] std::uint64_t window_base() const noexcept; // aligned live-window start

        // The live window is only requested while the view is on screen.
        void               set_visible(bool visible);
        [[nodiscard]] bool visible() const noexcept;

        void                                 set_modules(std::vector<process::ModuleInfo> modules);
        void                                 set_address_mode(ui::AddressMode mode);
        void                                 set_machine_mode(disasm::MachineMode mode);
        [[nodiscard]] disasm::MachineMode    machine_mode() const noexcept;
        [[nodiscard]] QString                address_text(std::uint64_t address) const;
        [[nodiscard]] const ui::ModuleSpans& module_spans() const; // Go To validation

        // LiveSurface: one aligned window while visible with a target attached.
        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

        // Decodes forward until at least `minimum` rows exist (or the window
        // ends); returns how many rows are decoded now. The scroll ceiling.
        std::size_t               ensure_rows(std::size_t minimum);
        [[nodiscard]] std::size_t row_count() const noexcept;
        [[nodiscard]] std::size_t bytes_width() const noexcept; // widest byte text, in characters

        struct Row
        {
            std::uint64_t address {};
            std::size_t   length {};
            QString       bytes; // "48 89 E5", or "??" when unreadable
            QString       text;  // "MOV RBP, RSP", or empty
            bool          readable {};
        };

        [[nodiscard]] Row                        row(std::size_t index) const;
        [[nodiscard]] std::optional<std::size_t> row_at(std::uint64_t address) const;

    signals:
        void rowsChanged(); // new bytes, a new window or a longer decode

    private:
        // One window slot submitted by the pass in flight.
        struct PendingWindow
        {
            std::uint64_t base {};
            std::uint64_t size {};
        };

        [[nodiscard]] std::uint64_t window_base_for(std::uint64_t address) const noexcept;
        void                        reset_decode();
        [[nodiscard]] QString       instruction_bytes(const disasm::Instruction& instruction) const;

        process::AttachedTarget& target_;

        std::uint64_t       first_address_ {0};
        std::size_t         visible_rows_ {1};
        bool                visible_ {true};
        disasm::MachineMode machine_mode_ {disasm::MachineMode::long_64};

        ui::ModuleSpans module_spans_;
        ui::AddressMode address_mode_ {ui::AddressMode::module_relative};

        // The window submitted in the pass in flight plus the pid it belonged
        // to; reset whenever the window moves, so a stale reading is dropped.
        std::optional<std::uint64_t> last_window_base_;
        std::optional<PendingWindow> pending_;
        process::ProcessId           requested_pid_ {0};

        // The decoded instruction stream, kept while the window's bytes are
        // unchanged so an idle pass never re-decodes.
        std::vector<std::byte>           bytes_;
        bool                             readable_ {false};
        std::vector<disasm::Instruction> instructions_;
        std::size_t                      decoded_offset_ {0};
        bool                             decode_exhausted_ {false};
    };

} // namespace slopkit::ui::components
