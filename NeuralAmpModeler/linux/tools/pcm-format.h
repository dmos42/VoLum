/*
 ==============================================================================

 How a block of samples is laid out in memory, and how to move one sample in or
 out of it. A sound card hands you float, 32-bit or 16-bit integers depending on
 what it agreed to, and the plug-in works in float, so this small table is the
 whole translation - which is also why the WAV writer reaches for it.

 ==============================================================================
*/

#pragma once

#include <cstdint>
#include <cstring>

namespace volumlinux
{

enum class PcmKind
{
  Float,
  S32,
  S16
};

inline unsigned int kindBytes(PcmKind k)
{
  return k == PcmKind::S16 ? 2u : 4u;
}

inline float loadSample(const char* p, PcmKind k)
{
  if (k == PcmKind::Float)
  {
    float f = 0.f;
    std::memcpy(&f, p, sizeof(f));
    return f;
  }

  if (k == PcmKind::S32)
  {
    int32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return static_cast<float>(v) * (1.0f / 2147483648.0f);
  }

  int16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return static_cast<float>(v) * (1.0f / 32768.0f);
}

inline void storeSample(char* p, PcmKind k, float v)
{
  if (v > 1.f)
    v = 1.f;
  else if (v < -1.f)
    v = -1.f;

  if (k == PcmKind::Float)
    std::memcpy(p, &v, sizeof(v));
  else if (k == PcmKind::S32)
  {
    const int32_t s = static_cast<int32_t>(v * 2147483647.0f);
    std::memcpy(p, &s, sizeof(s));
  }
  else
  {
    const int16_t s = static_cast<int16_t>(v * 32767.0f);
    std::memcpy(p, &s, sizeof(s));
  }
}

} // namespace volumlinux
