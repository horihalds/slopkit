#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "reclass/node.hpp"
#include "reclass/project.hpp"

namespace slopkit::test
{
    [[nodiscard]] inline reclass::Node node_of(reclass::Project& project, reclass::NodeKind kind, std::string name = {})
    {
        reclass::Node node;
        node.id   = project.next_node_id();
        node.kind = kind;
        node.name = std::move(name);
        return node;
    }

    [[nodiscard]] inline std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values)
    {
        std::vector<std::byte> bytes;
        for (const unsigned value : values)
        {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }

}
