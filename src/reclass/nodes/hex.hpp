#pragma once

#include "reclass/nodes/node.hpp"

namespace slopkit::reclass
{
    using Hex8Node  = ScalarNode<NodeKind::hex8, 1>;
    using Hex16Node = ScalarNode<NodeKind::hex16, 2>;
    using Hex32Node = ScalarNode<NodeKind::hex32, 4>;
    using Hex64Node = ScalarNode<NodeKind::hex64, 8>;
}
