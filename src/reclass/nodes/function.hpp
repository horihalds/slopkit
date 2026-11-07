#pragma once

#include "reclass/nodes/node.hpp"

namespace slopkit::reclass
{
    // The vtable, a virtual method and a function pointer are all one pointer
    // wide; a function node covers the code block at its address.
    using VirtualMethodTableNode = ScalarNode<NodeKind::virtual_method_table, 8>;
    using VirtualMethodNode      = ScalarNode<NodeKind::virtual_method, 8>;
    using FunctionPtrNode        = ScalarNode<NodeKind::function_ptr, 8>;
    using FunctionNode           = ScalarNode<NodeKind::function, kFunctionBytes>;
}
