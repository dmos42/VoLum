/*
 ==============================================================================

 The ALSA half of what the Linux hosts share: one Stream holding what a device
 actually agreed to (format, channels, rate, period), the negotiation that asks
 for float samples and a short period and takes what the hardware offers, and the
 reads/writes that keep going across an xrun instead of giving up.

 volum-live is the only thing that has used it so far; the standalone needs the
 same engine, and a second copy of this code is how a fix ends up in only one of
 them. So it lives here, once, and both hosts call it.

 Nothing in here knows about the plug-in except that the buffers it fills are the
 plug-in's Sample32 planes.

 ==============================================================================
*/

#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pluginterfaces/vst/vsttypes.h"
#include "pcm-format.h"

namespace volumlinux
{

using Steinberg::Vst::Sample32;

/** The one flag the audio loop watches. std::atomic_flag is always lock free, which is
 * what makes setting it from a signal handler something we may rely on. */
void requestStop();
bool stopRequested();

// MARK: streams

struct Stream
{
  snd_pcm_t* pcm = nullptr;
  PcmKind kind = PcmKind::Float;
  snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
  unsigned int channels = 0;
  unsigned int rate = 0;
  unsigned int periodFrames = 0;
  unsigned int bufferFrames = 0;
  unsigned int periodBytes = 0;
  unsigned int frameBytes = 0;
  long xruns = 0;
  std::vector<char> raw;
  std::string name;
};

inline const char* streamLabel(snd_pcm_stream_t dir)
{
  return dir == SND_PCM_STREAM_CAPTURE ? "capture" : "lecture";
}

/** Open a device and negotiate the smallest setup that does the job: float samples if
 * the device speaks them, the plug-in's channel count where the device allows it, and
 * the requested rate and period as close as the hardware permits. */
/** Print the card names ALSA offers, with the ones that need the desktop's sound
 * released first called out: the difference between "default" and "hw:" is the most
 * common reason a first attempt fails. */
void listAlsaDevices();

bool openStream(Stream& s, const std::string& device, snd_pcm_stream_t dir, int pluginChannels, double wantRate,
                unsigned int wantPeriod, unsigned int wantPeriods, std::string& err);
void closeStream(Stream& s);

/** Frames read or written, stopping early once stopRequested() is set. Both keep
 * trying through an xrun rather than turning the failure into silence. */
snd_pcm_sframes_t readFrames(Stream& s, char* raw, unsigned int frames);
snd_pcm_sframes_t writeFrames(Stream& s, const char* raw, unsigned int frames);

void fillFromDevice(const Stream& s, unsigned int frames, const std::vector<Sample32*>& ptr, double gain);
void drainToDevice(Stream& s, unsigned int frames, const std::vector<Sample32*>& ptr);

} // namespace volumlinux
