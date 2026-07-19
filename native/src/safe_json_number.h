#pragma once
#include "common.h"

namespace hp {

inline int ClampedJsonIntOr(
    double value, int fallback, int minimum, int maximum) noexcept {
  if (!std::isfinite(value) || minimum > maximum) return fallback;
  const double bounded = std::clamp(
      value, static_cast<double>(minimum), static_cast<double>(maximum));
  return static_cast<int>(bounded);
}

template <typename Integer>
std::optional<Integer> ExactJsonInteger(
    double value, Integer minimum, Integer maximum) noexcept {
  static_assert(std::is_integral_v<Integer>);
  if (!std::isfinite(value) || std::trunc(value) != value || minimum > maximum) {
    return std::nullopt;
  }
  const double lower = static_cast<double>(minimum);
  const double upper = static_cast<double>(maximum);
  if (value < lower || value > upper) return std::nullopt;
  return static_cast<Integer>(value);
}

inline int64_t NonNegativeJsonMilliseconds(double value) noexcept {
  // JSON numbers are exact only through 2^53 - 1. Reject larger values before
  // converting to int64_t so hostile or corrupt payloads cannot trigger an
  // out-of-range floating-point-to-integer conversion.
  constexpr double kMaxExactJsonInteger = 9'007'199'254'740'991.0;
  if (!std::isfinite(value) || value <= 0 || value > kMaxExactJsonInteger) return 0;
  return static_cast<int64_t>(value);
}

}  // namespace hp
