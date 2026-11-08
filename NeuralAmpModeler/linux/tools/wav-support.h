/*
 ==============================================================================

 WAV in and WAV out, so the hosts can be tested with no sound card at all: feed a
 recorded file, write the plug-in's output to another, and diff the two. The
 reader only handles what a DAW writes out - PCM 16/24/32 and float 32, interleaved;
 the writer always emits float 32 because that is what the plug-in produced.

 ==============================================================================
*/

#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pluginterfaces/vst/vsttypes.h"
#include "pcm-format.h"

namespace volumlinux
{

using Steinberg::Vst::Sample32;

bool readWav(const std::string& path, std::vector<float>& data, int& channels, unsigned int& rate, std::string& err);

class WavWriter
{
public:
  bool isOpen() const { return mFile != nullptr; }

  bool open(const std::string& path, unsigned int channels, unsigned int rate)
  {
    mFile = std::fopen(path.c_str(), "wb");

    if (!mFile)
      return false;

    mChannels = channels;
    mRate = rate;

    char header[44];
    std::memset(header, 0, sizeof(header));
    std::memcpy(header, "RIFF", 4);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    header[16] = 16; // fmt chunk size
    header[20] = 1; // PCM
    header[22] = static_cast<char>(channels & 0xff);
    header[23] = static_cast<char>((channels >> 8) & 0xff);
    header[24] = static_cast<char>(rate & 0xff);
    header[25] = static_cast<char>((rate >> 8) & 0xff);
    header[26] = static_cast<char>((rate >> 16) & 0xff);
    header[27] = static_cast<char>((rate >> 24) & 0xff);
    const unsigned int byteRate = rate * channels * 2;
    header[28] = static_cast<char>(byteRate & 0xff);
    header[29] = static_cast<char>((byteRate >> 8) & 0xff);
    header[30] = static_cast<char>((byteRate >> 16) & 0xff);
    header[31] = static_cast<char>((byteRate >> 24) & 0xff);
    header[32] = static_cast<char>((channels * 2) & 0xff);
    header[33] = static_cast<char>(((channels * 2) >> 8) & 0xff);
    header[34] = 16; // bits
    std::memcpy(header + 36, "data", 4);

    std::fwrite(header, 1, sizeof(header), mFile);
    return true;
  }

  void block(const std::vector<Sample32*>& ptr, unsigned int frames)
  {
    if (!mFile)
      return;

    for (unsigned int i = 0; i < frames; ++i)
    {
      for (unsigned int c = 0; c < mChannels; ++c)
      {
        const float v = c < ptr.size() ? ptr[c][i] : 0.f;
        storeSample(mScratch, PcmKind::S16, v);
        std::fwrite(mScratch, 1, 2, mFile);
      }
    }

    mFrames += frames;
  }

  void close()
  {
    if (!mFile)
      return;

    const unsigned int dataBytes = mFrames * mChannels * 2;
    const unsigned int riffSize = dataBytes + 36;

    std::fseek(mFile, 4, SEEK_SET);
    std::fputc(int(riffSize & 0xff), mFile);
    std::fputc(int((riffSize >> 8) & 0xff), mFile);
    std::fputc(int((riffSize >> 16) & 0xff), mFile);
    std::fputc(int((riffSize >> 24) & 0xff), mFile);

    std::fseek(mFile, 40, SEEK_SET);
    std::fputc(int(dataBytes & 0xff), mFile);
    std::fputc(int((dataBytes >> 8) & 0xff), mFile);
    std::fputc(int((dataBytes >> 16) & 0xff), mFile);
    std::fputc(int((dataBytes >> 24) & 0xff), mFile);

    std::fclose(mFile);
    mFile = nullptr;
    printf("WAV ecrit : %u images a %u Hz -> %ld octets\n", mFrames, mRate, long(dataBytes + 44));
  }

private:
  FILE* mFile = nullptr;
  unsigned int mChannels = 2;
  unsigned int mRate = 48000;
  unsigned int mFrames = 0;
  // Sized for the widest sample we ever serialize, and written as a whole each time.
  char mScratch[4] = {};
};


} // namespace volumlinux
