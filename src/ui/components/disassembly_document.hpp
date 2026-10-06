#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "disasm/decoder.hpp"
#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "ui/address_format.hpp"
#include "ui/components/code_patch.hpp"
#include "ui/live_values.hpp"

#include <QObject>
#include <QString>

namespace slopkit::ui::components
{

    // What the listing's Copy submenu puts on the clipboard for a decoded row.
    enum class CopyFormat
    {
        module_relative,           // "<module>+<RVA>"; the absolute form outside every module
        absolute,                  // "7F3A1B2C"
        bytes,                     // "48 89 E5", or "??" when unreadable
        instruction,               // "MOV RBP, RSP"
        address_and_bytes,         // "app+10: 48 89 E5"
        address_and_instruction,   // "app+10: MOV RBP, RSP"
        address_bytes_instruction, // "app+10: 48 89 E5  MOV RBP, RSP"
    };

    // How the listing paints one run of an instruction line.
    enum class SegmentKind
    {
        plain,        // mnemonic, brackets, commas: the row's own colour
        cpu_register, // a register name
        immediate,    // an immediate or a displacement
        module,       // the module name of a `module+RVA` address
    };

    // One painted run of the rendered line, in the order it prints.
    struct RowSegment
    {
        QString     text;
        SegmentKind kind {SegmentKind::plain};
    };

    // The live state behind the disassembly listing: one aligned code window and
    // a lazily grown cache of the instructions decoded from it. It owns no
    // widget, so the risky part is testable headlessly; DisassemblyView only
    // paints it. The window is the aligned sweep unit: the cursor may move
    // inside it, and walking past the decoded rows steps the whole window by
    // one aligned page, so the listing never dead-ends.
    class DisassemblyDocument : public QObject, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        static constexpr std::size_t kWindowSize  = 8192;   // bytes per live window
        static constexpr std::size_t kDecodeSlack = 32;     // rows decoded past the viewport
        static constexpr std::size_t kIdBase      = 0x1000; // this pane's LiveRequest id

        DisassemblyDocument(process::AccessWorker&   worker,
                            process::AttachedTarget& target,
                            CodePatchTable&          patches,
                            QObject*                 parent = nullptr);

        // The cursor the listing starts at and how many rows fit.
        void                        set_view(std::uint64_t first_address, std::size_t visible_rows);
        [[nodiscard]] std::uint64_t first_address() const noexcept;
        [[nodiscard]] std::size_t   visible_rows() const noexcept;
        [[nodiscard]] std::uint64_t window_base() const noexcept; // aligned live-window start
        // True once the cursor's window has been swept to its end (or stopped at
        // a truncated tail), so only another window can yield further rows.
        [[nodiscard]] bool          window_exhausted() const noexcept;

        // The live window is only requested while the view is on screen.
        void               set_visible(bool visible);
        [[nodiscard]] bool visible() const noexcept;

        void                                 set_modules(std::vector<process::ModuleInfo> modules);
        void                                 set_address_mode(ui::AddressMode mode);
        void                                 set_machine_mode(disasm::MachineMode mode);
        [[nodiscard]] disasm::MachineMode    machine_mode() const noexcept;
        [[nodiscard]] QString                address_text(std::uint64_t address) const;
        [[nodiscard]] const ui::ModuleSpans& module_spans() const; // Go To validation

        // The clipboard text for the decoded row `index` in `format`; empty when
        // the row is out of range or the format has nothing to copy. The address
        // forms follow the current display mode, the module-relative form falls
        // back to the absolute form outside every module.
        [[nodiscard]] QString copy_text(std::size_t index, CopyFormat format) const;

        // LiveSurface: one aligned window while visible with a target attached.
        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

        // Decodes forward until at least `minimum` rows exist (or the window
        // ends); returns how many rows are decoded now. The scroll ceiling.
        std::size_t               ensure_rows(std::size_t minimum);
        [[nodiscard]] std::size_t row_count() const noexcept;
        [[nodiscard]] std::size_t bytes_width() const noexcept; // widest byte text, in characters

        // How many lines the row's byte text takes at `per_line` bytes per line;
        // `per_line == 0` behaves as 1 and a row with no byte text still takes
        // one line. A row index that is out of range takes no line.
        [[nodiscard]] std::size_t line_count(std::size_t index, std::size_t per_line) const noexcept;
        // The row's byte text on `line` at `per_line` bytes per line; empty when
        // the row or the line index is out of range or the row has no bytes. A
        // byte token is never split.
        [[nodiscard]] QString     byte_line(std::size_t index, std::size_t line, std::size_t per_line) const;

        struct Row
        {
            std::uint64_t           address {};
            std::size_t             length {};
            QString                 bytes;    // "48 89 E5", or "??" when unreadable
            QString                 text;     // "MOV RBP, RSP", or empty
            std::vector<RowSegment> segments; // the runs `text` concatenates to; empty when unreadable
            bool                    readable {};
        };

        [[nodiscard]] Row                                 row(std::size_t index) const;
        [[nodiscard]] std::optional<std::size_t>          row_at(std::uint64_t address) const;
        // The addresses row `index` references (branch/call targets and memory
        // operands, in printed order); empty for an out-of-range or unreadable
        // row and for a `.byte` row, which references nothing. The span points
        // into the decoded instruction stream, so it is valid only until the
        // window moves.
        [[nodiscard]] std::span<const disasm::AddressRef> row_addresses(std::size_t index) const;

