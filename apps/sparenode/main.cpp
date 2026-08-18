#include <iostream>

#include "sparenode/version.hpp"

/// @brief Prints the SpareNode version for the current executable.
/// @return Zero after attempting to write the version.
int main()
{
    std::cout << "SpareNode " << sparenode::version << '\n';
    return 0;
}
