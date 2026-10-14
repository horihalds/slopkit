#pragma once

#include "reclass/nodes/node.hpp"

namespace slopkit::reclass
{
    // Two, three or four floats, and the square matrices of the reference's
    // matrix node: each is a fixed run of floats, so the kind alone sizes it.
    using Vector2Node = ScalarNode<NodeKind::vector2, 8>;
    using Vector3Node = ScalarNode<NodeKind::vector3, 12>;
    using Vector4Node = ScalarNode<NodeKind::vector4, 16>;

    using Matrix3x3Node = ScalarNode<NodeKind::matrix3x3, 36>;
    using Matrix3x4Node = ScalarNode<NodeKind::matrix3x4, 48>;
    using Matrix4x4Node = ScalarNode<NodeKind::matrix4x4, 64>;
}
