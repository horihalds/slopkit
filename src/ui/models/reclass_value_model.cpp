#include "ui/models/reclass_value_model.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "reclass/value.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::models
{
    namespace
    {
        constexpr auto kResolveInterval = std::chrono::seconds(1);
    }

    ReclassValueModel::ReclassValueModel(process::AccessWorker&   worker,
                                         process::AttachedTarget& target,
                                         QObject*                 parent)
        : QObject(parent), worker_(worker), target_(target)
    {
    }

    void ReclassValueModel::set_structure(ReclassStructureModel& structure)
    {
        structure_ = &structure;
        invalidate();
    }

    void ReclassValueModel::set_modules(std::vector<process::ModuleInfo> modules)
    {
        modules_.set_modules(modules);
        force_resolve_ = true;
    }

    void ReclassValueModel::set_address_mode(AddressMode mode)
    {
        address_mode_ = mode;
        emit valuesChanged();
    }

    void ReclassValueModel::invalidate()
    {
        cells_.clear();
        force_resolve_ = true;
    }

    bool ReclassValueModel::attached() const
    {
        return target_.valid();
    }

    void ReclassValueModel::sync_classes()
    {
        if (structure_ == nullptr)
        {
            return;
        }

        std::vector<ClassAddress>          updated;
        const std::vector<reclass::Class>& project_classes = structure_->project().classes();
        updated.reserve(project_classes.size());
        for (const reclass::Class& type : project_classes)
        {
            ClassAddress entry;
            entry.id         = type.id;
            entry.expression = type.address_expression;

            const auto previous = std::ranges::find_if(classes_,
                                                       [&](const ClassAddress& candidate)
                                                       {
                                                           return candidate.id == type.id;
                                                       });
            if (previous != classes_.end() && previous->expression == entry.expression)
            {
                entry.address = previous->address;
            }
            updated.push_back(std::move(entry));
        }

        if (updated != classes_)
        {
            classes_       = std::move(updated);
            force_resolve_ = true;
        }
    }

    const ReclassValueModel::ClassAddress* ReclassValueModel::class_address_at(std::size_t index) const
    {
        return index < classes_.size() ? &classes_[index] : nullptr;
    }

    std::optional<std::uint64_t> ReclassValueModel::class_address(std::size_t class_index)
    {
        sync_classes();
        const ClassAddress* type = class_address_at(class_index);
        return type == nullptr ? std::nullopt : type->address;
    }

    QString ReclassValueModel::format_address(std::uint64_t address) const
    {
        return format_cell_address_full(address_mode_, modules_, address);
    }

    bool ReclassValueModel::editable(const ReclassRow& row) const
    {
        if (!attached())
        {
            return false;
        }
        const reclass::Node* node = node_of(row);
        if (node == nullptr || !reclass::is_editable(node->kind))
        {
            return false;
        }
        return block_view(row.block).readable && node_address(row).has_value();
    }

    bool ReclassValueModel::write_value(const ReclassRow& row, const QString& text)
    {
        const reclass::Node* node = node_of(row);
        if (node == nullptr)
        {
            return false;
        }
        if (!attached())
        {
            emit statusChanged(tr("Not attached; cannot write."), true);
            return false;
        }
        if (!reclass::is_editable(node->kind))
        {
            emit statusChanged(
                tr("%1 values cannot be written from the grid.").arg(to_qstring(reclass::describe(node->kind))), true);
            return false;
        }
        if (write_job_.has_value())
        {
            emit statusChanged(tr("A write is already in progress."), true);
            return false;
        }

        const std::optional<std::uint64_t> address = node_address(row);
        if (!address.has_value())
        {
            emit statusChanged(tr("The node's address is unknown; cannot write."), true);
            return false;
        }
        if (!block_view(row.block).readable)
        {
            emit statusChanged(tr("The node's block was not read; cannot write."), true);
            return false;
        }

        const auto parsed = reclass::parse_node_value(node->kind, text.toStdString(), node->hex);
        if (!parsed)
        {
            emit statusChanged(tr("Value: %1").arg(to_qstring(parsed.error())), true);
            return false;
        }

        const process::JobId job_id = worker_.next_job_id();
        write_job_                  = job_id;

        const bool submitted = worker_.submit_write(
            job_id,
            row.node_id,
            *address,
            *parsed,
            [this, job_id](process::JobResult&& result)
            {
                if (write_job_ != job_id)
                {
                    return;
                }
                write_job_.reset();

                if (!std::holds_alternative<process::WriteResult>(result))
                {
                    emit statusChanged(tr("The write did not complete."), true);
                    return;
                }
                if (const auto& written = std::get<process::WriteResult>(result); written.error.has_value())
                {
                    emit statusChanged(tr("Write failed: %1").arg(to_qstring(process::describe(*written.error))), true);
                    return;
                }
                emit statusChanged(tr("Value written."), false);
            });
        if (!submitted)
        {
            write_job_.reset();
            emit statusChanged(tr("The write was refused."), true);
            return false;
        }
        return true;
    }

    void ReclassValueModel::maybe_resolve_addresses()
    {
        sync_classes();
        if (structure_ == nullptr || !attached() || resolve_job_.has_value())
        {
            return;
        }

        std::vector<process::ResolveRequest> requests;
        std::string                          signature;
        for (const ClassAddress& type : classes_)
        {
            if (type.expression.empty())
            {
                continue;
            }
            requests.push_back(process::ResolveRequest {.key = type.id, .expression = type.expression});
            signature += std::to_string(type.id);
            signature += ':';
            signature += type.expression;
            signature += ';';
        }

        const auto now = std::chrono::steady_clock::now();
        if (requests.empty())
        {
            resolve_signature_.clear();
            force_resolve_ = false;
            return;
        }
        if (!force_resolve_ && signature == resolve_signature_ && now - last_resolve_ < kResolveInterval)
        {
            return;
        }
        force_resolve_     = false;
        resolve_signature_ = signature;
        last_resolve_      = now;

        const process::JobId id = worker_.next_job_id();
        resolve_job_            = id;

        const auto submitted_requests = requests;
        const bool submitted          = worker_.submit_resolve_expressions(
            id,
            std::move(requests),
            module_refs(modules_),
            8,
            [this, id, submitted_requests](process::JobResult&& result)
            {
                if (!resolve_job_.has_value() || *resolve_job_ != id)
                {
                    return;
                }
                resolve_job_.reset();

                if (!std::holds_alternative<process::ResolveResult>(result))
                {
                    return;
                }
                const auto& resolved = std::get<process::ResolveResult>(result);
                if (resolved.error.has_value())
                {
                    return;
                }

                bool changed = false;
                for (const process::ResolveItemResult& item : resolved.items)
                {
                    const auto request = std::ranges::find_if(submitted_requests,
                                                              [&](const process::ResolveRequest& candidate)
                                                              {
                                                                  return candidate.key == item.key;
                                                              });
                    if (request == submitted_requests.end())
                    {
                        continue;
                    }

                    const auto type = std::ranges::find_if(classes_,
                                                           [&](const ClassAddress& candidate)
                                                           {
                                                               return candidate.id == item.key;
                                                           });
                    if (type == classes_.end() || type->expression != request->expression)
                    {
                        continue;
                    }
                    if (type->address != item.address)
                    {
                        type->address = item.address;
                        changed       = true;
                    }
                }

                if (changed)
                {
                    emit addressesChanged();
                    emit valuesChanged();
                }
            });
        if (!submitted)
        {
            resolve_job_.reset();
        }
    }

    std::span<const std::byte> ReclassValueModel::block_bytes(std::size_t block) const
    {
        const LiveCell* cell = cells_.at(block);
        if (cell == nullptr || !cell->has_reading || !cell->readable || !cell->bytes.has_value())
        {
            return {};
        }
        return *cell->bytes;
    }

    std::optional<std::uint64_t> ReclassValueModel::block_base(std::size_t index) const
    {
        if (structure_ == nullptr)
        {
            return std::nullopt;
        }
        const std::vector<ReclassBlock>& blocks = structure_->blocks();
        if (index >= blocks.size())
        {
            return std::nullopt;
        }

        const ReclassBlock& block = blocks[index];
        if (index == 0)
        {
            const ClassAddress* type = class_address_at(block.class_index);
            return type == nullptr ? std::nullopt : type->address;
        }

        const std::size_t start  = block.base_offset - blocks[block.parent_block].base_offset;
        const auto        parent = block_base(block.parent_block);
        if (!parent.has_value())
        {
            return std::nullopt;
        }
        if (!block.via_pointer)
        {
            return *parent + start;
        }

        // A pointer block sits wherever the pointer's own bytes point, which
        // are read from the parent's previous pass.
        const std::span<const std::byte> bytes = block_bytes(block.parent_block);
        if (bytes.size() < start + 8)
        {
            return std::nullopt;
        }
        std::uint64_t target = 0;
        std::memcpy(&target, bytes.data() + start, sizeof(target));
        return target == 0 ? std::nullopt : std::optional<std::uint64_t> {target};
    }

    ReclassValueModel::BlockView ReclassValueModel::block_view(std::size_t index) const
    {
        const auto base = block_base(index);
        if (!base.has_value() || structure_ == nullptr)
        {
            return {};
        }

        const std::vector<ReclassBlock>& blocks = structure_->blocks();
        if (index == 0)
        {
            const std::span<const std::byte> bytes = block_bytes(0);
            return BlockView {.bytes = bytes, .start = 0, .base = *base, .readable = !bytes.empty()};
        }

        const ReclassBlock& block = blocks[index];
        const std::size_t   start = block.base_offset - blocks[block.parent_block].base_offset;

        if (block.via_pointer)
        {
            const std::span<const std::byte> bytes = block_bytes(index);
            return BlockView {.bytes = bytes, .start = 0, .base = *base, .readable = !bytes.empty()};
        }

        // A class instance lives inside its parent's memory.
        const BlockView parent = block_view(block.parent_block);
        if (!parent.readable || parent.start + start >= parent.bytes.size())
        {
            return BlockView {.bytes = {}, .start = 0, .base = *base, .readable = false};
        }
        return BlockView {.bytes = parent.bytes, .start = parent.start + start, .base = *base, .readable = true};
    }

    std::vector<LiveRequest> ReclassValueModel::requests()
    {
        sync_classes();
        if (structure_ == nullptr || !attached())
        {
            cells_.clear();
            return {};
        }

        const std::vector<ReclassBlock>& blocks = structure_->blocks();
        if (blocks.empty())
        {
            cells_.clear();
            return {};
        }

        return cells_.request(blocks.size(),
                              [this](std::size_t block)
                              {
                                  // A candidate that is not readable asks for no
                                  // read at all, whatever its address fields say.
                                  const LiveCandidate skip {.address = 0, .size = 0, .identity = 0, .readable = false};

                                  const std::vector<ReclassBlock>& current = structure_->blocks();
                                  if (block >= current.size())
                                  {
                                      return skip;
                                  }

                                  const ReclassBlock& info = current[block];
                                  // A class instance shares its parent's bytes, so
                                  // only the class itself and the pointer blocks
                                  // are read.
                                  if (info.size == 0 || (!info.via_pointer && block != 0))
                                  {
                                      return skip;
                                  }

                                  const auto base = block_base(block);
                                  if (!base.has_value())
                                  {
                                      return skip;
                                  }

                                  return LiveCandidate {
                                      .address = *base, .size = info.size, .identity = *base, .readable = true};
                              });
    }

    void ReclassValueModel::apply_readings(std::span<const LiveReading> readings)
    {
        if (structure_ == nullptr)
        {
            return;
        }
        cells_.apply(
            readings,
            [this](std::size_t block, std::uint64_t identity)
            {
                // The block still sits where it did when it was asked
                // for; a pointer that moved drops its stale reading.
                const auto base = block_base(block);
                return base.has_value() && *base == identity;
            },
            [](std::size_t, std::size_t) {});
        emit valuesChanged();
    }

    const reclass::Node* ReclassValueModel::node_of(const ReclassRow& row) const
    {
        if (structure_ == nullptr)
        {
            return nullptr;
        }
        const reclass::Class* type = structure_->project().class_at(row.class_index);
        if (type == nullptr || row.node_index >= type->nodes.size())
        {
            return nullptr;
        }
        return &type->nodes[row.node_index];
    }

    std::optional<std::uint64_t> ReclassValueModel::node_address(const ReclassRow& row) const
    {
        if (structure_ == nullptr)
        {
            return std::nullopt;
        }
        const std::vector<ReclassBlock>& blocks = structure_->blocks();
        if (row.block >= blocks.size())
        {
            return std::nullopt;
        }
        const auto base = block_base(row.block);
        if (!base.has_value())
        {
            return std::nullopt;
        }
        return *base + (row.offset - blocks[row.block].base_offset);
    }

    std::span<const std::byte> ReclassValueModel::node_bytes(const ReclassRow& row) const
    {
        const reclass::Node* node = node_of(row);
        if (node == nullptr || structure_ == nullptr)
        {
            return {};
        }

        const std::size_t size = reclass::kind_size(node->kind);
        const BlockView   view = block_view(row.block);
        if (size == 0 || !view.readable)
        {
            return {};
        }

        const std::vector<ReclassBlock>& blocks = structure_->blocks();
        const std::size_t                index  = view.start + row.offset - blocks[row.block].base_offset;
        if (index + size > view.bytes.size())
        {
            return {};
        }
        return view.bytes.subspan(index, size);
    }

    bool ReclassValueModel::inside_module(std::uint64_t address) const
    {
        return modules_.containing(address) != nullptr;
    }

    QString ReclassValueModel::address_text(const ReclassRow& row) const
    {
        if (!attached())
        {
            return tr("Attach to a target first.");
        }
        if (structure_ == nullptr)
        {
            return {};
        }

        const std::vector<ReclassBlock>& blocks = structure_->blocks();
        if (row.block >= blocks.size())
        {
            return {};
        }
        if (row.block == 0)
        {
            const ClassAddress* type = class_address_at(blocks[0].class_index);
            if (type == nullptr || !type->address.has_value())
            {
                return tr("not resolved yet");
            }
        }

        const auto address = node_address(row);
        if (!address.has_value())
        {
            return QStringLiteral("?");
        }
        return format_cell_address(address_mode_, modules_, *address);
    }

    QString ReclassValueModel::value_text(const ReclassRow& row) const
    {
        if (!attached())
        {
            return QStringLiteral("?");
        }

        const reclass::Node* node = node_of(row);
        if (node == nullptr || reclass::kind_size(node->kind) == 0)
        {
            return {};
        }

        const std::span<const std::byte> bytes = node_bytes(row);
        if (bytes.empty())
        {
            return QStringLiteral("?");
        }
        return to_qstring(reclass::format_node_value(node->kind, bytes, node->hex));
    }

}
