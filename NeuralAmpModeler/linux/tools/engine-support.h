/*
 ==============================================================================

 The plug-in side of a host: hand it a block, take the block back, keep the
 numbers that tell you whether anything came out.

 The metering matters more than it looks. The peak/rms pair is what distinguishes
 "the port loaded and the loop ran" from "the port loaded, the loop ran and the
 guitar actually got through", and it is how the headless probe says a silent
 output is a fault instead of a pass.

 ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "host-support.h"

namespace volumlinux
{

using Steinberg::Vst::AudioBusBuffers;
using Steinberg::Vst::ProcessData;
using Steinberg::Vst::Sample32;

// MARK: the engine

struct Engine
{
  Loaded* plugin = nullptr;
  unsigned int blockFrames = 64;
  std::vector<float> inStore;
  std::vector<float> outStore;
  std::vector<Sample32*> inPtr;
  std::vector<Sample32*> outPtr;
  AudioBusBuffers inBus{};
  AudioBusBuffers outBus{};
  ProcessData data{};
  long blocks = 0;
  long processErrors = 0;
  double peakIn = 0.0, peakOut = 0.0, sumIn = 0.0, sumOut = 0.0;
  double runPeakIn = 0.0, runPeakOut = 0.0;
  long counted = 0;

  void setup(Loaded& l, unsigned int frames)
  {
    plugin = &l;
    blockFrames = frames;

    inStore.assign(size_t(l.inChannels) * frames, 0.f);
    outStore.assign(size_t(l.outChannels) * frames, 0.f);

    for (int c = 0; c < l.inChannels; ++c)
      inPtr.push_back(inStore.data() + size_t(c) * frames);

    for (int c = 0; c < l.outChannels; ++c)
      outPtr.push_back(outStore.data() + size_t(c) * frames);

    inBus.numChannels = l.inChannels;
    inBus.silenceFlags = 0;
    inBus.channelBuffers32 = inPtr.data();

    outBus.numChannels = l.outChannels;
    outBus.silenceFlags = 0;
    outBus.channelBuffers32 = outPtr.data();

    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = frames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;
  }

  bool process()
  {
    if (plugin->processor->process(data) != Steinberg::kResultOk)
    {
      ++processErrors;
      return false;
    }

    ++blocks;
    return true;
  }

  void meter()
  {
    for (unsigned int i = 0; i < blockFrames; ++i)
    {
      const double a = std::abs(inPtr.empty() ? 0.0 : inPtr[0][i]);
      const double b = std::abs(outPtr.empty() ? 0.0 : outPtr[0][i]);

      peakIn = std::max(peakIn, a);
      peakOut = std::max(peakOut, b);
      // Held for the whole run: the periodic report clears the window values, and the
      // final verdict must not depend on when the last report happened to fall.
      runPeakIn = std::max(runPeakIn, a);
      runPeakOut = std::max(runPeakOut, b);
      sumIn += a * a;
      sumOut += b * b;
      ++counted;
    }
  }
};

inline double dbfs(double v)
{
  return v <= 1e-9 ? -180.0 : 20.0 * std::log10(v);
}

inline void reportLevels(const Engine& eng, double secondsRun)
{
  if (eng.counted == 0)
    return;

  const double rmsIn = std::sqrt(eng.sumIn / eng.counted);
  const double rmsOut = std::sqrt(eng.sumOut / eng.counted);

  printf("[%5.1f s] entree crete=%+.1f dBFS rms=%+.1f dBFS | sortie crete=%+.1f dBFS rms=%+.1f dBFS | %s\n", secondsRun,
         dbfs(eng.peakIn), dbfs(rmsIn), dbfs(eng.peakOut), dbfs(rmsOut),
         eng.peakIn > 0.999 ? "ENTREE SATUREE : baisse --gain ou le volume de la source" : "niveaux ok");
}

inline void resetLevels(Engine& eng)
{
  eng.peakIn = eng.peakOut = eng.sumIn = eng.sumOut = 0.0;
  eng.counted = 0;
}

} // namespace volumlinux
