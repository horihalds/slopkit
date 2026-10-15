#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "reclass/node.hpp"
#include "reclass/value.hpp"
#include "ui/address_format.hpp"
#include "ui/live_values.hpp"
#include "ui/models/live_cells.hpp"
#include "ui/models/reclass_structure_model.hpp"

namespace slopkit::ui::models
{
    // The live half of the node grid: it resolves every class's address
    // expression, submits one read per visible block and slices the bytes back
    // into the node values. Nothing is submitted while no target is attached.
    class ReclassValueModel : public QObject
    {
        Q_OBJECT

    public:
        // How a row's value reads right now, in the address list's vocabulary:
        // a block the target answered, one it did not, and one it has not been
        // asked about yet.
        enum class LiveState
        {
            none,
            readable,
            unreadable,
        };

        ReclassValueModel(process::AccessWorker& worker, process::AttachedTarget& target, QObject* parent = nullptr);

        void set_structure(ReclassStructureModel& structure);

        // The memory map: the modules the classes' expressions resolve against
        // and the regions the pointer field tags an address with.
        void set_modules(std::vector<process::ModuleInfo> modules, std::vector<process::RegionInfo> regions);

        // The rows changed shape: the readings belong to the old shape, while
        // the resolved addresses stay because they are keyed by class id.
        void invalidate();

        // At most one batched resolve job, on a one-second cadence, while a
        // target is attached.
        void maybe_resolve_addresses();

        [[nodiscard]] std::vector<LiveRequest> requests();

        void apply_readings(std::span<const LiveReading> readings);

        [[nodiscard]] QString address_text(const ReclassRow& row) const;

        [[nodiscard]] QString value_text(const ReclassRow& row) const;

        // The value cell's text as the runs the grid paints: a hex node's own
        // bytes, a kind that draws a value of its own behind the grid's `= `
        // marker, and nothing for a pointer or a container. `value_text` is the
        // runs' concatenation.
        [[nodiscard]] std::vector<reclass::ValuePart> value_parts(const ReclassRow& row) const;

        // The readings a hex32/hex64 cell draws beside its bytes, as the hints
        // field shows them: the float, the integer and the raw bits in
        // parentheses. Empty for every other kind.
        [[nodiscard]] std::vector<reclass::ValuePart> hint_parts(const ReclassRow& row) const;
        [[nodiscard]] QString                         hint_text(const ReclassRow& row) const;

        // The pointer field: the address a pointer node holds, or the address a
        // hex cell's bytes plausibly name. Empty when the value is not an
        // address at all.
        [[nodiscard]] QString pointer_text(const ReclassRow& row) const;

        // The row's own bytes as text: a printable byte as itself, every other
        // byte as '.', an empty cell for a kind without bytes and '?' for a
        // block that was not read or with no target attached.
        [[nodiscard]] QString ascii_text(const ReclassRow& row) const;

        // The same value as the editor is prefilled with: the value a write
        // parses, without the readings the cell draws beside it.
        [[nodiscard]] QString edit_text(const ReclassRow& row) const;

        // How the row's value reads right now: the block that is actually read
        // answered, did not answer, or has not been asked yet.
        [[nodiscard]] LiveState live_state(const ReclassRow& row) const;

        // The node's address in the target, when its block is known.
        [[nodiscard]] std::optional<std::uint64_t> node_address(const ReclassRow& row) const;

        // The bytes the node was last read from, empty when its block was not
        // read; the dissect command guesses a kind from them.
        [[nodiscard]] std::span<const std::byte> node_bytes(const ReclassRow& row) const;

        // One flag per byte of `node_bytes(row)`: which of the bytes read at the
        // last pass differ from the pass before; empty when the block was not
        // read or there is nothing to compare the reading against.
        [[nodiscard]] std::span<const std::uint8_t> node_changed_bytes(const ReclassRow& row) const;

