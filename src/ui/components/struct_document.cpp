#include "ui/components/struct_document.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "expr/expression.hpp"
#include "expr/resolver.hpp"
#include "ui/components/field_codec.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::components
{
    namespace
    {
        // Following a pointer chain is bounded so a linked structure cannot
        // expand without end; the ancestor set catches a pointer that loops.
        constexpr std::size_t kMaxFollowDepth = 8;
    }

    StructDocument::StructDocument(process::AccessWorker&   worker,
                                   process::AttachedTarget& target,
                                   structs::Structs&        definitions,
                                   QObject*                 parent,
                                   script::SymbolTable&     symbols)
        : QObject(parent), worker_(worker), target_(target), definitions_(definitions), symbols_(symbols)
    {
    }

    void StructDocument::set_visible(bool visible)
    {
        visible_ = visible;
    }

    bool StructDocument::visible() const noexcept
    {
        return visible_;
    }

    std::size_t StructDocument::pointer_size() const noexcept
    {
        return pointer_size_;
    }

    void StructDocument::set_modules(std::vector<process::ModuleInfo> modules)
    {
        pointer_size_ = 8;
        if (const process::ModuleInfo* main = process::main_module(modules); main != nullptr && main->pointer_size != 0)
        {
            pointer_size_ = main->pointer_size;
        }
        module_spans_.set_modules(modules);
        resolve_pending_ = true;
    }

    void StructDocument::set_address_mode(ui::AddressMode mode)
    {
        address_mode_ = mode;
    }

    const ui::ModuleSpans& StructDocument::module_spans() const
    {
        return module_spans_;
    }

    script::SymbolTable& StructDocument::symbols() const
    {
        return symbols_;
    }

    void StructDocument::refresh()
    {
        resolve_pending_ = true;
        emit valuesChanged();
    }

    bool StructDocument::followed(std::uint64_t id) const
    {
        return followed_.contains(id);
    }

    bool StructDocument::can_follow(std::uint64_t id) const
    {
        return !follow_block(id).has_value();
    }

    QString StructDocument::follow_refusal(std::uint64_t id) const
    {
        const auto reason = follow_block(id);
        return reason.has_value() ? *reason : QString {};
    }

    std::optional<QString> StructDocument::follow_block(std::uint64_t id) const
    {
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        const std::size_t                          row   = definitions_.index_of(id);
        if (row >= nodes.size())
        {
            return QStringLiteral("The field no longer exists.");
        }
        if (nodes[row].type.kind != structs::FieldKind::pointer)
        {
            return QStringLiteral("Only a pointer field can be followed.");
        }

        const std::vector<structs::FieldPlacement> placements = current_placements();
        std::set<std::uint64_t>                    path;
        if (row < placements.size() && placements[row].address != 0)
        {
            path.insert(placements[row].address);
        }

        std::size_t   depth  = 0;
        std::uint64_t parent = nodes[row].parent;
        std::size_t   guard  = nodes.size() + 1;
        while (parent != 0 && guard-- > 0)
        {
            const std::size_t parent_row = definitions_.index_of(parent);
            if (parent_row >= nodes.size())
            {
                break;
            }
            if (followed_.contains(parent))
            {
                ++depth;
                if (const auto value = pointer_value_.find(parent); value != pointer_value_.end())
                {
                    path.insert(value->second);
                }
            }
            if (nodes[parent_row].parent == 0)
            {
                path.insert(root_base(parent));
            }
            parent = nodes[parent_row].parent;
        }
        if (depth >= kMaxFollowDepth)
        {
            return QStringLiteral("Too many nested pointers to follow.");
        }
        if (const auto value = pointer_value_.find(id);
            value != pointer_value_.end() && value->second != 0 && path.contains(value->second))
        {
            return QStringLiteral("Refusing to follow a pointer that loops back into this chain.");
        }
        return std::nullopt;
    }

    void StructDocument::set_followed(std::uint64_t id, bool followed)
    {
        if (followed)
        {
            if (const auto reason = follow_block(id); reason.has_value())
            {
                emit statusChanged(*reason, true);
                return;
            }
            followed_.insert(id);
        }
        else
        {
            followed_.erase(id);
        }
        emit valuesChanged();
    }

    void StructDocument::set_expanded(std::uint64_t id, bool expanded)
    {
        if (expanded)
        {
            collapsed_.erase(id);
        }
        else
        {
            collapsed_.insert(id);
        }
        emit valuesChanged();
    }

    bool StructDocument::expanded(std::uint64_t id) const
    {
        return !collapsed_.contains(id);
    }

    std::uint64_t StructDocument::root_base(std::uint64_t root) const noexcept
    {
        const auto found = base_of_root_.find(root);
        return found == base_of_root_.end() ? 0 : found->second;
    }

    std::vector<std::uint64_t> StructDocument::pointer_bases() const
    {
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        std::vector<std::uint64_t>                 bases(nodes.size(), 0);
        for (std::size_t row = 0; row < nodes.size(); ++row)
        {
            if (nodes[row].type.kind != structs::FieldKind::pointer || !followed_.contains(nodes[row].id))
            {
                continue;
            }
            const auto value = pointer_value_.find(nodes[row].id);
            if (value != pointer_value_.end())
            {
                bases[row] = value->second;
            }
        }
        return bases;
    }

    std::vector<std::uint8_t> StructDocument::visible_mask() const
    {
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        std::vector<std::uint8_t>                  mask(nodes.size(), 1);
        for (std::size_t row = 0; row < nodes.size(); ++row)
        {
            std::uint64_t parent = nodes[row].parent;
            std::size_t   guard  = nodes.size() + 1;
            while (parent != 0 && guard-- > 0)
            {
                if (collapsed_.contains(parent))
                {
                    mask[row] = 0;
                    break;
                }
                const std::size_t parent_row = definitions_.index_of(parent);
                if (parent_row >= nodes.size())
                {
                    break;
                }
                parent = nodes[parent_row].parent;
            }
        }
        return mask;
    }

    std::vector<structs::FieldPlacement> StructDocument::current_placements() const
    {
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        std::vector<std::string_view>              names;
        std::vector<std::uint64_t>                 bases;
        for (const structs::StructNode& node : nodes)
        {
            if (node.parent == 0)
            {
                names.push_back(node.name);
                bases.push_back(root_base(node.id));
            }
        }
        return structs::place(definitions_, names, bases, pointer_bases(), pointer_size_);
    }

    bool StructDocument::fields_overlap(std::size_t row) const
    {
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        if (row >= nodes.size())
        {
            return false;
        }
        const std::size_t depth = definitions_.depth_of(row);

        std::vector<std::pair<std::uint64_t, std::uint64_t>> intervals;
        std::size_t                                          end = row + 1;
        while (end < nodes.size() && definitions_.depth_of(end) > depth)
        {
            if (definitions_.depth_of(end) == depth + 1)
            {
                const std::size_t size = nodes[end].type.size(pointer_size_);
                if (size != 0)
                {
                    intervals.emplace_back(nodes[end].offset, nodes[end].offset + size);
                }
            }
            ++end;
        }
        std::ranges::sort(intervals);
        for (std::size_t index = 1; index < intervals.size(); ++index)
        {
            if (intervals[index].first < intervals[index - 1].second)
            {
                return true;
            }
        }
        return false;
    }

    void StructDocument::ensure_bases()
    {
        if (!resolve_pending_ || resolve_job_.has_value() || !target_.valid())
        {
            return;
        }
        resolve_pending_ = false;

        const std::vector<expr::ModuleRef>   modules = ui::module_refs(module_spans_);
        const std::vector<expr::SymbolRef>   symbols = symbols_.snapshot();
        std::vector<process::ResolveRequest> requests;

        for (const structs::StructNode& node : definitions_.nodes())
        {
            if (node.parent != 0)
            {
                continue;
            }
            if (node.expression.empty())
            {
                base_of_root_[node.id] = 0;
                continue;
            }
            const auto parsed = expr::parse(node.expression);
            if (!parsed)
            {
                log::warning(log::category::ui, std::format("struct view: cannot parse '{}'", node.expression));
                base_of_root_[node.id] = 0;
                continue;
            }
            if (parsed->pointer_levels() == 0)
            {
                const auto value =
                    expr::evaluate(*parsed, modules, symbols, {}, expr::Options {.pointer_size = pointer_size_});
                base_of_root_[node.id] = value ? *value : 0;
                continue;
            }
            requests.push_back(process::ResolveRequest {.key = node.id, .expression = node.expression});
        }

        if (requests.empty())
        {
            return;
        }

        const process::JobId job_id = worker_.next_job_id();
        resolve_job_                = job_id;
        const bool submitted        = worker_.submit_resolve_expressions(job_id,
                                                                         std::move(requests),
                                                                         modules,
                                                                         pointer_size_,
                                                                         [this, job_id](process::JobResult&& result)
                                                                         {
                                                                      on_resolved(job_id, std::move(result));
                                                                         });
        if (!submitted)
        {
            resolve_job_.reset();
            resolve_pending_ = true;
            log::warning(log::category::ui, "struct view: the access worker is not accepting jobs");
        }
    }

    void StructDocument::on_resolved(process::JobId id, process::JobResult&& result)
    {
        if (resolve_job_ != id)
        {
            return;
        }
        resolve_job_.reset();

        const auto& resolved = std::get<process::ResolveResult>(result);
        if (resolved.error.has_value())
        {
            log::warning(log::category::ui,
                         std::format("struct view resolve failed: {}", process::describe(*resolved.error)));
            resolve_pending_ = true;
            return;
        }
        for (const process::ResolveItemResult& item : resolved.items)
        {
            // A failed resolve keeps the last good base so a transient read error
            // does not blank the whole struct.
            if (item.address.has_value())
            {
                base_of_root_[item.key] = *item.address;
            }
        }
        emit valuesChanged();
    }

    QString StructDocument::offset_text(std::uint64_t id) const
    {
        const std::size_t                          row   = definitions_.index_of(id);
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        if (row >= nodes.size() || nodes[row].parent == 0)
        {
            return {};
        }
        return QString::number(nodes[row].offset, 16).toUpper();
    }

    QString StructDocument::type_text(std::uint64_t id) const
    {
        const std::size_t                          row   = definitions_.index_of(id);
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        if (row >= nodes.size() || nodes[row].parent == 0)
        {
            return {};
        }
        return to_qstring(nodes[row].type.describe());
    }

    QString StructDocument::value_text(std::uint64_t id) const
    {
        const std::size_t                          row   = definitions_.index_of(id);
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        if (row >= nodes.size())
        {
            return {};
        }
        const structs::StructNode& node = nodes[row];
        if (node.parent == 0)
        {
            const std::uint64_t base = root_base(id);
            return base == 0 ? QStringLiteral("0x0") : format_cell_address(address_mode_, module_spans_, base);
        }
        if (node.type.kind == structs::FieldKind::structure)
        {
            return {};
        }
        const ui::models::LiveCell* cell = cells_.at(row);
        if (cell == nullptr || !cell->has_reading)
        {
            return {};
        }
        if (!cell->readable || !cell->bytes.has_value())
        {
            return QStringLiteral("?");
        }
        return field_codec::decode(node, *cell->bytes, pointer_size_, address_mode_, module_spans_);
    }

    QString StructDocument::describe(std::span<const std::uint64_t> ids) const
    {
        if (ids.empty())
        {
            return {};
        }
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        if (ids.size() == 1)
        {
            const std::size_t row = definitions_.index_of(ids.front());
            if (row >= nodes.size())
            {
                return {};
            }
            const structs::StructNode& node = nodes[row];
            if (node.parent == 0)
            {
                QString text =
                    QStringLiteral("%1 at %2, size 0x%3")
                        .arg(to_qstring(node.name),
                             format_cell_address(address_mode_, module_spans_, root_base(node.id)),
                             QString::number(definitions_.struct_size(node.id, pointer_size_), 16).toUpper());
                if (fields_overlap(row))
                {
                    text += QStringLiteral(" \u2014 fields overlap");
                }
                return text;
            }
            return QStringLiteral("%1: %2 at +0x%3")
                .arg(to_qstring(node.name),
                     to_qstring(node.type.describe()),
                     QString::number(node.offset, 16).toUpper());
        }
        return QStringLiteral("%1 nodes selected").arg(ids.size());
    }

    bool StructDocument::write_value(std::uint64_t id, const QString& text)
    {
        if (!target_.valid())
        {
            log::warning(log::category::ui, "struct view write refused: no target attached");
            emit statusChanged(QStringLiteral("Attach to a target first."), true);
            return false;
        }
        if (write_pending_.has_value())
        {
            log::warning(log::category::ui, "struct view write refused: a write is already in progress");
            emit statusChanged(QStringLiteral("A write is already in progress."), true);
            return false;
        }

        const std::size_t                          row   = definitions_.index_of(id);
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        if (row >= nodes.size())
        {
            return false;
        }
        const structs::StructNode& node    = nodes[row];
        const auto                 encoded = field_codec::encode(node, text, pointer_size_);
        if (!encoded.has_value())
        {
            log::warning(log::category::ui, "struct view write refused: the value does not fit the field");
            emit statusChanged(QStringLiteral("Cannot write that value into the field."), true);
            return false;
        }

        const std::vector<structs::FieldPlacement> placements = current_placements();
        if (row >= placements.size() || placements[row].address == 0)
        {
            log::warning(log::category::ui, "struct view write refused: the field has no resolved address");
            emit statusChanged(QStringLiteral("The field's address is not resolved."), true);
            return false;
        }
        const std::uint64_t address = placements[row].address;

        const process::JobId job_id = worker_.next_job_id();
        write_pending_              = job_id;
        const bool submitted        = worker_.submit_write(
            job_id,
            id,
            address,
            *encoded,
            [this, row, job_id, cached = *encoded](process::JobResult&& result)
            {
                if (write_pending_ != job_id)
                {
                    return;
                }
                write_pending_.reset();
                const auto& write = std::get<process::WriteResult>(result);
                if (write.error.has_value())
                {
                    const QString message =
                        QStringLiteral("Write failed: %1").arg(to_qstring(process::describe(*write.error)));
                    log::warning(log::category::ui,
                                 std::format("struct view write failed: {}", process::describe(*write.error)));
                    emit statusChanged(message, true);
                    return;
                }
                if (row < cells_.size())
                {
                    ui::models::LiveCell& cell = cells_.cell(row);
                    cell.bytes                 = cached;
                    cell.readable              = true;
                    cell.has_reading           = true;
                    cell.changed               = false;
                }
                emit statusChanged(QStringLiteral("Wrote %1 byte(s).").arg(cached.size()), false);
                emit valuesChanged();
            });
        if (!submitted)
        {
            write_pending_.reset();
            log::warning(log::category::ui, "struct view write refused: the access worker is not accepting jobs");
            emit statusChanged(QStringLiteral("The access worker is not accepting jobs."), true);
            return false;
        }
        return true;
    }

    std::vector<ui::LiveRequest> StructDocument::next_live_request()
    {
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        if (!visible_ || nodes.empty() || !target_.valid())
        {
            cells_.clear();
            return {};
        }
        if (target_.pid != requested_pid_)
        {
            requested_pid_   = target_.pid;
            resolve_pending_ = true;
        }
        ensure_bases();

        const std::vector<structs::FieldPlacement> placements = current_placements();
        const std::vector<std::uint8_t>            mask       = visible_mask();

        std::vector<ui::models::LiveCandidate> candidates(nodes.size());
        for (std::size_t row = 0; row < nodes.size(); ++row)
        {
            ui::models::LiveCandidate& candidate = candidates[row];
            candidate.identity                   = nodes[row].id;
            const structs::StructNode& node      = nodes[row];
            if (mask[row] == 0 || node.parent == 0 || node.type.kind == structs::FieldKind::structure)
            {
                candidate.readable = false;
                continue;
            }
            if (row >= placements.size() || placements[row].address == 0 || placements[row].size == 0)
            {
                candidate.readable = false;
                continue;
            }
            candidate.address  = placements[row].address;
            candidate.size     = placements[row].size;
            candidate.readable = true;
        }

        std::vector<ui::LiveRequest> requests = cells_.request(nodes.size(),
                                                               [&candidates](std::size_t row)
                                                               {
                                                                   return candidates[row];
                                                               });

        // A row that is no longer read (a collapsed subtree, an unfollowed
        // pointer) must not keep showing a stale value.
        for (std::size_t row = 0; row < cells_.size(); ++row)
        {
            if (candidates[row].readable)
            {
                continue;
            }
            ui::models::LiveCell& cell = cells_.cell(row);
            cell.has_reading           = false;
            cell.readable              = false;
            cell.changed               = false;
            cell.bytes.reset();
        }
        return requests;
    }

    void StructDocument::apply_live_readings(std::span<const ui::LiveReading> readings)
    {
        if (readings.empty() || !target_.valid() || target_.pid != requested_pid_)
        {
            return;
        }
        const std::span<const structs::StructNode> nodes = definitions_.nodes();
        cells_.apply(
            readings,
            [&](std::size_t row, std::uint64_t identity)
            {
                return row < nodes.size() && nodes[row].id == identity;
            },
            [](std::size_t, std::size_t) {});

        for (std::size_t row = 0; row < nodes.size(); ++row)
        {
            if (nodes[row].type.kind != structs::FieldKind::pointer)
            {
                continue;
            }
            const ui::models::LiveCell* cell = cells_.at(row);
            if (cell == nullptr || !cell->readable || !cell->bytes.has_value() || cell->bytes->size() < pointer_size_)
            {
                continue;
            }
            pointer_value_[nodes[row].id] = field_codec::read_le(*cell->bytes, pointer_size_);
        }
        emit valuesChanged();
    }

}
