#include "ui/components/disassembly_document.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include "scan/types.hpp"

namespace slopkit::ui::components
{

    namespace
    {
        QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        }

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

    DisassemblyDocument::DisassemblyDocument(process::AccessWorker&, process::AttachedTarget& target, QObject* parent)
        : QObject(parent), target_(target)
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
        if (const auto relative = ui::module_relative_text(address_mode_, module_spans_, address); relative.has_value())
        {
            return *relative;
        }
        return QStringLiteral("0x") + QString::number(address, 16).rightJustified(16, QLatin1Char('0')).toUpper();
    }

    const ui::ModuleSpans& DisassemblyDocument::module_spans() const
    {
        return module_spans_;
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
        result.text     = to_qstring(instruction.text);
        return result;
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

} // namespace slopkit::ui::components
