#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "scan/types.hpp"
#include "scan/value.hpp"
#include "script/symbols.hpp"
#include "ui/address_format.hpp"
#include "ui/live_values.hpp"

#include <QObject>
#include <QString>

namespace slopkit::ui::components
{
    struct ValueFormat
    {
        scan::ValueType type {scan::ValueType::byte};
        bool            hex {true};

        [[nodiscard]] std::size_t size() const noexcept
        {
            return scan::value_size(type);
        }

        [[nodiscard]] bool operator==(const ValueFormat&) const = default;
    };

    enum class TextEncoding
    {
        ascii,
        utf8,
        utf16,
    };

    struct Block
    {
        std::uint64_t          base {};
        std::vector<std::byte> bytes;
        std::vector<bool>      readable;
        std::vector<bool>      changed;
        bool                   ever_read {false};
    };

    class MemoryViewDocument : public QObject, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        MemoryViewDocument(process::AccessWorker&   worker,
                           process::AttachedTarget& target,
                           QObject*                 parent  = nullptr,
                           script::SymbolTable&     symbols = script::default_symbol_table());

        void set_view(std::uint64_t first_byte, std::size_t bytes_per_row, std::size_t visible_rows);
        [[nodiscard]] std::uint64_t first_byte() const noexcept;
        [[nodiscard]] std::size_t   bytes_per_row() const noexcept;
        [[nodiscard]] std::size_t   visible_rows() const noexcept;

        void               set_visible(bool visible);
        [[nodiscard]] bool visible() const noexcept;

        [[nodiscard]] ValueFormat  format() const noexcept;
        void                       set_format(ValueFormat format);
        [[nodiscard]] TextEncoding encoding() const noexcept;
        void                       set_encoding(TextEncoding encoding);

        void                                 set_modules(std::vector<process::ModuleInfo> modules);
        void                                 set_address_mode(ui::AddressMode mode);
        [[nodiscard]] QString                address_text(std::uint64_t address) const;
        [[nodiscard]] QString                full_address_text(std::uint64_t address) const;
        [[nodiscard]] QString                display_text(std::uint64_t address) const;
        [[nodiscard]] const ui::ModuleSpans& module_spans() const;
        [[nodiscard]] script::SymbolTable&   symbols() const;

        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

        struct Cell
        {
            QString text;
            bool    readable {false};
            bool    changed {false};
        };

        [[nodiscard]] Cell    cell(std::uint64_t address) const;
        [[nodiscard]] QString text_row(std::uint64_t row_address) const;

        // Parses the text for the cell's format and writes it at `address`; a
        // refusal answers with the reason to show on the view's status line and
        // keeps the log record it writes today.
        std::expected<void, QString> write_value(std::uint64_t address, const QString& text);

    signals:
        void repaintRequested();

    private:
        struct PendingBlock
        {
            std::uint64_t base {};
            std::uint64_t size {};
        };

        [[nodiscard]] std::uint64_t window_extent() const noexcept;
        [[nodiscard]] std::uint64_t visible_block_base() const noexcept;
        [[nodiscard]] std::uint64_t block_base_for(std::uint64_t address) const noexcept;
        [[nodiscard]] const Block*  block_for(std::uint64_t address) const noexcept;
        [[nodiscard]] bool          byte_at(std::uint64_t address, std::byte& value) const;
        void                        clear_pending();
        void                        prune_blocks(std::uint64_t visible_base);
        void                        seed_written(std::uint64_t address, const std::vector<std::byte>& bytes);

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        script::SymbolTable&     symbols_;

        std::uint64_t first_byte_ {0};
        std::size_t   bytes_per_row_ {16};
        std::size_t   visible_rows_ {1};
        bool          visible_ {true};

        ValueFormat     format_ {};
        TextEncoding    encoding_ {TextEncoding::ascii};
        ui::ModuleSpans module_spans_;
        ui::AddressMode address_mode_ {ui::AddressMode::module_relative};

        std::map<std::uint64_t, Block> blocks_;

        std::array<std::optional<PendingBlock>, 3> pending_bases_ {};
        std::optional<std::uint64_t>               last_window_base_;
        process::ProcessId                         requested_pid_ {0};

        std::optional<process::JobId> write_pending_;
    };

}
