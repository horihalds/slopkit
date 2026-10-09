#include "ui/components/disassembly_document.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "disasm/assembler.hpp"
#include "scan/types.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::components
{

    namespace
    {
        // The requested read may not run past the canonical user-space ceiling.
        std::size_t clamp_size(std::uint64_t address, std::uint64_t size) noexcept
        {
            if (address > scan::kMaxUserAddress)
            {
                return 0;
            }
            const std::uint64_t room = scan::kMaxUserAddress - address + 1;
            return static_cast<std::size_t>(std::min(size, room));
        }

        // Space-joined `XX` tokens for one line of a row's byte column.
        QString joined_bytes(std::span<const std::byte> bytes)
        {
            std::string text;
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                if (index != 0)
                {
                    text += ' ';
                }
                text += std::format("{:02X}", std::to_integer<unsigned>(bytes[index]));
            }
            return to_qstring(text);
        }
    } // namespace

    DisassemblyDocument::DisassemblyDocument(process::AccessWorker&   worker,
                                             process::AttachedTarget& target,
                                             CodePatchTable&          patches,
                                             QObject*                 parent,
                                             script::SymbolTable&     symbols)
        : QObject(parent), worker_(worker), target_(target), patches_(patches), symbols_(symbols)
    {
    }

    void DisassemblyDocument::set_view(std::uint64_t first_address, std::size_t visible_rows)
    {
        visible_rows_  = std::max<std::size_t>(1, visible_rows);
        first_address_ = std::min(first_address, scan::kMaxUserAddress);

        // Keep the live window while the cursor stays inside it; leaving it
        // seats the cursor on a fresh, page-aligned window and drops the pass in
        // flight. The previous rows stay painted until the new base's payload
        // lands - the reset happens then, keyed on the base, so a byte-identical
        // window is re-decoded too.
        const std::uint64_t base = window_base_for(first_address_);
        if (!window_anchored_ || base != window_base_)
        {
            window_base_     = base;
            window_anchored_ = true;
            pending_.reset();
            requested_pid_ = 0;
        }
    }

    std::uint64_t DisassemblyDocument::first_address() const noexcept
    {
        return first_address_;
    }

    std::size_t DisassemblyDocument::visible_rows() const noexcept
    {
        return visible_rows_;
    }

    std::uint64_t DisassemblyDocument::window_base() const noexcept
    {
        return window_base_;
    }

    std::uint64_t DisassemblyDocument::rows_base() const noexcept
    {
        return decoded_base_.value_or(window_base_);
    }

    void DisassemblyDocument::reset_decode()
    {
        instructions_.clear();
        decoded_offset_   = 0;
        decode_exhausted_ = false;
    }

    bool DisassemblyDocument::window_exhausted() const noexcept
    {
        return decode_exhausted_ && decoded_base_.has_value() && *decoded_base_ == window_base_;
    }

    std::uint64_t DisassemblyDocument::window_base_for(std::uint64_t address) const noexcept
    {
        return address - address % kWindowSize;
    }

    void DisassemblyDocument::set_visible(bool visible)
    {
        visible_ = visible;
    }

    bool DisassemblyDocument::visible() const noexcept
    {
        return visible_;
    }

    void DisassemblyDocument::set_modules(std::vector<process::ModuleInfo> modules)
    {
        ui::ModuleSpans spans;
        spans.set_modules(modules);
        if (spans == module_spans_)
        {
            return;
        }
        module_spans_ = std::move(spans);
        emit rowsChanged();
    }

    void DisassemblyDocument::set_address_mode(ui::AddressMode mode)
    {
        if (address_mode_ == mode)
        {
            return;
        }
        address_mode_ = mode;
        emit rowsChanged();
    }

    void DisassemblyDocument::set_machine_mode(disasm::MachineMode mode)
    {
        if (machine_mode_ == mode)
        {
            return;
        }
        machine_mode_ = mode;

        // The same bytes decode differently, so the cache has to go; the bytes
        // themselves are mode-independent and stay.
        instructions_.clear();
        decoded_offset_   = 0;
        decode_exhausted_ = false;
        emit rowsChanged();
    }

    disasm::MachineMode DisassemblyDocument::machine_mode() const noexcept
    {
        return machine_mode_;
    }

    QString DisassemblyDocument::address_text(std::uint64_t address) const
    {
        return ui::format_pane_address(address_mode_, module_spans_, address);
    }

    QString DisassemblyDocument::full_address_text(std::uint64_t address) const
    {
        return ui::format_pane_address_full(address_mode_, module_spans_, address);
    }

    const ui::ModuleSpans& DisassemblyDocument::module_spans() const
    {
        return module_spans_;
    }

    script::SymbolTable& DisassemblyDocument::symbols() const
    {
        return symbols_;
    }

    QString DisassemblyDocument::copy_text(std::size_t index, CopyFormat format) const
    {
        if (index >= instructions_.size())
        {
            return {};
        }
        const Row     value   = row(index);
        const QString address = address_text(value.address);
        switch (format)
        {
        case CopyFormat::module_relative:
            return ui::format_cell_address_full(ui::AddressMode::module_relative, module_spans_, value.address);
        case CopyFormat::absolute:
            return ui::format_absolute(value.address);
        case CopyFormat::bytes:
            return value.bytes;
        case CopyFormat::instruction:
            return value.text;
        case CopyFormat::address_and_bytes:
            return value.bytes.isEmpty() ? address : address + QStringLiteral(": ") + value.bytes;
        case CopyFormat::address_and_instruction:
            return value.text.isEmpty() ? address : address + QStringLiteral(": ") + value.text;
        case CopyFormat::address_bytes_instruction:
        {
            QString text = address;
            if (!value.bytes.isEmpty())
            {
                text += QStringLiteral(": ") + value.bytes;
            }
            if (!value.text.isEmpty())
            {
                text += (value.bytes.isEmpty() ? QStringLiteral(": ") : QStringLiteral("  ")) + value.text;
            }
            return text;
        }
        }
        return {};
    }

    std::vector<ui::LiveRequest> DisassemblyDocument::next_live_request()
    {
        pending_.reset();
        requested_pid_ = 0;
        if (!visible_ || !target_.valid())
        {
            return {};
        }

        const std::uint64_t base = window_base();
        const std::size_t   size = clamp_size(base, kWindowSize);
        if (size == 0)
        {
            return {};
        }

        pending_       = PendingWindow {.base = base, .size = size};
        requested_pid_ = target_.pid;
        return {
            ui::LiveRequest {.id = kIdBase, .address = base, .size = size}
        };
    }

    void DisassemblyDocument::apply_live_readings(std::span<const ui::LiveReading> readings)
    {
        if (!pending_.has_value() || !target_.valid() || target_.pid != requested_pid_)
        {
            return; // Nothing was submitted, or the target moved on in flight.
        }

        const ui::LiveReading* reading = nullptr;
        for (const ui::LiveReading& candidate : readings)
        {
            if (candidate.id == kIdBase)
            {
                reading = &candidate;
                break;
            }
        }
        if (reading == nullptr)
        {
            return; // Not our window.
        }

        bool dirty = false;
        if (reading->readable && !reading->bytes.empty())
        {
            // Re-decode when this window's payload is new for its base. Keying
            // on the base and not the bytes re-decodes a byte-identical window
            // (all-NOP pages) as well.
            const bool same_base = decoded_base_.has_value() && *decoded_base_ == pending_->base;
            if (!same_base || bytes_ != reading->bytes)
            {
                bytes_        = reading->bytes;
                readable_     = true;
                decoded_base_ = pending_->base;
                instructions_.clear();
                decoded_offset_   = 0;
                decode_exhausted_ = false;
                dirty             = true;
            }
        }
        else if (readable_)
        {
            // Keep the rows we decoded and mask them to `??`: a transient failed
            // read must not wipe the pane.
            bytes_.clear();
            readable_ = false;
            dirty     = true;
        }

        if (dirty)
        {
            emit rowsChanged();
        }
    }

    std::size_t DisassemblyDocument::ensure_rows(std::size_t minimum)
    {
        // Only sweep the window whose payload has actually landed; while a step
        // is in flight the previous window's rows stay painted untouched.
        if (!readable_ || !decoded_base_.has_value() || *decoded_base_ != window_base_ || decode_exhausted_
            || instructions_.size() >= minimum)
        {
            return instructions_.size();
        }

        const std::span<const std::byte> code(bytes_.data(), bytes_.size());
        const std::uint64_t              base = window_base_ + decoded_offset_;
        const std::size_t                want = minimum - instructions_.size();
        const auto block = disasm::decode_block(code.subspan(decoded_offset_), base, want, machine_mode_);
        if (block.empty())
        {
            decode_exhausted_ = true;
            return instructions_.size();
        }

        instructions_.insert(instructions_.end(), block.begin(), block.end());
        decoded_offset_ += static_cast<std::size_t>(block.back().address - base) + block.back().length;

        // `decode_block` stops early only for a truncated tail, which the window
        // cannot extend past, so no further sweep would make progress.
        if (decoded_offset_ >= bytes_.size() || block.size() < want)
        {
            decode_exhausted_ = true;
        }
        return instructions_.size();
    }

    std::size_t DisassemblyDocument::row_count() const noexcept
    {
        return instructions_.size();
    }

    std::size_t DisassemblyDocument::bytes_width() const noexcept
    {
        if (!readable_)
        {
            return 2;
        }
        std::size_t width = 2;
        for (const disasm::Instruction& instruction : instructions_)
        {
            width = std::max(width, instruction.length == 0 ? 2 : instruction.length * 3 - 1);
        }
        return width;
    }

    std::size_t DisassemblyDocument::byte_tokens(std::size_t index) const noexcept
    {
        if (index >= instructions_.size())
        {
            return 0;
        }
        if (!readable_)
        {
            return 1; // The single `??` token.
        }

        // How many of the instruction's bytes the decoded window holds; a byte
        // that the window cannot cover renders as an empty row.
        const disasm::Instruction& instruction = instructions_[index];
        const std::uint64_t        base        = rows_base();
        if (instruction.address < base)
        {
            return 0;
        }
        const std::size_t offset = static_cast<std::size_t>(instruction.address - base);
        if (offset >= bytes_.size())
        {
            return 0;
        }
        return std::min<std::size_t>(instruction.length, bytes_.size() - offset);
    }

    std::span<const std::byte> DisassemblyDocument::cached_bytes(std::uint64_t address, std::size_t size) const
    {
        if (!readable_ || !decoded_base_.has_value())
        {
            return {};
        }
        const std::uint64_t base = *decoded_base_;
        if (address < base || address - base > bytes_.size() || size > bytes_.size() - (address - base))
        {
            return {};
        }
        return std::span<const std::byte>(bytes_.data() + static_cast<std::ptrdiff_t>(address - base), size);
    }

    void DisassemblyDocument::note_written(std::uint64_t address, std::span<const std::byte> bytes)
    {
        if (readable_ && decoded_base_.has_value() && address >= *decoded_base_
            && address - *decoded_base_ + bytes.size() <= bytes_.size())
        {
            std::copy(
                bytes.begin(), bytes.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(address - *decoded_base_));
        }
        // The listing shows the bytes the target now holds without waiting for
        // the next live pass, which would reach the same bytes anyway.
        reset_decode();
        emit rowsChanged();
    }

    std::size_t DisassemblyDocument::line_count(std::size_t index, std::size_t per_line) const noexcept
    {
        const std::size_t tokens = byte_tokens(index);
        if (tokens == 0)
        {
            return index < instructions_.size() ? 1 : 0;
        }
        const std::size_t stride = std::max<std::size_t>(1, per_line);
        return (tokens + stride - 1) / stride;
    }

    QString DisassemblyDocument::byte_line(std::size_t index, std::size_t line, std::size_t per_line) const
    {
        const std::size_t stride = std::max<std::size_t>(1, per_line);
        const std::size_t first  = line * stride;
        const std::size_t tokens = byte_tokens(index);
        if (first >= tokens)
        {
            return {};
        }
        if (!readable_)
        {
            return QStringLiteral("??"); // `tokens` is 1, so `first` is 0.
        }

        const disasm::Instruction& instruction = instructions_[index];
        const std::size_t          offset      = static_cast<std::size_t>(instruction.address - rows_base()) + first;
        const std::size_t          count       = std::min(stride, tokens - first);
        return joined_bytes(std::span<const std::byte>(bytes_.data() + offset, count));
    }

    DisassemblyDocument::Row DisassemblyDocument::row(std::size_t index) const
    {
        Row result;
        if (index >= instructions_.size())
        {
            return result;
        }

        const disasm::Instruction& instruction = instructions_[index];
        result.address                         = instruction.address;
        result.length                          = instruction.length;
        if (!readable_)
        {
            result.bytes = QStringLiteral("??");
            return result;
        }

        result.readable = true;
        result.bytes    = instruction_bytes(instruction);
        result.segments = instruction_segments(instruction);
        for (const RowSegment& segment : result.segments)
        {
            result.text += segment.text;
        }
        return result;
    }

    std::span<const disasm::AddressRef> DisassemblyDocument::row_addresses(std::size_t index) const
    {
        if (index >= instructions_.size() || !readable_)
        {
            return {};
        }
        return instructions_[index].addresses;
    }

    std::span<const disasm::MemoryRef> DisassemblyDocument::row_memory(std::size_t index) const
    {
        if (index >= instructions_.size() || !readable_)
        {
            return {};
        }
        return instructions_[index].memory;
    }

    const CodePatch* DisassemblyDocument::patch_at(std::size_t index) const
    {
        if (index >= instructions_.size())
        {
            return nullptr;
        }
        return patches_.covering(instructions_[index].address);
    }

    QString DisassemblyDocument::row_annotation(std::size_t index) const
    {
        const CodePatch* patch = patch_at(index);
        if (patch == nullptr || index >= instructions_.size() || patch->begin != instructions_[index].address)
        {
            return {};
        }
        return (patch->nop ? QStringLiteral("NOPed: ") : QStringLiteral("Edited: ")) + patch->original_text;
    }

    bool DisassemblyDocument::nop_instruction(std::size_t index)
    {
        if (!target_.valid())
        {
            log::warning(log::category::ui, "disassembly nop refused: no target attached");
            return false;
        }
        if (patch_write_.has_value())
        {
            log::warning(log::category::ui, "disassembly nop refused: a code patch write is already in flight");
            return false;
        }

        const Row value = row(index);
        if (index >= instructions_.size() || !value.readable || !instructions_[index].valid)
        {
            log::warning(log::category::ui, "disassembly nop refused: the row is not a decoded instruction");
            return false;
        }
        const std::span<const std::byte> originals = cached_bytes(value.address, value.length);
        if (originals.size() != value.length)
        {
            log::warning(log::category::ui, "disassembly nop refused: the instruction is not fully cached");
            return false;
        }

        const process::JobId job_id = worker_.next_job_id();
        patch_write_                = job_id;

        std::vector<std::byte> original(originals.begin(), originals.end());
        std::vector<std::byte> replacement = nop_bytes(value.length);
        std::vector<std::byte> written     = replacement;
        const bool             submitted   = worker_.submit_write(
            job_id,
            value.address,
            value.address,
            std::move(replacement),
            [this,
             job_id,
             address  = value.address,
             length   = value.length,
             original = std::move(original),
             text     = value.text,
             written  = std::move(written)](process::JobResult&& result)
            {
                if (patch_write_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                patch_write_.reset();

                const auto& write = std::get<process::WriteResult>(result);
                if (write.error)
                {
                    log::warning(log::category::ui,
                                 std::format("disassembly nop failed: {}", process::describe(*write.error)));
                    return;
                }

                patches_.apply_nop(address, length, std::move(original), text);
                note_written(address, written);
                log::info(log::category::ui,
                          std::format("disassembly nop: {} byte(s) at {:#x} replaced with NOP", length, address));
            });
        if (!submitted)
        {
            patch_write_.reset();
            log::warning(log::category::ui, "disassembly nop refused: the access worker is not accepting jobs");
            return false;
        }
        return true;
    }

    bool DisassemblyDocument::restore_instruction(std::uint64_t address)
    {
        if (patch_write_.has_value())
        {
            log::warning(log::category::ui, "disassembly restore refused: a code patch write is already in flight");
            return false;
        }
        const CodePatch* patch = patches_.covering(address);
        if (patch == nullptr)
        {
            log::warning(log::category::ui, "disassembly restore refused: no patch covers this address");
            return false;
        }

        const std::uint64_t    begin    = patch->begin;
        std::vector<std::byte> original = patch->original_bytes;
        const std::size_t      length   = original.size();

        const process::JobId job_id = worker_.next_job_id();
        patch_write_                = job_id;

        std::vector<std::byte> written   = original;
        const bool             submitted = worker_.submit_write(
            job_id,
            begin,
            begin,
            std::move(original),
            [this, job_id, begin, length, written = std::move(written)](process::JobResult&& result)
            {
                if (patch_write_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                patch_write_.reset();

                const auto& write = std::get<process::WriteResult>(result);
                if (write.error)
                {
                    log::warning(log::category::ui,
                                 std::format("disassembly restore failed: {}", process::describe(*write.error)));
                    return;
                }

                patches_.restore(begin);
                note_written(begin, written);
                log::info(log::category::ui,
                          std::format("disassembly restore: {} byte(s) at {:#x} written back", length, begin));
            });
        if (!submitted)
        {
            patch_write_.reset();
            log::warning(log::category::ui, "disassembly restore refused: the access worker is not accepting jobs");
            return false;
        }
        return true;
    }

    std::optional<QString> DisassemblyDocument::edit_source(std::size_t index) const
    {
        if (!editable(index))
        {
            return std::nullopt;
        }
        return to_qstring(instructions_[index].text);
    }

    std::string DisassemblyDocument::edit_error(std::size_t index, std::string_view text) const
    {
        if (!editable(index))
        {
            return "this instruction can no longer be edited";
        }
        const auto replacement = replacement_for(index, text);
        if (!replacement)
        {
            return replacement.error();
        }
        const std::size_t length = instructions_[index].length;
        if (replacement->size() > length)
        {
            return std::format(
                "{} bytes does not fit the original {} byte(s); use fewer bytes", replacement->size(), length);
        }
        return {};
    }

    bool DisassemblyDocument::edit_instruction(std::size_t index, std::string_view text)
    {
        if (!target_.valid())
        {
            log::warning(log::category::ui, "disassembly edit refused: no target attached");
            return false;
        }
        if (patch_write_.has_value())
        {
            log::warning(log::category::ui, "disassembly edit refused: a code patch write is already in flight");
            return false;
        }
        if (!editable(index))
        {
            log::warning(log::category::ui, "disassembly edit refused: the row is not an editable decoded instruction");
            return false;
        }

        const Row  value       = row(index);
        const auto replacement = replacement_for(index, text);
        if (!replacement)
        {
            log::warning(log::category::ui, std::format("disassembly edit refused: {}", replacement.error()));
            return false;
        }
        if (replacement->size() > value.length)
        {
            log::warning(log::category::ui,
                         std::format("disassembly edit refused: {} bytes does not fit the original {} byte(s)",
                                     replacement->size(),
                                     value.length));
            return false;
        }

        const std::span<const std::byte> originals = cached_bytes(value.address, value.length);
        if (originals.size() != value.length)
        {
            log::warning(log::category::ui, "disassembly edit refused: the instruction is not fully cached");
            return false;
        }

        // Re-assembling the original text reproduces it, so accepting the box
        // unchanged is not an edit and records no patch.
        if (replacement->size() == originals.size()
            && std::equal(replacement->begin(), replacement->end(), originals.begin()))
        {
            return true;
        }

        const process::JobId job_id = worker_.next_job_id();
        patch_write_                = job_id;

        std::vector<std::byte> original  = std::vector<std::byte>(originals.begin(), originals.end());
        const std::size_t      assembled = replacement->size();
        std::vector<std::byte> written   = *replacement;
        written.resize(value.length, std::byte {0x90});
        std::vector<std::byte> submitted_bytes = written;
        const bool             submitted       = worker_.submit_write(
            job_id,
            value.address,
            value.address,
            std::move(written),
            [this,
             job_id,
             address = value.address,
             length  = value.length,
             assembled,
             original        = std::move(original),
             text            = value.text,
             submitted_bytes = std::move(submitted_bytes)](process::JobResult&& result)
            {
                if (patch_write_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                patch_write_.reset();

                const auto& write = std::get<process::WriteResult>(result);
                if (write.error)
                {
                    log::warning(log::category::ui,
                                 std::format("disassembly edit failed: {}", process::describe(*write.error)));
                    return;
                }

                patches_.apply_edit(address, length, std::move(original), text);
                note_written(address, submitted_bytes);
                log::info(log::category::ui,
                          std::format("disassembly edit: {} byte(s) at {:#x} replace a {} byte instruction",
                                      assembled,
                                      address,
                                      length));
            });
        if (!submitted)
        {
            patch_write_.reset();
            log::warning(log::category::ui, "disassembly edit refused: the access worker is not accepting jobs");
            return false;
        }
        return true;
    }

    bool DisassemblyDocument::editable(std::size_t index) const
    {
        if (!readable_ || index >= instructions_.size())
        {
            return false;
        }
        const disasm::Instruction& instruction = instructions_[index];
        if (!instruction.valid || patches_.covering(instruction.address) != nullptr)
        {
            return false;
        }
        return cached_bytes(instruction.address, instruction.length).size() == instruction.length;
    }

    DisassemblyDocument::HookAnalysis DisassemblyDocument::hook_analysis(std::size_t index) const
    {
        HookAnalysis analysis;
        if (!target_.valid())
        {
            analysis.reason = "no target is attached";
            return analysis;
        }
        if (index >= instructions_.size())
        {
            analysis.reason = "this row is not in the listing";
            return analysis;
        }
        if (patch_at(index) != nullptr)
        {
            analysis.reason = "this row is already patched by this session";
            return analysis;
        }
        if (!editable(index))
        {
            analysis.reason = "this row is not a fully decoded instruction";
            return analysis;
        }

        // The neighbourhood the generator decides from: every decoded row with
        // its cached bytes and the addresses its text prints. The spans point
        // into the document's own cache and instruction stream, which stay put
        // for the length of the call.
        std::vector<script::HookRow> rows;
        rows.reserve(instructions_.size());
        for (const disasm::Instruction& instruction : instructions_)
        {
            script::HookRow row;
            row.address       = instruction.address;
            row.bytes         = cached_bytes(instruction.address, instruction.length);
            row.address_bytes = instruction.address_bytes;
            row.addresses     = instruction.addresses;
            row.memory        = instruction.memory;
            row.text          = instruction.text;
            row.valid         = instruction.valid && row.bytes.size() == instruction.length;
            rows.push_back(std::move(row));
        }

        const std::uint64_t   address = instructions_[index].address;
        script::HookCandidate candidate;
        candidate.rows     = rows;
        candidate.selected = index;
        candidate.mode     = machine_mode_;
        if (const ui::ModuleSpan* span = module_spans_.containing(address); span != nullptr)
        {
            candidate.module_name     = span->name;
            candidate.module_rva_text = QString::number(address - span->base, 16).toUpper().toStdString();
            candidate.description     = span->name + "+" + candidate.module_rva_text;
        }
        else
        {
            candidate.description = QString::number(address, 16).toUpper().toStdString();
        }

        analysis.target = script::build_hook(candidate, analysis.reason);
        return analysis;
    }

    std::expected<std::vector<std::byte>, std::string> DisassemblyDocument::replacement_for(std::size_t      index,
                                                                                            std::string_view text) const
    {
        const disasm::Instruction& instruction = instructions_[index];
        return disasm::assemble(text,
                                disasm::AssembleContext {.address = instruction.address,
                                                         .mode    = machine_mode_,
                                                         .memory  = instruction.memory});
    }

    std::optional<std::size_t> DisassemblyDocument::row_at(std::uint64_t address) const
    {
        if (instructions_.empty() || address < instructions_.front().address)
        {
            return std::nullopt;
        }

        // The last instruction whose start is at or before `address`.
        std::size_t low  = 0;
        std::size_t high = instructions_.size();
        while (low < high)
        {
            const std::size_t middle = low + (high - low) / 2;
            if (instructions_[middle].address <= address)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        const std::size_t index = low - 1;
        if (address >= instructions_[index].address + instructions_[index].length)
        {
            return std::nullopt;
        }
        return index;
    }

    QString DisassemblyDocument::instruction_bytes(const disasm::Instruction& instruction) const
    {
        if (instruction.address < rows_base())
        {
            return {};
        }
        const std::size_t offset = static_cast<std::size_t>(instruction.address - rows_base());
        if (offset >= bytes_.size())
        {
            return {};
        }

        const std::size_t length = std::min(instruction.length, bytes_.size() - offset);
        return joined_bytes(std::span<const std::byte>(bytes_.data() + offset, length));
    }

    std::vector<RowSegment> DisassemblyDocument::instruction_segments(const disasm::Instruction& instruction) const
    {
        // One classified slice of `instruction.text`: where it sits in the raw
        // text and the run(s) that replace it. An address inside a module becomes
        // a module-name run, a plain `+` and an immediate `RVA` run; every other
        // slice is one run.
        struct Mark
        {
            std::size_t             offset {};
            std::size_t             length {};
            std::vector<RowSegment> runs;
        };

        std::vector<Mark> marks;
        marks.reserve(instruction.tokens.size() + instruction.addresses.size());

        for (const disasm::TokenSpan& span : instruction.tokens)
        {
            const std::string_view slice = std::string_view(instruction.text).substr(span.offset, span.length);
            SegmentKind            kind  = SegmentKind::plain;
            if (span.kind == disasm::TokenKind::cpu_register)
            {
                kind = SegmentKind::cpu_register;
            }
            else if (span.kind == disasm::TokenKind::immediate || span.kind == disasm::TokenKind::displacement)
            {
                kind = SegmentKind::immediate;
            }
            marks.push_back({span.offset, span.length, {RowSegment {to_qstring(slice), kind}}});
        }

        for (const disasm::AddressRef& ref : instruction.addresses)
        {
            const std::string_view slice    = std::string_view(instruction.text).substr(ref.offset, ref.length);
            const auto             relative = ui::module_relative_text(address_mode_, module_spans_, ref.address);
            if (!relative.has_value())
            {
                marks.push_back({ref.offset, ref.length, {RowSegment {to_qstring(slice), SegmentKind::immediate}}});
                continue;
            }

            const ui::ModuleSpan*   span        = module_spans_.containing(ref.address);
            // The module run paints the shortened label; it carries the full
            // `<module>+<RVA>` for the hover only when the label was shortened.
            const QString           module_name = span != nullptr ? to_qstring(span->label()) : QString {};
            const QString           full        = span != nullptr && span->label() != span->name
                                                    ? ui::format_module_relative_full(*span, ref.address)
                                                    : QString {};
            const QString           offset_text = relative->mid(module_name.size()); // "+RVA"
            std::vector<RowSegment> runs;
            runs.push_back({module_name, SegmentKind::module, full});
            if (!offset_text.isEmpty())
            {
                // The separator stays in the row's own colour and the RVA prints
                // as the number it is, matching a memory displacement.
                runs.push_back({offset_text.left(1), SegmentKind::plain});
                if (const QString rva = offset_text.mid(1); !rva.isEmpty())
                {
                    runs.push_back({rva, SegmentKind::immediate});
                }
            }
            marks.push_back({ref.offset, ref.length, std::move(runs)});
        }

        std::ranges::sort(marks, {}, &Mark::offset);

        std::vector<RowSegment> segments;
        const auto              append = [&segments](RowSegment run)
        {
            if (run.text.isEmpty())
            {
                return;
            }
            if (run.kind == SegmentKind::plain && !segments.empty() && segments.back().kind == SegmentKind::plain)
            {
                segments.back().text += run.text;
                return;
            }
            segments.push_back(std::move(run));
        };

        const std::string_view raw    = instruction.text;
        std::size_t            cursor = 0;
        for (const Mark& mark : marks)
        {
            if (mark.offset < cursor)
            {
                continue; // an overlapping slice was already painted
            }
            if (mark.offset > cursor)
            {
                append(RowSegment {to_qstring(raw.substr(cursor, mark.offset - cursor)), SegmentKind::plain});
            }
            for (const RowSegment& run : mark.runs)
            {
                append(run);
            }
            cursor = mark.offset + mark.length;
        }
        if (cursor < raw.size())
        {
            append(RowSegment {to_qstring(raw.substr(cursor)), SegmentKind::plain});
        }
        return segments;
    }

} // namespace slopkit::ui::components
