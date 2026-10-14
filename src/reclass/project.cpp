#include "reclass/project.hpp"

#include <algorithm>

namespace slopkit::reclass
{
    namespace
    {
        std::string cycle_message(const std::vector<const Class*>& chain, const Class& repeated)
        {
            std::string message {"cyclic class reference: "};
            for (auto entry = std::ranges::find(chain, &repeated); entry != chain.end(); ++entry)
            {
                message += (*entry)->name;
                message += " -> ";
            }
            message += repeated.name;
            return message;
        }

        std::expected<std::size_t, std::string>
        storage_of(const Project& project, const Class& type, std::vector<const Class*>& chain);

        std::expected<std::size_t, std::string>
        storage_of_node(const Project& project, const Node& node, std::vector<const Class*>& chain)
        {
            if (node.kind != NodeKind::class_instance)
            {
                return kind_size(node.kind);
            }

            const Class* target = project.find_class(node.class_name);
            if (target == nullptr)
            {
                return std::size_t {0};
            }
            if (std::ranges::find(chain, target) != chain.end())
            {
                return std::unexpected(cycle_message(chain, *target));
            }
            return storage_of(project, *target, chain);
        }

        std::expected<std::size_t, std::string>
        storage_of(const Project& project, const Class& type, std::vector<const Class*>& chain)
        {
            chain.push_back(&type);

            std::size_t total = 0;
            for (const Node& node : type.nodes)
            {
                const auto size = storage_of_node(project, node, chain);
                if (!size)
                {
                    chain.pop_back();
                    return std::unexpected(size.error());
                }
                total += *size;
            }

            chain.pop_back();
            return total;
        }
    }

    const std::vector<Class>& Project::classes() const noexcept
    {
        return classes_;
    }

    std::vector<Class>& Project::classes() noexcept
    {
        return classes_;
    }

    const Class* Project::class_at(std::size_t index) const noexcept
    {
        return index < classes_.size() ? &classes_[index] : nullptr;
    }

    Class* Project::class_at(std::size_t index) noexcept
    {
        return index < classes_.size() ? &classes_[index] : nullptr;
    }

    const Class* Project::find_class(std::string_view name) const noexcept
    {
        const auto found = std::ranges::find_if(classes_,
                                                [name](const Class& type)
                                                {
                                                    return std::string_view {type.name} == name;
                                                });
        return found != classes_.end() ? &*found : nullptr;
    }

    std::size_t Project::add_class(std::string name, std::string expression)
    {
        if (name.empty() || find_class(name) != nullptr)
        {
            return 0;
        }

        Class type;
        type.id                 = next_class_id_++;
        type.name               = std::move(name);
        type.address_expression = std::move(expression);

        const std::size_t id = type.id;
        classes_.push_back(std::move(type));
        return id;
    }

    void Project::remove_class(std::size_t index)
    {
        if (index < classes_.size())
        {
            classes_.erase(classes_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    std::size_t Project::next_node_id() noexcept
    {
        return next_node_id_++;
    }

    void Project::clear() noexcept
    {
        classes_.clear();
        next_class_id_ = 1;
        next_node_id_  = 1;
    }

    std::expected<Layout, std::string> layout_class(const Project& project, const Class& type)
    {
        std::vector<const Class*> chain {&type};

        Layout layout;
        layout.offsets.reserve(type.nodes.size());

        std::size_t offset = 0;
        for (const Node& node : type.nodes)
        {
            layout.offsets.push_back(offset);

            const auto size = storage_of_node(project, node, chain);
            if (!size)
            {
                return std::unexpected(size.error());
            }
            offset += *size;
        }

        layout.size = offset;
        return layout;
    }

    std::size_t node_size(const Project& project, const Node& node)
    {
        std::vector<const Class*> chain;
        return storage_of_node(project, node, chain).value_or(0);
    }

}
