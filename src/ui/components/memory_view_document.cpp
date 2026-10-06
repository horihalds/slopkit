#include "ui/components/memory_view_document.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QStringDecoder>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::components
{

    namespace
    {
        bool is_printable(std::byte value)
        {
            const auto character = std::to_integer<unsigned>(value);
            return character >= 0x20 && character < 0x7F;
        }

        // `value` rounded down to a multiple of `step`; `step` is never zero.
        std::uint64_t align_down(std::uint64_t value, std::uint64_t step) noexcept
        {
            return value - value % step;
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

    MemoryViewDocument::MemoryViewDocument(process::AccessWorker&   worker,
                                           process::AttachedTarget& target,
                                           QObject*                 parent)
        : QObject(parent), worker_(worker), target_(target)
    {
    }

    void MemoryViewDocument::set_view(std::uint64_t first_byte, std::size_t bytes_per_row, std::size_t visible_rows)
    {
        bytes_per_row_ = std::max<std::size_t>(1, bytes_per_row);
        visible_rows_  = std::max<std::size_t>(1, visible_rows);

        // Keep the requested top byte verbatim; only the block window stays
        // aligned to the visible block.
        first_byte_                      = first_byte;
        const std::uint64_t visible_base = visible_block_base();
        if (visible_base != last_window_base_)
        {
            last_window_base_ = visible_base;
            clear_pending(); // The in-flight pass belonged to the previous window.
        }
        prune_blocks(visible_base);
    }

    std::uint64_t MemoryViewDocument::first_byte() const noexcept
    {
        return first_byte_;
    }

    std::size_t MemoryViewDocument::bytes_per_row() const noexcept
    {
        return bytes_per_row_;
    }

    std::size_t MemoryViewDocument::visible_rows() const noexcept
    {
        return visible_rows_;
    }

    void MemoryViewDocument::set_visible(bool visible)
    {
        visible_ = visible;
    }

    bool MemoryViewDocument::visible() const noexcept
    {
        return visible_;
    }

    ValueFormat MemoryViewDocument::format() const noexcept
    {
        return format_;
    }

    void MemoryViewDocument::set_format(ValueFormat format)
    {
        if (format_ == format)
        {
            return;
        }
        format_ = format;
        emit repaintRequested();
    }

    TextEncoding MemoryViewDocument::encoding() const noexcept
    {
        return encoding_;
    }

    void MemoryViewDocument::set_encoding(TextEncoding encoding)
    {
        if (encoding_ == encoding)
        {
            return;
        }
        encoding_ = encoding;
        emit repaintRequested();
    }

    void MemoryViewDocument::set_modules(std::vector<process::ModuleInfo> modules)
    {
        ui::ModuleSpans spans;
        spans.set_modules(modules);
        if (spans == module_spans_)
        {
            return;
        }
        module_spans_ = std::move(spans);
        emit repaintRequested();
    }

    void MemoryViewDocument::set_address_mode(ui::AddressMode mode)
    {
        if (address_mode_ == mode)
        {
            return;
        }
        address_mode_ = mode;
        emit repaintRequested();
    }

    QString MemoryViewDocument::address_text(std::uint64_t address) const
    {
        return ui::format_pane_address(address_mode_, module_spans_, address);
    }

    QString MemoryViewDocument::display_text(std::uint64_t address) const
    {
        return ui::format_cell_address(address_mode_, module_spans_, address);
    }

    const ui::ModuleSpans& MemoryViewDocument::module_spans() const
    {
        return module_spans_;
    }

    std::uint64_t MemoryViewDocument::window_extent() const noexcept
    {
        return static_cast<std::uint64_t>(bytes_per_row_) * static_cast<std::uint64_t>(visible_rows_);
    }

    std::uint64_t MemoryViewDocument::visible_block_base() const noexcept
    {
        return align_down(first_byte_, window_extent());
    }

    std::uint64_t MemoryViewDocument::block_base_for(std::uint64_t address) const noexcept
    {
        return align_down(address, window_extent());
    }

    const Block* MemoryViewDocument::block_for(std::uint64_t address) const noexcept
    {
        const auto entry = blocks_.find(block_base_for(address));
        return entry == blocks_.end() ? nullptr : &entry->second;
    }

    bool MemoryViewDocument::byte_at(std::uint64_t address, std::byte& value) const
    {
        const Block* block = block_for(address);
        if (block == nullptr)
        {
            return false;
        }
        const std::uint64_t offset = address - block->base;
        if (offset >= block->readable.size() || offset >= block->bytes.size() || !block->readable[offset])
        {
            return false;
        }
        value = block->bytes[offset];
        return true;
    }

    void MemoryViewDocument::clear_pending()
    {
        pending_bases_.fill(std::nullopt);
        requested_pid_ = 0;
    }

    void MemoryViewDocument::prune_blocks(std::uint64_t visible_base)
    {
        const std::uint64_t extent = window_extent();
        std::erase_if(blocks_,
                      [&](const auto& entry)
                      {
                          const std::uint64_t base = entry.first;
                          return base != visible_base && !(visible_base >= extent && base == visible_base - extent)
                              && !(base == visible_base + extent);
                      });
    }

    std::vector<ui::LiveRequest> MemoryViewDocument::next_live_request()
    {
        clear_pending();
        if (!visible_ || !target_.valid())
        {
            return {};
        }

        const std::uint64_t extent       = window_extent();
        const std::uint64_t visible_base = visible_block_base();
        requested_pid_                   = target_.pid;

        std::vector<ui::LiveRequest> requests;
        requests.reserve(3);

        const auto add = [&](std::size_t slot, std::uint64_t base)
        {
            const std::size_t size = clamp_size(base, extent);
            if (size == 0)
            {
                return;
            }
            pending_bases_[slot] = PendingBlock {.base = base, .size = size};
            requests.push_back(ui::LiveRequest {.id = slot, .address = base, .size = size});
        };

        if (visible_base >= extent)
        {
            add(0, visible_base - extent);
        }
        add(1, visible_base);
        if (scan::kMaxUserAddress - visible_base >= extent)
        {
            add(2, visible_base + extent);
        }
        return requests;
    }

    void MemoryViewDocument::apply_live_readings(std::span<const ui::LiveReading> readings)
    {
        if (readings.empty() || !target_.valid() || target_.pid != requested_pid_)
        {
            return; // The target moved on while the pass was in flight.
        }

        bool dirty = false;
        for (const ui::LiveReading& reading : readings)
        {
            if (reading.id >= pending_bases_.size() || !pending_bases_[reading.id].has_value())
            {
                continue; // Not a block this pass submitted.
            }

            const PendingBlock& pending = *pending_bases_[reading.id];
            Block&              block   = blocks_[pending.base];
            block.base                  = pending.base;

            if (!reading.readable || reading.bytes.empty())
            {
                // Give the window the buffer it was submitted with (zero-filled)
                // so the failed bytes paint `?` instead of nothing at all.
                const auto size = static_cast<std::size_t>(pending.size);
                const bool grew = block.bytes.size() < size;
                if (grew)
                {
                    block.bytes.resize(size, std::byte {0});
                }
                const bool was_readable = std::ranges::any_of(block.readable,
                                                              [](bool readable)
                                                              {
                                                                  return readable;
                                                              });
                block.readable.assign(block.bytes.size(), false);
                block.changed.assign(block.bytes.size(), false);
                dirty = dirty || was_readable || grew;
                continue;
            }

            std::vector<bool> changed(reading.bytes.size(), false);
            for (std::size_t index = 0; index < reading.bytes.size(); ++index)
            {
                const bool was_readable = block.ever_read && index < block.readable.size() && block.readable[index];
                changed[index] =
                    was_readable && index < block.bytes.size() && block.bytes[index] != reading.bytes[index];
            }

            if (!block.ever_read || block.bytes != reading.bytes || block.changed != changed
                || block.readable.size() != reading.bytes.size())
            {
                dirty = true;
            }
            block.bytes = reading.bytes;
            block.readable.assign(reading.bytes.size(), true);
            block.changed   = std::move(changed);
            block.ever_read = true;
        }

        if (dirty)
        {
            emit repaintRequested();
        }
    }

    MemoryViewDocument::Cell MemoryViewDocument::cell(std::uint64_t address) const
    {
        const std::size_t size = format_.size();
        Cell              result;
        const Block*      block = block_for(address);
        if (block == nullptr || size == 0)
        {
            return result;
        }

        const std::uint64_t offset = address - block->base;
        if (offset + size > block->bytes.size())
        {
            return result;
        }
        const auto start = static_cast<std::size_t>(offset);

        bool readable = true;
        bool changed  = false;
        for (std::size_t index = 0; index < size; ++index)
        {
            readable = readable && block->readable[start + index];
            changed  = changed || block->changed[start + index];
        }

        result.readable = readable;
        result.changed  = changed;
        if (readable)
        {
            result.text = to_qstring(scan::format_value(
                format_.type, std::span<const std::byte>(block->bytes.data() + start, size), format_.hex));
            // Hex values read without the `0x` prefix, so a row stays narrow.
            if (format_.hex && result.text.startsWith(QStringLiteral("0x")))
            {
                result.text.remove(0, 2);
            }
            return result;
        }

        if (format_.hex)
        {
            // Two `?` per unreadable byte, each readable byte's digits in their
            // own positions, walking the bytes most-significant first so a
            // partially readable value lines up with scan::format_value's digits.
            result.text.reserve(static_cast<qsizetype>(size) * 2);
            for (std::size_t index = size; index-- > 0;)
            {
                const std::size_t position = start + index;
                if (!block->readable[position])
                {
                    result.text += QStringLiteral("??");
                    continue;
                }
                QString byte_text = to_qstring(scan::format_value(
                    scan::ValueType::byte, std::span<const std::byte>(block->bytes.data() + position, 1), true));
                if (byte_text.startsWith(QStringLiteral("0x")))
                {
                    byte_text.remove(0, 2);
                }
                result.text += byte_text;
            }
            return result;
        }

        result.text = QStringLiteral("?");
        return result;
    }

    QString MemoryViewDocument::text_row(std::uint64_t row_address) const
    {
        const std::size_t count = bytes_per_row_;

        if (encoding_ == TextEncoding::ascii)
        {
            QString text;
            text.reserve(static_cast<qsizetype>(count));
            for (std::size_t index = 0; index < count; ++index)
            {
                std::byte value {};
                if (byte_at(row_address + index, value) && is_printable(value))
                {
                    text += QLatin1Char(static_cast<char>(std::to_integer<unsigned char>(value)));
                }
                else
                {
                    text += QLatin1Char('.');
                }
            }
            return text;
        }

        if (encoding_ == TextEncoding::utf8)
        {
            QByteArray buffer;
            buffer.reserve(static_cast<qsizetype>(count));
            for (std::size_t index = 0; index < count; ++index)
            {
                std::byte value {};
                if (byte_at(row_address + index, value))
                {
                    buffer.append(static_cast<char>(std::to_integer<unsigned char>(value)));
                }
                else
                {
                    buffer.append('.');
                }
            }
            QStringDecoder decoder(QStringDecoder::Utf8);
            QString        text = decoder(buffer);
            if (decoder.hasError())
            {
                text.replace(QChar::ReplacementCharacter, QLatin1Char('.'));
            }
            return text;
        }

        QString text;
        for (std::size_t index = 0; index + 1 < count; index += 2)
        {
            std::byte low {};
            std::byte high {};
            if (byte_at(row_address + index, low) && byte_at(row_address + index + 1, high))
            {
                const auto code =
                    static_cast<char16_t>(std::to_integer<unsigned>(low) | (std::to_integer<unsigned>(high) << 8));
                text += code == u'\0' ? QLatin1Char(' ') : QChar(code);
            }
            else
            {
                text += QLatin1Char('.');
            }
        }
        if (count % 2 != 0)
        {
            text += QLatin1Char('.');
        }
        return text;
    }

    void MemoryViewDocument::seed_written(std::uint64_t address, const std::vector<std::byte>& bytes)
    {
        const std::uint64_t base  = block_base_for(address);
        Block&              block = blocks_[base];
        block.base                = base;

        const auto        offset = static_cast<std::size_t>(address - base);
        const std::size_t need   = offset + bytes.size();
        if (block.bytes.size() < need)
        {
            block.bytes.resize(need, std::byte {0});
            block.readable.resize(need, false);
            block.changed.resize(need, false);
        }
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            block.bytes[offset + index]    = bytes[index];
            block.readable[offset + index] = true;
            block.changed[offset + index]  = false;
        }
        block.ever_read = true;
    }

    bool MemoryViewDocument::write_value(std::uint64_t address, const QString& text)
    {
        if (!target_.valid())
        {
            log::warning(log::category::ui, "memory view write refused: no target attached");
            return false;
        }
        if (write_pending_.has_value())
        {
            log::warning(log::category::ui, "memory view write refused: a write is already in progress");
            return false;
        }

        const std::size_t size = format_.size();
        if (size == 0)
        {
            log::warning(log::category::ui, "memory view write refused: unsupported value type");
            return false;
        }

        const std::string input  = text.toStdString();
        // Parse here as well so a malformed value keeps its detailed message.
        const auto        parsed = scan::parse_value(format_.type, input, format_.hex);
        if (!parsed)
        {
            log::warning(log::category::ui, std::format("memory view value rejected: {}", parsed.error().message));
            return false;
        }
        const std::vector<std::byte> encoded = scan::encode_value(format_.type, *parsed);
        if (encoded.size() != size)
        {
            log::warning(log::category::ui, "memory view write refused: invalid value");
            return false;
        }

        const process::JobId job_id = worker_.next_job_id();
        write_pending_              = job_id;

        std::vector<std::byte> submitted_bytes = encoded;
        const bool             submitted       = worker_.submit_write(
            job_id,
            address,
            address,
            std::move(submitted_bytes),
            [this, address, job_id, cached = encoded](process::JobResult&& result)
            {
                if (write_pending_ != job_id)
                {
                    return; // Superseded or shut down.
                }
                write_pending_.reset();

                const auto& write = std::get<process::WriteResult>(result);
                if (write.error)
                {
                    log::warning(log::category::ui,
                                 std::format("memory view write failed: {}", process::describe(*write.error)));
                    emit repaintRequested();
                    return;
                }

                // Show what was just written without raising the change
                // highlight, so the user's own write is not flagged as a move.
                seed_written(address, cached);
                log::info(log::category::ui,
                          std::format("memory view wrote {} byte(s) at {:#x}", cached.size(), address));
                emit repaintRequested();
            });
        if (!submitted)
        {
            write_pending_.reset();
            log::warning(log::category::ui, "memory view write refused: the access worker is not accepting jobs");
            return false;
        }
        return true;
    }

} // namespace slopkit::ui::components