        // Whether the address lies inside a readable mapped region, which the
        // dissect command uses to tell a pointer it can dereference from one it
        // has to probe first.
        [[nodiscard]] bool inside_region(std::uint64_t address) const;

        // The address a class is pinned to, once its expression resolved; the
        // project's classes are re-read first.
        [[nodiscard]] std::optional<std::uint64_t> class_address(std::size_t class_index);

        // The address as the grid spells it: the shortest upper-case digits,
        // with no `0x` and no zero padding.
        [[nodiscard]] QString short_address(std::uint64_t address) const;

        // The padded absolute form the window's Copy writes, and nothing in a
        // row draws.
        [[nodiscard]] QString format_address(std::uint64_t address) const;

        // Whether a value can be typed into this node's cell.
        [[nodiscard]] bool editable(const ReclassRow& row) const;

        // Whether one byte of this node can be typed into: a hex node's dump and
        // any typed-into node's Ascii characters, while the node's own bytes are
        // the row's and the block was read.
        [[nodiscard]] bool byte_editable(const ReclassRow& row) const;

        // Parses the text for the node's kind and writes one value into the
        // target; the outcome is reported on statusChanged.
        bool write_value(const ReclassRow& row, const QString& text);

        // Parses one byte and writes it at `byte_offset` inside the node's own
        // bytes, leaving the node's other bytes alone; the outcome is reported
        // on statusChanged.
        bool write_byte(const ReclassRow& row, std::size_t byte_offset, const QString& text);

        [[nodiscard]] bool attached() const;

    signals:
        // A write's outcome, or a refusal, for the window's status line.
        void statusChanged(const QString& message, bool error);

        // The class addresses have been refreshed.
        void addressesChanged();

        // The block bytes have been refreshed: the live cells may repaint.
        void valuesChanged();

    private:
        struct ClassAddress
        {
            std::uint64_t                id {};
            std::string                  expression;
            std::optional<std::uint64_t> address;

            bool operator==(const ClassAddress&) const = default;
        };

        // The bytes a block is read from, which of them moved since the last
        // pass, where the block starts inside them and the address that start
        // sits at.
        struct BlockView
        {
            std::span<const std::byte>    bytes;
            std::span<const std::uint8_t> changed;
            std::size_t                   start {};
            std::uint64_t                 base {};
            bool                          readable {};
        };

        void sync_classes();

        [[nodiscard]] const ClassAddress* class_address_at(std::size_t index) const;

        // Where a block sits in the target: the class's resolved address, or a
        // pointer's value read from the previous pass's parent bytes.
        [[nodiscard]] std::optional<std::uint64_t> block_base(std::size_t block) const;

        [[nodiscard]] BlockView block_view(std::size_t block) const;

        [[nodiscard]] std::span<const std::byte> block_bytes(std::size_t block) const;

        [[nodiscard]] std::span<const std::uint8_t> block_changed(std::size_t block) const;

        [[nodiscard]] const reclass::Node* node_of(const ReclassRow& row) const;

        // Where a node's bytes lie inside the cached bytes of a block that was
        // read: the reading they belong to and the offset in it.
        struct CachedRange
        {
            std::size_t cell {};
            std::size_t index {};
        };

        [[nodiscard]] std::optional<CachedRange> cached_range(const ReclassRow& row, std::size_t byte_offset) const;

        // The grid paints the last reading, so a write patches the bytes it
        // wrote there and repaints at once; the next live pass still corrects a
        // write the target took without saying so.
        void patch_cached_bytes(const CachedRange& range, std::span<const std::byte> bytes);

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        ReclassStructureModel*   structure_ {};
        ModuleSpans              modules_;
        RegionSpans              regions_;

        std::vector<ClassAddress> classes_;
        LiveCells                 cells_;

        std::optional<process::JobId>         resolve_job_;
        std::optional<process::JobId>         write_job_;
        std::string                           resolve_signature_;
        bool                                  force_resolve_ {true};
        std::chrono::steady_clock::time_point last_resolve_ {};
    };

}
