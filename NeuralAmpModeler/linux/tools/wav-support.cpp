#include "wav-support.h"

#include <cstdint>

namespace volumlinux
{

bool readWav(const std::string& path, std::vector<float>& data, int& channels, unsigned int& rate, std::string& err)
{
  FILE* f = std::fopen(path.c_str(), "rb");

  if (!f)
  {
    err = "impossible de lire \"" + path + "\"";
    return false;
  }

  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);

  std::vector<unsigned char> all(static_cast<size_t>(size > 0 ? size : 0));

  if (size <= 44 || std::fread(all.data(), 1, all.size(), f) != all.size())
  {
    err = "\"" + path + "\" est trop court pour etre un WAV";
    std::fclose(f);
    return false;
  }

  std::fclose(f);

  if (std::memcmp(all.data(), "RIFF", 4) != 0 || std::memcmp(all.data() + 8, "WAVE", 4) != 0)
  {
    err = "\"" + path + "\" n'est pas un WAV RIFF";
    return false;
  }

  unsigned int fmtTag = 0, bits = 0;

  for (size_t p = 12; p + 8 <= all.size();)
  {
    const unsigned int chunkSize = all[p + 4] | (all[p + 5] << 8) | (all[p + 6] << 16) | (all[p + 7] << 24);
    const bool isFmt = std::memcmp(all.data() + p, "fmt ", 4) == 0;
    const bool isData = std::memcmp(all.data() + p, "data", 4) == 0;

    if (isFmt && p + 8 + 16 <= all.size())
    {
      fmtTag = all[p + 8] | (all[p + 9] << 8);
      channels = all[p + 10] | (all[p + 11] << 8);
      rate = all[p + 12] | (all[p + 13] << 8) | (all[p + 14] << 16) | (all[p + 15] << 24);
      bits = all[p + 22] | (all[p + 23] << 8);
    }

    if (isData)
    {
      const size_t bytes = std::min<size_t>(chunkSize, all.size() - p - 8);
      const unsigned char* src = all.data() + p + 8;
      const unsigned int sampleBytes = bits / 8;

      if (channels < 1 || sampleBytes == 0)
      {
        err = "en-tete WAV invalide dans \"" + path + "\"";
        return false;
      }

      const size_t frames = bytes / (size_t(sampleBytes) * size_t(channels));
      data.resize(frames * size_t(channels));

      for (size_t i = 0; i < frames * size_t(channels); ++i)
      {
        const unsigned char* q = src + i * sampleBytes;

        if (fmtTag == 3 && bits == 32)
        {
          float v = 0.f;
          std::memcpy(&v, q, 4);
          data[i] = v;
        }
        else if (bits == 16)
        {
          const int16_t v = int16_t(q[0] | (q[1] << 8));
          data[i] = float(v) * (1.0f / 32768.0f);
        }
        else if (bits == 24)
        {
          int32_t v = int32_t(q[0] | (q[1] << 8) | (q[2] << 16));
          if (v & 0x800000)
            v -= 0x1000000;
          data[i] = float(v) * (1.0f / 8388608.0f);
        }
        else if (bits == 32)
        {
          const int32_t v = int32_t(q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24));
          data[i] = float(v) * (1.0f / 2147483648.0f);
        }
        else
        {
          err = "\"" + path + "\" : format " + std::to_string(bits) + " bits non pris en charge";
          return false;
        }
      }

      return true;
    }

    p += 8 + chunkSize + (chunkSize & 1);
  }

  err = "aucun bloc \"data\" dans \"" + path + "\"";
  return false;
}
} // namespace volumlinux
