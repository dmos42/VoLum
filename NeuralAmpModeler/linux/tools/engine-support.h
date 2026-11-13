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

#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "public.sdk/source/vst/hosting/eventlist.h"

namespace volumlinux
{

using Steinberg::Vst::AudioBusBuffers;
using Steinberg::Vst::ProcessData;
using Steinberg::Vst::Sample32;

// MARK: midi bytes in, VST3 events out

/** Decode one raw MIDI message into a VST3 event. Notes go through the typed
 *  events, everything else through the legacy CC event - the one the plug-in's
 *  MIDI-in path answers (recall CC, footswitches, program change). Returns false
 *  for what a VST3 event cannot carry (real-time bytes, unframed system common). */
inline bool MidiBytesToEvent(const uint8_t* bytes, size_t len, int offsetSamples, Steinberg::Vst::Event& ev)
{
  if (!bytes || len == 0 || offsetSamples < 0)
    return false;

  ev = Steinberg::Vst::Event{};
  ev.busIndex = 0;
  ev.sampleOffset = offsetSamples;

  const int channel = bytes[0] & 0x0F;

  switch (bytes[0] & 0xF0)
  {
    case 0x80:
      ev.type = Steinberg::Vst::Event::kNoteOffEvent;
      ev.noteOff.channel = channel;
      ev.noteOff.pitch = len > 1 ? bytes[1] : 0;
      ev.noteOff.velocity = len > 2 ? bytes[2] / 127.f : 0.f;
      ev.noteOff.noteId = -1;
      return true;
    case 0x90:
      if (len > 2 && bytes[2] == 0)
      {
        ev.type = Steinberg::Vst::Event::kNoteOffEvent;
        ev.noteOff.channel = channel;
        ev.noteOff.pitch = bytes[1];
        ev.noteOff.noteId = -1;
        return true;
      }
      ev.type = Steinberg::Vst::Event::kNoteOnEvent;
      ev.noteOn.channel = channel;
      ev.noteOn.pitch = len > 1 ? bytes[1] : 0;
      ev.noteOn.velocity = len > 2 ? bytes[2] / 127.f : 0.f;
      ev.noteOn.noteId = -1;
      return true;
    case 0xA0:
      ev.type = Steinberg::Vst::Event::kPolyPressureEvent;
      ev.polyPressure.channel = channel;
      ev.polyPressure.pitch = len > 1 ? bytes[1] : 0;
      ev.polyPressure.pressure = len > 2 ? bytes[2] / 127.f : 0.f;
      ev.polyPressure.noteId = -1;
      return true;
    case 0xB0:
      if (len < 3)
        return false;
      ev.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
      ev.midiCCOut.controlNumber = bytes[1];
      ev.midiCCOut.channel = channel;
      ev.midiCCOut.value = bytes[2];
      ev.midiCCOut.value2 = 0;
      return true;
    case 0xC0:
      if (len < 2)
        return false;
      ev.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
      ev.midiCCOut.controlNumber = Steinberg::Vst::kCtrlProgramChange;
      ev.midiCCOut.channel = channel;
      ev.midiCCOut.value = bytes[1];
      return true;
    case 0xD0:
      if (len < 2)
        return false;
      ev.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
      ev.midiCCOut.controlNumber = Steinberg::Vst::kAfterTouch;
      ev.midiCCOut.channel = channel;
      ev.midiCCOut.value = bytes[1];
      return true;
    case 0xE0:
      if (len < 3)
        return false;
      ev.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
      ev.midiCCOut.controlNumber = Steinberg::Vst::kPitchBend;
      ev.midiCCOut.channel = channel;
      ev.midiCCOut.value = bytes[1];
      ev.midiCCOut.value2 = bytes[2];
      return true;
    default:
      return false;
  }
}

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
  // MIDI the hosts drained since the last block: notes as typed events, the rest
  // as the legacy CC event the plug-in's MIDI-in path answers. Cleared after each
  // process(), so a queued message is handed over exactly once.
  Steinberg::Vst::EventList events;
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

    // Re-setup is a real path now: the audio panel reopens the stream mid-run
    // (issue #10), and a second setup that pushed onto these would leave the
    // first setup's pointers in front of the real ones.
    inPtr.clear();
    outPtr.clear();

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
    data.inputEvents = &events;
  }

  /** Queue one raw MIDI message for the next block, offsetSamples frames in. A
   *  running status byte or a stray data byte (len 1) is not a message: dropped. */
  void queueMidi(const uint8_t* bytes, size_t len, int offsetSamples)
  {
    Steinberg::Vst::Event ev;

    if (!MidiBytesToEvent(bytes, len, offsetSamples, ev))
      return;

    events.addEvent(ev);
  }

  bool process()
  {
    const bool ok = plugin->processor->process(data) == Steinberg::kResultOk;

    if (!ok)
      ++processErrors;

    events.clear();
    ++blocks;
    return ok;
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