        // The memory operands row `index` accesses (register-relative, absolute
        // and rip-relative), as the decoder reports them; empty for an
        // out-of-range or unreadable row, a `.byte` row and a `lea`. The span
        // points into the decoded instruction stream, so it is valid only until
        // the window moves.
        [[nodiscard]] std::span<const disasm::MemoryRef> row_memory(std::size_t index) const;

        // The applied code patch covering the decoded row `index` (a click anywhere in
        // the replaced instruction reaches it); null when the row is not inside one.
        [[nodiscard]] const CodePatch* patch_at(std::size_t index) const;
        // True when the decoded row `index` is an instruction this session can still
        // rewrite: valid, fully cached and not already covered by a session patch. The
        // listing's menu and the Memory Viewer's Tools menu both gate their NOP/Edit
        // commands on it, so the two surfaces cannot disagree about a row.
        [[nodiscard]] bool             editable(std::size_t index) const;
        // The paint annotation for row `index`: "NOPed: MOV RBP, RSP" or "Edited: MOV
        // RBP, RSP" on the patch's own first row, empty everywhere else.
        [[nodiscard]] QString          row_annotation(std::size_t index) const;

        // Replaces the whole instruction on row `index` with NOP bytes and records what
        // it held. False when there is no target, the row is not a decoded instruction
        // whose bytes are fully cached, or a patch write is already in flight.
        bool nop_instruction(std::size_t index);
        // Writes the original bytes of the patch covering `address` back and forgets it.
        bool restore_instruction(std::uint64_t address);

        // The assembler text the edit box for row `index` starts from - the instruction
        // as the listing prints it, so addresses are absolute; nothing when the row is
        // not an editable decoded instruction (see `editable`).
        [[nodiscard]] std::optional<QString> edit_source(std::size_t index) const;
        // Validates assembler `text` for row `index`: empty when it encodes to an
        // instruction that fits the row, otherwise why it does not.
        [[nodiscard]] std::string            edit_error(std::size_t index, std::string_view text) const;
        // Assembles `text` for row `index`, pads any shortfall with NOP bytes and
        // records what the row held so it can be restored. False when there is no
        // target, the row is not editable, the text does not assemble or it is longer
        // than the original.
        bool                                 edit_instruction(std::size_t index, std::string_view text);

    signals:
        void rowsChanged(); // new bytes, a new window or a longer decode

    private:
        // One window slot submitted by the pass in flight.
        struct PendingWindow
        {
            std::uint64_t base {};
            std::uint64_t size {};
        };

        [[nodiscard]] std::uint64_t           window_base_for(std::uint64_t address) const noexcept;
        // The base the currently decoded rows were swept from; the live window
        // while a step is in flight still holds the previous window's rows.
        [[nodiscard]] std::uint64_t           rows_base() const noexcept;
        void                                  reset_decode();
        [[nodiscard]] QString                 instruction_bytes(const disasm::Instruction& instruction) const;
        [[nodiscard]] std::vector<RowSegment> instruction_segments(const disasm::Instruction& instruction) const;
        [[nodiscard]] std::size_t             byte_tokens(std::size_t index) const noexcept;
        // The bytes `text` assembles to for row `index`, using the instruction's memory
        // operands as the rip-relative template; the error when it does not assemble.
        [[nodiscard]] std::expected<std::vector<std::byte>, std::string> replacement_for(std::size_t      index,
                                                                                         std::string_view text) const;
        // The cached window bytes of `size` bytes at `address`, empty when they are not
        // all cached (a window boundary).
        [[nodiscard]] std::span<const std::byte> cached_bytes(std::uint64_t address, std::size_t size) const;
        // Reflects bytes this session wrote into the cached window in place, so the
        // listing shows them without waiting for an idle pass.
        void                                     note_written(std::uint64_t address, std::span<const std::byte> bytes);

        process::AccessWorker&        worker_;
        process::AttachedTarget&      target_;
        CodePatchTable&               patches_;
        // The patch write in flight, so a second NOP or a restore is refused.
        std::optional<process::JobId> patch_write_;

        std::uint64_t       first_address_ {0};
        std::size_t         visible_rows_ {1};
        bool                visible_ {true};
        disasm::MachineMode machine_mode_ {disasm::MachineMode::long_64};

        ui::ModuleSpans module_spans_;
        ui::AddressMode address_mode_ {ui::AddressMode::module_relative};

        // The window submitted in the pass in flight plus the pid it belonged
        // to; reset whenever the window moves, so a stale reading is dropped.
        std::optional<PendingWindow> pending_;
        process::ProcessId           requested_pid_ {0};

        // The aligned live window and the base its bytes/rows were swept from;
        // a step keeps the old rows until the new base's payload lands.
        std::uint64_t                window_base_ {0};
        bool                         window_anchored_ {false};
        std::optional<std::uint64_t> decoded_base_;

        // The decoded instruction stream, kept while the window's bytes are
        // unchanged so an idle pass never re-decodes.
        std::vector<std::byte>           bytes_;
        bool                             readable_ {false};
        std::vector<disasm::Instruction> instructions_;
        std::size_t                      decoded_offset_ {0};
        bool                             decode_exhausted_ {false};
    };

} // namespace slopkit::ui::components
