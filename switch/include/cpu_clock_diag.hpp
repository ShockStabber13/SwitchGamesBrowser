#pragma once
#include <string>
#include <vector>

namespace sgb {
// Read-only diagnostic: never calls SetClockRate or changes voltages.
std::vector<std::string> probeCpuClockReadOnly();
}
