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
    } // namespace

    DisassemblyDocument::DisassemblyDocument(process::AccessWorker&, process::AttachedTarget& target, QObject* parent)
        : QObject(parent), target_(target)
    {
    }

    void DisassemblyDocument::set_view(std::uint64_t first_address, std::size_t visible_rows)
    {
        visible_rows_  = std::max<std::size_t>(1, visible_rows);
        first_address_ = std::min(first_address, scan::kMaxUserAddress);

        // A move inside the window keeps the decoded stream; crossing into
        // another window invalidates it and drops the pass in flight.
        const std::uint64_t base = window_base();
        if (!last_window_base_.has_value() || *last_window_base_ != base)
        {
            last_window_base_ = base;
            pending_.reset();
            requested_pid_ = 0;
            reset_decode();
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
        return window_base_for(first_address_);
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
            if (!readable_ || bytes_ != reading->bytes)
            {
                bytes_    = reading->bytes;
                readable_ = true;
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
        if (!readable_ || decode_exhausted_ || instructions_.size() >= minimum)
        {
            return instructions_.size();
        }

        const std::span<const std::byte> code(bytes_.data(), bytes_.size());
        const std::uint64_t              base = window_base() + decoded_offset_;
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

    void DisassemblyDocument::reset_decode()
    {
        bytes_.clear();
        readable_ = false;
        instructions_.clear();
        decoded_offset_   = 0;
        decode_exhausted_ = false;
    }

    QString DisassemblyDocument::instruction_bytes(const disasm::Instruction& instruction) const
    {
        if (instruction.address < window_base())
        {
            return {};
        }
        const std::size_t offset = static_cast<std::size_t>(instruction.address - window_base());
        if (offset >= bytes_.size())
        {
            return {};
        }

        const std::size_t length = std::min(instruction.length, bytes_.size() - offset);
        std::string       text;
        for (std::size_t index = 0; index < length; ++index)
        {
            if (index != 0)
            {
                text += ' ';
            }
            text += std::format("{:02X}", std::to_integer<unsigned>(bytes_[offset + index]));
        }
        return to_qstring(text);
    }

} // namespace slopkit::ui::components
