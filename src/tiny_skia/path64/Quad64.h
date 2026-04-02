#pragma once

#include <array>
#include <cstddef>

namespace tiny_skia::path64 {

namespace quad64 {

std::size_t pushValidTs(const std::array<double, 3>& s, std::size_t realRoots,
                        std::array<double, 3>& t);

std::size_t rootsValidT(double a, double b, double c, std::array<double, 3>& t);

std::size_t rootsReal(double a, double b, double c, std::array<double, 3>& s);

}  // namespace quad64
}  // namespace tiny_skia::path64
