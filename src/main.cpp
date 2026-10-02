#include <iostream>

#include "core/version.hpp"

int main()
{
    std::cout << "slopkit - reverse engineering toolset\n";
    std::cout << "version " << slopkit::version() << '\n';
    return 0;
}
