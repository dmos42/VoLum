/*
 ==============================================================================

 Implementation of alsa-support.h. The types and the small sample conversions
 stayed in the header because they are the shape every caller works with; the
 device work, which is where the hardware surprises live, is here.

 ==============================================================================
*/

#include "alsa-support.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace volumlinux
{

std::atomic_flag gStopFlag{};

void requestStop()
{
  gStopFlag.test_and_set();
}

bool stopRequested()
{
  return gStopFlag.test();
}

void listAlsaDevices()
{
  char** hint = nullptr;

  if (snd_device_name_hint(-1, "pcm", reinterpret_cast<void***>(&hint)) < 0)
  {
    printf("aucun peripherique ALSA annonce\n");
    return;
  }

  printf("peripheriques d'enregistrement (pour --in) :\n");

  for (char** h = hint; *h; ++h)
  {
    char* name = snd_device_name_get_hint(*h, "NAME");
    char* desc = snd_device_name_get_hint(*h, "DESCRIPTION");
    char* ioid = snd_device_name_get_hint(*h, "IOID");

    if (name && (!ioid || std::strcmp(ioid, "Capture") == 0))
      printf("  %-28s %s\n", name, desc ? desc : "");

    std::free(name);
    std::free(desc);
    std::free(ioid);
  }

  printf("\nperipheriques de sortie (pour --out, ce que tu entends) :\n");

  for (char** h = hint; *h; ++h)
  {
    char* name = snd_device_name_get_hint(*h, "NAME");
    char* desc = snd_device_name_get_hint(*h, "DESCRIPTION");
    char* ioid = snd_device_name_get_hint(*h, "IOID");

    if (name && (!ioid || std::strcmp(ioid, "Playback") == 0))
      printf("  %-28s %s\n", name, desc ? desc : "");

    std::free(name);
    std::free(desc);
    std::free(ioid);
  }

  snd_device_name_free_hint(reinterpret_cast<void**>(hint));
}
bool openStream(Stream& s, const std::string& device, snd_pcm_stream_t dir, int pluginChannels, double wantRate,
                unsigned int wantPeriod, unsigned int wantPeriods, std::string& err)
{
  s.name = device;

  const int opened = snd_pcm_open(&s.pcm, device.c_str(), dir, 0);

  if (opened < 0)
  {
    err = std::string(streamLabel(dir)) + " : impossible d'ouvrir \"" + device + "\" : " + snd_strerror(opened);

    if (opened == -ENOENT)
      err += "\n  (essaie --list pour voir les noms disponibles, et \"default\" pour passer par PipeWire)";
    else if (opened == -EBUSY)
      err +=
        "\n  (la carte est tenue par le son du bureau. Deux sorties :"
        "\n   - le plus simple : repasse par le bureau avec --out default (et --in default),"
        "\n     la periode demandee sera ajustee par PipeWire ;"
        "\n   - pour la carte en direct : rends-la le temps de l'essai avec"
        "\n       systemctl --user stop wireplumber pipewire pipewire-pulse"
        "\n     puis remets le son du bureau avec"
        "\n       systemctl --user start wireplumber pipewire pipewire-pulse)";

    return false;
  }

  snd_pcm_hw_params_t* hp = nullptr;
  snd_pcm_hw_params_alloca(&hp);
  snd_pcm_hw_params_any(s.pcm, hp);

  snd_pcm_hw_params_set_access(s.pcm, hp, SND_PCM_ACCESS_RW_INTERLEAVED);

  const snd_pcm_format_t candidates[] = {SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S16_LE};
  const PcmKind kinds[] = {PcmKind::Float, PcmKind::S32, PcmKind::S16};

  for (int i = 0; i < 3; ++i)
  {
    if (snd_pcm_hw_params_test_format(s.pcm, hp, candidates[i]) == 0)
    {
      s.format = candidates[i];
      s.kind = kinds[i];
      break;
    }
  }

  if (s.format == SND_PCM_FORMAT_UNKNOWN)
  {
    err = std::string(streamLabel(dir)) + " : \"" + device
          + "\" n'offre aucun format exploitable (flottant 32, signe 32 ou signe 16)";
    snd_pcm_close(s.pcm);
    s.pcm = nullptr;
    return false;
  }

  snd_pcm_hw_params_set_format(s.pcm, hp, s.format);

  unsigned int channels = static_cast<unsigned int>(pluginChannels);
  unsigned int chMin = 0, chMax = 0;
  snd_pcm_hw_params_get_channels_min(hp, &chMin);
  snd_pcm_hw_params_get_channels_max(hp, &chMax);

  if (snd_pcm_hw_params_test_channels(s.pcm, hp, channels) != 0)
  {
    const unsigned int fallback = channels < chMin ? chMin : chMax;
    printf("  %s : %d canaux refuses par \"%s\" (plage %u..%u) -> %u canaux\n", streamLabel(dir), pluginChannels,
           device.c_str(), chMin, chMax, fallback);
    channels = fallback;

    if (channels == 0)
    {
      err = std::string(streamLabel(dir)) + " : \"" + device + "\" n'annonce aucun canal";
      snd_pcm_close(s.pcm);
      s.pcm = nullptr;
      return false;
    }
  }

  snd_pcm_hw_params_set_channels(s.pcm, hp, channels);
  s.channels = channels;

  unsigned int rate = wantRate > 0 ? static_cast<unsigned int>(wantRate) : 48000u;
  int dirn = 0;
  snd_pcm_hw_params_set_rate_near(s.pcm, hp, &rate, &dirn);

  unsigned int periods = wantPeriods;
  snd_pcm_hw_params_set_periods_near(s.pcm, hp, &periods, &dirn);

  // ALSA counts buffer sizes in frames, with its own type: not an unsigned int.
  snd_pcm_uframes_t period = wantPeriod;
  snd_pcm_hw_params_set_period_size_near(s.pcm, hp, &period, &dirn);

  const int applied = snd_pcm_hw_params(s.pcm, hp);

  if (applied < 0)
  {
    err = std::string(streamLabel(dir)) + " : \"" + device + "\" refuse cette configuration : " + snd_strerror(applied)
          + "\n  (essaye --period 128 ou --periods 3)";
    snd_pcm_close(s.pcm);
    s.pcm = nullptr;
    return false;
  }

  snd_pcm_hw_params_get_rate(hp, &rate, &dirn);
  snd_pcm_hw_params_get_period_size(hp, &period, &dirn);

  snd_pcm_uframes_t buf = 0;
  snd_pcm_hw_params_get_buffer_size(hp, &buf);

  s.rate = rate;
  s.periodFrames = static_cast<unsigned int>(period);
  s.bufferFrames = static_cast<unsigned int>(buf);
  s.periodBytes = s.periodFrames * kindBytes(s.kind);
  s.frameBytes = channels * kindBytes(s.kind);

  snd_pcm_sw_params_t* sp = nullptr;
  snd_pcm_sw_params_alloca(&sp);
  snd_pcm_sw_params_current(s.pcm, sp);
  snd_pcm_sw_params_set_start_threshold(s.pcm, sp, period);
  snd_pcm_sw_params_set_avail_min(s.pcm, sp, period);
  // Without this the driver would fill the gaps with silence instead of telling us.
  snd_pcm_sw_params_set_silence_threshold(s.pcm, sp, 0);
  snd_pcm_sw_params_set_silence_size(s.pcm, sp, 0);
  snd_pcm_sw_params(s.pcm, sp);

  s.raw.resize(static_cast<size_t>(s.bufferFrames) * s.channels * kindBytes(s.kind), 0);

  // A server-backed device re-shapes our requests: its own period, not ours, sets how
  // often the audio actually moves, so say so before anyone blames this program.
  const bool serverBacked = device == "default" || device == "sysdefault" || device == "pipewire" || device == "pulse";

  if (serverBacked)
    printf(
      "%-8s : peripherique du serveur son : la periode demandee est recoupee par le serveur,"
      "\n         la latence finale depend de sa periode a lui. Pour la carte en direct,"
      " utilise front: ou plughw:\n",
      streamLabel(dir));

  const double periodMs = 1000.0 * double(period) / double(rate);
  printf("%-8s \"%s\" : %u Hz, %s, %u canaux, periode %u (%.2f ms), tampon %u images (%u periodes)\n", streamLabel(dir),
         device.c_str(), rate, snd_pcm_format_name(s.format), channels, static_cast<unsigned int>(period), periodMs,
         s.bufferFrames, static_cast<unsigned int>(buf / period));

  // A period this short over a server that runs on its own quantum is not a small
  // latency, it is a stream the server has to keep feeding in pieces: measured here in
  // full duplex with the window open, 64 frames ran the playback dry several times a
  // minute, 256 and 512 never once. What a listener hears from that is choppy output,
  // and the advice is a flag, not a rebuild.
  if (serverBacked && period < 256)
    printf(
      "%-8s : attention : %u images par periode sur un peripherique du serveur son :"
      "\n         le serveur taille a sa periode a lui, ce flux se videra par instants."
      "\n         --period 256 est propre dans nos mesures ; pour plus court, prends la carte"
      "\n         en direct (front: ou plughw:) ou regle le quantum du serveur.\n",
      streamLabel(dir), static_cast<unsigned int>(period));

  return true;
}

void closeStream(Stream& s)
{
  if (!s.pcm)
    return;

  snd_pcm_drain(s.pcm);
  snd_pcm_close(s.pcm);
  s.pcm = nullptr;
}

/** Transfer exactly `frames` frames, in device-period sized pieces, recovering from
 * over/underruns as we go. Returns the frame count, or a negative ALSA error. */
snd_pcm_sframes_t readFrames(Stream& s, char* raw, unsigned int frames)
{
  unsigned int done = 0;

  while (done < frames && !stopRequested())
  {
    const unsigned int want = std::min(frames - done, s.periodFrames);
    const snd_pcm_sframes_t got = snd_pcm_readi(s.pcm, raw + size_t(done) * s.frameBytes, want);

    if (got == -EPIPE)
    {
      ++s.xruns;
      printf("  [xrun] %s : retard d'entree (%s)\n", streamLabel(SND_PCM_STREAM_CAPTURE), s.name.c_str());
    }

    if (got < 0)
    {
      if (snd_pcm_recover(s.pcm, int(got), 0) == 0)
        continue;

      return got;
    }

    done += static_cast<unsigned int>(got);
  }

  return static_cast<snd_pcm_sframes_t>(done);
}

snd_pcm_sframes_t writeFrames(Stream& s, const char* raw, unsigned int frames)
{
  unsigned int done = 0;

  while (done < frames && !stopRequested())
  {
    const unsigned int want = std::min(frames - done, s.periodFrames);
    const snd_pcm_sframes_t put = snd_pcm_writei(s.pcm, raw + size_t(done) * s.frameBytes, want);

    if (put == -EPIPE)
    {
      ++s.xruns;
      printf("  [xrun] %s : tampon vide (%s)\n", streamLabel(SND_PCM_STREAM_PLAYBACK), s.name.c_str());
    }

    if (put < 0)
    {
      if (snd_pcm_recover(s.pcm, int(put), 0) == 0)
        continue;

      return put;
    }

    done += static_cast<unsigned int>(put);
  }

  return static_cast<snd_pcm_sframes_t>(done);
}

void fillFromDevice(const Stream& s, unsigned int frames, const std::vector<Sample32*>& ptr, double gain)
{
  const unsigned int stride = s.channels * kindBytes(s.kind);

  for (size_t c = 0; c < ptr.size(); ++c)
  {
    if (c >= s.channels)
    {
      std::fill(ptr[c], ptr[c] + frames, Sample32(0));
      continue;
    }

    Sample32* dst = ptr[c];
    const char* src = s.raw.data() + c * kindBytes(s.kind);

    for (unsigned int i = 0; i < frames; ++i, src += stride)
      dst[i] = static_cast<Sample32>(loadSample(src, s.kind) * float(gain));
  }
}

void drainToDevice(Stream& s, unsigned int frames, const std::vector<Sample32*>& ptr)
{
  const unsigned int stride = s.channels * kindBytes(s.kind);

  for (unsigned int c = 0; c < s.channels; ++c)
  {
    char* dst = s.raw.data() + c * kindBytes(s.kind);
    const Sample32* src = c < ptr.size() ? ptr[c] : nullptr;

    for (unsigned int i = 0; i < frames; ++i, dst += stride)
      storeSample(dst, s.kind, src ? src[i] : 0.f);
  }
}

} // namespace volumlinux
