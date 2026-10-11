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
#include "script/hook_script.hpp"
#include "script/symbols.hpp"
#include "ui/address_format.hpp"
#include "ui/components/code_patch.hpp"
#include "ui/live_values.hpp"

#include <QObject>
#include <QString>

namespace slopkit::ui::components
{
    enum class CopyFormat
    {
        module_relative,
        absolute,
        bytes,
        instruction,
        address_and_bytes,
        address_and_instruction,
        address_bytes_instruction,
    };

    enum class SegmentKind
    {
        plain,
        cpu_register,
        immediate,
        module,
    };

    struct RowSegment
    {
        QString     text;
        SegmentKind kind {SegmentKind::plain};
        QString     full {};
    };

    class DisassemblyDocument : public QObject, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        static constexpr std::size_t kWindowSize  = 8192;
        static constexpr std::size_t kDecodeSlack = 32;
        static constexpr std::size_t kIdBase      = 0x1000;

        DisassemblyDocument(process::AccessWorker&   worker,
                            process::AttachedTarget& target,
                            CodePatchTable&          patches,
                            QObject*                 parent  = nullptr,
                            script::SymbolTable&     symbols = script::default_symbol_table());

        void                        set_view(std::uint64_t first_address, std::size_t visible_rows);
        [[nodiscard]] std::uint64_t first_address() const noexcept;
        [[nodiscard]] std::size_t   visible_rows() const noexcept;
        [[nodiscard]] std::uint64_t window_base() const noexcept;
        [[nodiscard]] bool          window_exhausted() const noexcept;

        void               set_visible(bool visible);
        [[nodiscard]] bool visible() const noexcept;

        void                                 set_modules(std::vector<process::ModuleInfo> modules);
        void                                 set_address_mode(ui::AddressMode mode);
        void                                 set_machine_mode(disasm::MachineMode mode);
        [[nodiscard]] disasm::MachineMode    machine_mode() const noexcept;
        [[nodiscard]] QString                address_text(std::uint64_t address) const;
        [[nodiscard]] QString                full_address_text(std::uint64_t address) const;
        [[nodiscard]] const ui::ModuleSpans& module_spans() const;
        [[nodiscard]] script::SymbolTable&   symbols() const;

        [[nodiscard]] QString copy_text(std::size_t index, CopyFormat format) const;

        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

        std::size_t               ensure_rows(std::size_t minimum);
        [[nodiscard]] std::size_t row_count() const noexcept;
        [[nodiscard]] std::size_t bytes_width() const noexcept;

        [[nodiscard]] std::size_t line_count(std::size_t index, std::size_t per_line) const noexcept;
        [[nodiscard]] QString     byte_line(std::size_t index, std::size_t line, std::size_t per_line) const;

        struct Row
        {
            std::uint64_t           address {};
            std::size_t             length {};
            QString                 bytes;
            QString                 text;
            std::vector<RowSegment> segments;
            bool                    readable {};
        };

        [[nodiscard]] Row                                 row(std::size_t index) const;
        [[nodiscard]] std::optional<std::size_t>          row_at(std::uint64_t address) const;
        [[nodiscard]] std::span<const disasm::AddressRef> row_addresses(std::size_t index) const;

        [[nodiscard]] std::span<const disasm::MemoryRef> row_memory(std::size_t index) const;

        [[nodiscard]] const CodePatch* patch_at(std::size_t index) const;
        [[nodiscard]] bool             editable(std::size_t index) const;
        [[nodiscard]] QString          row_annotation(std::size_t index) const;

        struct HookAnalysis
        {
            std::optional<script::HookTarget> target;
            std::string                       reason;
        };

        [[nodiscard]] HookAnalysis hook_analysis(std::size_t index) const;

        bool nop_instruction(std::size_t index);
        bool restore_instruction(std::uint64_t address);

        [[nodiscard]] std::optional<QString> edit_source(std::size_t index) const;
        [[nodiscard]] std::string            edit_error(std::size_t index, std::string_view text) const;
        bool                                 edit_instruction(std::size_t index, std::string_view text);

    signals:
        void rowsChanged();

    private:
        struct PendingWindow
        {
            std::uint64_t base {};
            std::uint64_t size {};
        };

        [[nodiscard]] std::uint64_t           window_base_for(std::uint64_t address) const noexcept;
        [[nodiscard]] std::uint64_t           rows_base() const noexcept;
        void                                  reset_decode();
        [[nodiscard]] QString                 instruction_bytes(const disasm::Instruction& instruction) const;
        [[nodiscard]] std::vector<RowSegment> instruction_segments(const disasm::Instruction& instruction) const;
        [[nodiscard]] std::size_t             byte_tokens(std::size_t index) const noexcept;
        [[nodiscard]] std::expected<std::vector<std::byte>, std::string> replacement_for(std::size_t      index,
                                                                                         std::string_view text) const;
        [[nodiscard]] std::span<const std::byte> cached_bytes(std::uint64_t address, std::size_t size) const;
        void                                     note_written(std::uint64_t address, std::span<const std::byte> bytes);

        process::AccessWorker&        worker_;
        process::AttachedTarget&      target_;
        CodePatchTable&               patches_;
        script::SymbolTable&          symbols_;
        std::optional<process::JobId> patch_write_;

        std::uint64_t       first_address_ {0};
        std::size_t         visible_rows_ {1};
        bool                visible_ {true};
        disasm::MachineMode machine_mode_ {disasm::MachineMode::long_64};

        ui::ModuleSpans module_spans_;
        ui::AddressMode address_mode_ {ui::AddressMode::module_relative};

        std::optional<PendingWindow> pending_;
        process::ProcessId           requested_pid_ {0};

        std::uint64_t                window_base_ {0};
        bool                         window_anchored_ {false};
        std::optional<std::uint64_t> decoded_base_;

        std::vector<std::byte>           bytes_;
        bool                             readable_ {false};
        std::vector<disasm::Instruction> instructions_;
        std::size_t                      decoded_offset_ {0};
        bool                             decode_exhausted_ {false};
    };

}
