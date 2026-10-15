// Windows UCRT lacks the C23 roundevenf symbol emitted by Clang for PPC rounding.
#include <cmath>
extern "C" float roundevenf(float value) {
  if (!std::isfinite(value) || value == 0.0f || std::fabs(value) >= 8388608.0f)
    return value;
  const float lower = std::floor(value);
  const float fraction = value - lower;
  float rounded = lower;
  if (fraction > 0.5f || (fraction == 0.5f && std::fmod(lower, 2.0f) != 0.0f))
    rounded += 1.0f;
  return std::copysign(rounded, value);
}
