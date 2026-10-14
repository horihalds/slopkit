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

        void set_modules(std::vector<process::ModuleInfo> modules);

        void set_address_mode(AddressMode mode);

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

        // How the row's value reads right now: the block that is actually read
        // answered, did not answer, or has not been asked yet.
        [[nodiscard]] LiveState live_state(const ReclassRow& row) const;

        // The node's address in the target, when its block is known.
        [[nodiscard]] std::optional<std::uint64_t> node_address(const ReclassRow& row) const;

        // The bytes the node was last read from, empty when its block was not
        // read; the dissect command guesses a kind from them.
        [[nodiscard]] std::span<const std::byte> node_bytes(const ReclassRow& row) const;

        // Whether the address lies inside a loaded module image.
        [[nodiscard]] bool inside_module(std::uint64_t address) const;

        // The address a class is pinned to, once its expression resolved; the
        // project's classes are re-read first.
        [[nodiscard]] std::optional<std::uint64_t> class_address(std::size_t class_index);

        // An address spelled the way the address column and the tooltips spell
        // it.
        [[nodiscard]] QString format_address(std::uint64_t address) const;

        // Whether a value can be typed into this node's cell.
        [[nodiscard]] bool editable(const ReclassRow& row) const;

        // Parses the text for the node's kind and writes one value into the
        // target; the outcome is reported on statusChanged.
        bool write_value(const ReclassRow& row, const QString& text);

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

        // The bytes a block is read from, where the block starts inside them and
        // the address that start sits at.
        struct BlockView
        {
            std::span<const std::byte> bytes;
            std::size_t                start {};
            std::uint64_t              base {};
            bool                       readable {};
        };

        void sync_classes();

        [[nodiscard]] const ClassAddress* class_address_at(std::size_t index) const;

        // Where a block sits in the target: the class's resolved address, or a
        // pointer's value read from the previous pass's parent bytes.
        [[nodiscard]] std::optional<std::uint64_t> block_base(std::size_t block) const;

        [[nodiscard]] BlockView block_view(std::size_t block) const;

        [[nodiscard]] std::span<const std::byte> block_bytes(std::size_t block) const;

        [[nodiscard]] const reclass::Node* node_of(const ReclassRow& row) const;

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        ReclassStructureModel*   structure_ {};
        ModuleSpans              modules_;
        AddressMode              address_mode_ {AddressMode::module_relative};

        std::vector<ClassAddress> classes_;
        LiveCells                 cells_;

        std::optional<process::JobId>         resolve_job_;
        std::optional<process::JobId>         write_job_;
        std::string                           resolve_signature_;
        bool                                  force_resolve_ {true};
        std::chrono::steady_clock::time_point last_resolve_ {};
    };

}
