/*
 ==============================================================================

 volum-live: VoLum without a DAW.

 The probe (volum-vst3-probe) answers "does the port load and make sound?". This tool
 goes one step further and is the audio engine itself: it opens an ALSA capture and
 playback stream, feeds VoLum block by block, and puts the plug-in's editor in a
 window. The DAW's remaining job - moving samples and keeping the graph turning -
 turns out to be about a hundred lines we can read.

 Why bother: on a PipeWire desktop a DAW reaches the card through a JACK bridge plus a
 buffer of its own, and the round trip lands near 11 ms. Talking to ALSA directly, one
 period of 64 frames is 1.45 ms at 44.1 kHz, so the round trip can drop to single
 digits.

 Usage
   volum-live [<module>.so> --help        (full option list)

 The same options may be written in ~/.config/VoLum/live.conf as key=value lines: that
 is where the device and the period size belong, so nothing has to be retyped and no
 DAW is involved.

 Note on the card: unless the desktop is not using it, ALSA goes through PipeWire
 ("default"), which shares the device. Opening "hw:<card>,<device>" takes the card for
 VoLum alone and the desktop goes silent until VoLum exits.

 Exit codes: 0 ok, 2 usage or module cannot be loaded, 3 no usable class,
 4 the audio device could not be used, 5 the plug-in refused the setup.

 ==============================================================================
*/

#include "host-support.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace volumlinux;
using Steinberg::Vst::AudioBusBuffers;
using Steinberg::Vst::ProcessData;
using Steinberg::Vst::Sample32;

namespace
{

volatile sig_atomic_t gStop = 0;

void onSignal(int)
{
  gStop = 1;
}

// MARK: options

struct Options
{
  std::string module; // VST3 module to load
  std::string configFile; // empty = the default location
  std::string outDevice = "default";
  std::string inDevice; // empty = same as outDevice, unless --no-in clears it
  std::string inFile; // feed the plug-in from a WAV instead of the card
  std::string outFile; // write the plug-in output to a WAV as well
  bool outRequested = false; // an explicit --out: a WAV input then plays live, not offline
  bool noIn = false; // --no-in: no capture stream at all
  double rate = 0.0; // 0 = whatever the device prefers, starting from 48000
  unsigned int period = 64;
  unsigned int periods = 2;
  double gain = 1.0; // input trim, applied before the plug-in sees the signal
  double seconds = 0.0; // 0 = run until the window closes or Ctrl-C
  double meterEvery = 2.0; // 0 = silent
  bool ui = true;
  bool listDevices = false;
  bool listParams = false;
  bool check = false; // negotiate everything, print what was agreed, do not run
  std::vector<std::pair<int, double>> params;
};

void printUsage(const char* argv0)
{
  printf(
    "usage : %s [module.vst3.so] [options]\n"
    "\n"
    "  --module PATH        module VST3 (defaut : ~/.vst3/VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so)\n"
    "  --out DEV            peripherique ALSA de sortie (defaut : default)\n"
    "  --in DEV             peripherique ALSA d'entree (defaut : la meme que --out)\n"
    "  --no-in              aucune capture : le plugin recevra du silence\n"
    "  --rate N             cadence, 0 = celle de la carte (defaut : 0)\n"
    "  --period N           images par periode, aussi la taille du bloc traite (defaut : 64)\n"
    "  --periods N          periodes dans le tampon (defaut : 2)\n"
    "  --gain X             gain d'entree avant le plugin (defaut : 1)\n"
    "  --seconds N          duree, 0 = jusqu'a la fermeture de la fenetre (defaut : 0)\n"
    "  --meter N            secondes entre deux mesures de niveau, 0 = aucune (defaut : 2)\n"
    "  --param IDX:VAL      regle un parametre normalise avant de demarrer (repetable)\n"
    "  --in-file F.wav      lire l'entree dans un WAV au lieu de la carte\n"
    "  --out-file F.wav     ecrire la sortie dans un WAV (avec --out : les deux a la fois)\n"
    "  --no-ui              ne pas ouvrir la fenetre du plugin\n"
    "  --check              verifier la configuration et quitter\n"
    "  --list               lister les peripheriques ALSA et quitter\n"
    "  --list-params        lister les parametres du plugin\n"
    "  --config PATH        fichier de reglages (defaut : ~/.config/VoLum/live.conf)\n"
    "  --help               cette aide\n"
    "\n"
    "Les memes cles peuvent etre ecrites dans %s, une par ligne, sous la forme\n"
    "  out=default\n"
    "  period=64\n"
    "\n"
    "guitare dans l'ampli, carte rendue a ce programme (--list pour les noms) :\n"
    "  %s --out front:CARD=THR10X,DEV=0 --in front:CARD=THR10X,DEV=0 --period 64\n"
    "\n"
    "regler a l'oreille sans guitare, en jouant un fichier dans la carte :\n"
    "  %s --in-file guitare.wav --out default\n"
    "\n"
    "traiter un fichier sans aucune carte son :\n"
    "  %s --in-file guitare.wav --out-file amplifie.wav\n",
    argv0,
    (getenv("HOME") ? std::string(getenv("HOME")) + "/.config/VoLum/live.conf"
                    : std::string("~/.config/VoLum/live.conf"))
      .c_str(),
    argv0, argv0, argv0);
}

/** Only these keys take a value, so a bare flag never swallows the next argument. */
bool keyWantsValue(const std::string& key)
{
  static const char* const valued[] = {"module", "out",     "in",   "in-file", "out-file", "config", "rate",
                                       "period", "periods", "gain", "seconds", "meter",    "param"};

  for (const char* candidate : valued)
    if (key == candidate)
      return true;

  return false;
}

bool applyOption(const std::string& key, const char* value, Options& opt, std::string& err)
{
  auto wantNumber = [&](double& out) {
    if (!value)
    {
      err = "--" + key + " attend une valeur";
      return false;
    }
    out = std::atof(value);
    return true;
  };
  auto wantText = [&](std::string& out) {
    if (!value)
    {
      err = "--" + key + " attend une valeur";
      return false;
    }
    out = value;
    return true;
  };

  if (key == "module")
    return wantText(opt.module);

  // --out is what tells us the user wants the sound live: without it, a WAV in and a
  // WAV out is a plain offline conversion.
  if (key == "out")
  {
    if (!wantText(opt.outDevice))
      return false;
    opt.outRequested = true;
    return true;
  }
  if (key == "in")
    return wantText(opt.inDevice);
  if (key == "in-file")
    return wantText(opt.inFile);
  if (key == "out-file")
    return wantText(opt.outFile);
  if (key == "config")
    return wantText(opt.configFile);
  if (key == "rate")
  {
    double v;
    if (!wantNumber(v))
      return false;
    opt.rate = v;
    return true;
  }
  if (key == "period")
  {
    double v;
    if (!wantNumber(v))
      return false;
    opt.period = unsigned(v);
    return true;
  }
  if (key == "periods")
  {
    double v;
    if (!wantNumber(v))
      return false;
    opt.periods = unsigned(v);
    return true;
  }
  if (key == "gain")
    return wantNumber(opt.gain);
  if (key == "seconds")
    return wantNumber(opt.seconds);
  if (key == "meter")
    return wantNumber(opt.meterEvery);

  if (key == "param")
  {
    if (!value)
    {
      err = "--param attend IDX:VAL";
      return false;
    }
    const char* colon = std::strchr(value, ':');
    if (!colon)
    {
      err = "--param attend IDX:VAL";
      return false;
    }
    opt.params.emplace_back(std::atoi(value), std::atof(colon + 1));
    return true;
  }

  if (key == "no-ui" || key == "ui")
  {
    opt.ui = key == "ui";
    return true;
  }
  if (key == "no-in")
  {
    opt.noIn = true;
    return true;
  }
  if (key == "list")
  {
    opt.listDevices = true;
    return true;
  }
  if (key == "list-params")
  {
    opt.listParams = true;
    return true;
  }
  if (key == "check")
  {
    opt.check = true;
    return true;
  }

  err = "option inconnue : " + key;
  return false;
}

bool loadConfigFile(const std::string& path, Options& opt, std::string& err)
{
  FILE* f = std::fopen(path.c_str(), "r");

  if (!f)
    return true; // no config file is not an error

  char line[512];
  int lineNo = 0;

  while (std::fgets(line, sizeof(line), f))
  {
    ++lineNo;
    std::string s(line);

    const size_t hash = s.find('#');
    if (hash != std::string::npos)
      s.erase(hash);

    const size_t eq = s.find('=');
    if (eq == std::string::npos)
      continue;

    auto trim = [](std::string& t) {
      while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front())))
        t.erase(t.begin());
      while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
        t.pop_back();
    };

    std::string key = s.substr(0, eq);
    std::string value = s.substr(eq + 1);
    trim(key);
    trim(value);

    if (key.empty())
      continue;

    // A flag key with no value means "on"; everything else needs its value.
    std::string where = path + " ligne " + std::to_string(lineNo) + " : ";
    if (!applyOption(key, value.empty() ? nullptr : value.c_str(), opt, err))
    {
      err = where + err;
      std::fclose(f);
      return false;
    }
  }

  std::fclose(f);
  printf("reglages lus dans %s\n", path.c_str());
  return true;
}

bool parseArgs(int argc, char** argv, Options& opt)
{
  // The config file is read first so the command line always wins.
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h")
    {
      printUsage(argv[0]);
      return false;
    }

    if (arg == "--config")
    {
      if (i + 1 >= argc)
      {
        fprintf(stderr, "--config attend un chemin\n");
        return false;
      }
      opt.configFile = argv[++i];
    }
  }

  if (opt.configFile.empty())
  {
    if (const char* home = getenv("HOME"))
      opt.configFile = std::string(home) + "/.config/VoLum/live.conf";
  }

  std::string err;

  if (!opt.configFile.empty() && !loadConfigFile(opt.configFile, opt, err))
  {
    fprintf(stderr, "%s\n", err.c_str());
    return false;
  }

  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];

    if (arg.rfind("--", 0) != 0)
    {
      opt.module = arg; // the module may still be given as a bare argument
      continue;
    }

    std::string key = arg.substr(2);
    const char* value = nullptr;
    const size_t eq = key.find('=');

    if (eq != std::string::npos)
    {
      value = argv[i] + 2 + eq + 1; // --opt=value
      key.erase(eq);
    }
    else if (keyWantsValue(key) && i + 1 < argc && argv[i + 1][0] != '-')
    {
      value = argv[++i]; // --opt value
    }

    if (!applyOption(key, value, opt, err))
    {
      fprintf(stderr, "%s\n", err.c_str());
      return false;
    }
  }

  if (opt.module.empty())
  {
    if (const char* home = getenv("HOME"))
      opt.module = std::string(home) + "/.vst3/VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so";
    else
      opt.module = "~/.vst3/VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so";
  }

  // Capture comes from the same device as playback unless --in said otherwise; --no-in
  // asks for no capture at all, and the plug-in is then fed silence or a WAV.
  if (opt.noIn)
    opt.inDevice.clear();
  else if (opt.inDevice.empty())
    opt.inDevice = opt.outDevice;

  if (opt.period == 0)
    opt.period = 64;
  if (opt.periods == 0)
    opt.periods = 2;

  return true;
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

// MARK: PCM format

enum class PcmKind
{
  Float,
  S32,
  S16
};

unsigned int kindBytes(PcmKind k)
{
  return k == PcmKind::S16 ? 2u : 4u;
}

float loadSample(const char* p, PcmKind k)
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

void storeSample(char* p, PcmKind k, float v)
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

const char* streamLabel(snd_pcm_stream_t dir)
{
  return dir == SND_PCM_STREAM_CAPTURE ? "capture" : "lecture";
}

/** Open a device and negotiate the smallest setup that does the job: float samples if
 * the device speaks them, the plug-in's channel count where the device allows it, and
 * the requested rate and period as close as the hardware permits. */
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
  if (device == "default" || device == "sysdefault" || device == "pipewire" || device == "pulse")
    printf(
      "%-8s : peripherique du serveur son : la periode demandee est recoupee par le serveur,"
      "\n         la latence finale depend de sa periode a lui. Pour la carte en direct,"
      " utilise front: ou plughw:\n",
      streamLabel(dir));

  const double periodMs = 1000.0 * double(period) / double(rate);
  printf("%-8s \"%s\" : %u Hz, %s, %u canaux, periode %u (%.2f ms), tampon %u images (%u periodes)\n", streamLabel(dir),
         device.c_str(), rate, snd_pcm_format_name(s.format), channels, static_cast<unsigned int>(period), periodMs,
         s.bufferFrames, static_cast<unsigned int>(buf / period));

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

  while (done < frames && !gStop)
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

  while (done < frames && !gStop)
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

// MARK: WAV

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

double dbfs(double v)
{
  return v <= 1e-9 ? -180.0 : 20.0 * std::log10(v);
}

void reportLevels(const Engine& eng, double secondsRun)
{
  if (eng.counted == 0)
    return;

  const double rmsIn = std::sqrt(eng.sumIn / eng.counted);
  const double rmsOut = std::sqrt(eng.sumOut / eng.counted);

  printf("[%5.1f s] entree crete=%+.1f dBFS rms=%+.1f dBFS | sortie crete=%+.1f dBFS rms=%+.1f dBFS | %s\n", secondsRun,
         dbfs(eng.peakIn), dbfs(rmsIn), dbfs(eng.peakOut), dbfs(rmsOut),
         eng.peakIn > 0.999 ? "ENTREE SATUREE : baisse --gain ou le volume de la source" : "niveaux ok");
}

void resetLevels(Engine& eng)
{
  eng.peakIn = eng.peakOut = eng.sumIn = eng.sumOut = 0.0;
  eng.counted = 0;
}

// MARK: runs

/** A WAV standing in for the sound card's input. */
struct FileSource
{
  const std::vector<float>* data = nullptr;
  int channels = 0;
  size_t pos = 0;

  explicit operator bool() const { return data != nullptr; }
};

/** Copy the next `frames` frames of that WAV into the plug-in's input planes: a mono
 * file feeds every input, an over-wide one is trimmed. Says whether audio is still to
 * come, so a live listen can stop when the file ends. */
bool fillFromFile(FileSource& src, unsigned int frames, const std::vector<Sample32*>& ptr, double gain)
{
  const std::vector<float>& fileIn = *src.data;
  const size_t planar = size_t(std::max(1, src.channels));

  for (size_t c = 0; c < ptr.size(); ++c)
  {
    const int from = int(c) < src.channels ? int(c) : (src.channels == 1 ? 0 : -1);

    for (unsigned int i = 0; i < frames; ++i)
    {
      const size_t at = (src.pos + i) * planar + (from >= 0 ? size_t(from) : 0);
      const float v = (from >= 0 && at < fileIn.size()) ? fileIn[at] : 0.f;
      ptr[c][i] = static_cast<Sample32>(v * float(gain));
    }
  }

  src.pos += frames;
  return src.pos * planar < fileIn.size();
}

int runRealtime(const Options& opt, Stream& cap, Stream& play, Engine& eng, EditorSession& editor, bool hasEditor,
                FileSource& file, WavWriter& writer)
{
  const unsigned int frames = eng.blockFrames;
  const auto start = std::chrono::steady_clock::now();
  auto nextMeter = start + std::chrono::duration<double>(opt.meterEvery);
  const auto deadline =
    opt.seconds > 0 ? start + std::chrono::duration<double>(opt.seconds) : std::chrono::steady_clock::time_point::max();
  int status = 0;
  // --in is optional: without it the plug-in is fed silence, which is enough to
  // hear what an amp model does to a preset and to test the output path alone.
  const bool haveCapture = cap.pcm != nullptr;

  while (!gStop && std::chrono::steady_clock::now() < deadline)
  {
    if (file)
    {
      if (!fillFromFile(file, frames, eng.inPtr, opt.gain))
      {
        printf("fin du fichier d'entree : arret\n");
        break;
      }
    }
    else if (haveCapture)
    {
      const snd_pcm_sframes_t got = readFrames(cap, cap.raw.data(), frames);

      if (got < 0)
      {
        fprintf(stderr, "lecture de l'entree interrompue : %s\n", snd_strerror(int(got)));
        status = 4;
        break;
      }

      if (gStop)
        break;

      fillFromDevice(cap, frames, eng.inPtr, opt.gain);
    }
    else
      std::fill(eng.inStore.begin(), eng.inStore.end(), 0.f);

    eng.process();
    eng.meter();

    if (writer.isOpen())
      writer.block(eng.outPtr, frames);

    drainToDevice(play, frames, eng.outPtr);

    const snd_pcm_sframes_t put = writeFrames(play, play.raw.data(), frames);

    if (put < 0)
    {
      fprintf(stderr, "ecriture vers la sortie interrompue : %s\n", snd_strerror(int(put)));
      status = 4;
      break;
    }

    if (hasEditor)
    {
      editor.pumpEvents();

      if (editor.closed())
      {
        printf("fenetre fermee : arret\n");
        break;
      }
    }

    if (opt.meterEvery > 0 && std::chrono::steady_clock::now() >= nextMeter)
    {
      reportLevels(eng, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
      resetLevels(eng);
      nextMeter += std::chrono::duration<double>(opt.meterEvery);
    }
  }

  return status;
}

int runOffline(const Options& opt, const std::vector<float>& fileIn, int fileChannels, Engine& eng, WavWriter& writer,
               EditorSession& editor, bool hasEditor, double rate)
{
  const unsigned int frames = eng.blockFrames;
  const size_t totalFrames = fileIn.size() / size_t(std::max(1, fileChannels));

  const auto start = std::chrono::steady_clock::now();
  auto nextMeter = start + std::chrono::duration<double>(opt.meterEvery);
  FileSource file{&fileIn, fileChannels, 0};
  long doneFrames = 0;

  while (!gStop)
  {
    fillFromFile(file, frames, eng.inPtr, opt.gain);
    doneFrames += int(frames);

    eng.process();
    eng.meter();

    if (writer.isOpen())
      writer.block(eng.outPtr, frames);

    if (hasEditor)
    {
      editor.pumpEvents();

      if (editor.closed())
      {
        printf("fenetre fermee : arret\n");
        break;
      }
    }

    if (opt.meterEvery > 0 && std::chrono::steady_clock::now() >= nextMeter)
    {
      reportLevels(eng, double(doneFrames) / rate);
      resetLevels(eng);
      nextMeter += std::chrono::duration<double>(opt.meterEvery);
    }

    if (file.pos >= totalFrames)
      break;

    if (opt.seconds > 0 && double(doneFrames) / rate >= opt.seconds)
      break;

    // Paced like a real device: VoLum stages its amp model from a 20 ms idle timer, and
    // a run that finishes in a few milliseconds would never let it tick.
    const auto due = start + std::chrono::microseconds(static_cast<long long>((double(doneFrames) * 1e6) / rate));

    if (const auto now = std::chrono::steady_clock::now(); now < due)
      std::this_thread::sleep_until(due);
  }

  return 0;
}

} // namespace

int main(int argc, char** argv)
{
  Options opt;

  if (!parseArgs(argc, argv, opt))
    return 2;

  if (opt.listDevices)
  {
    listAlsaDevices();
    return 0;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  printf("=== volum-live : %s ===\n", opt.module.c_str());

  Loaded plugin;
  LoadOptions loadOpts;
  loadOpts.listClasses = false;
  loadOpts.listParams = opt.listParams ? 300 : 0;

  int exitCode = loadPlugin(opt.module, plugin, loadOpts);

  if (exitCode)
  {
    unloadPlugin(plugin);
    return exitCode;
  }

  applyParams(plugin, opt.params);

  const bool fromFile = !opt.inFile.empty();
  // A WAV input feeds the card as soon as an output device was asked for; with only
  // another WAV asked for, it is a plain offline conversion.
  const bool offline = fromFile && !opt.outRequested;
  Stream cap, play;
  std::vector<float> fileIn;
  int fileChannels = 2;
  unsigned int fileRate = 48000;
  double rate = opt.rate;
  unsigned int blockFrames = opt.period;
  std::string err;

  if (fromFile)
  {
    if (!readWav(opt.inFile, fileIn, fileChannels, fileRate, err))
    {
      fprintf(stderr, "%s\n", err.c_str());
      unloadPlugin(plugin);
      return 2;
    }

    printf("entree : %s, %d canaux, %u Hz, %ld images\n", opt.inFile.c_str(), fileChannels, fileRate,
           long(fileIn.size()) / std::max(1, fileChannels));

    if (offline)
      rate = opt.rate > 0 ? opt.rate : double(fileRate);
  }

  if (!offline)
  {
    if (!openStream(
          play, opt.outDevice, SND_PCM_STREAM_PLAYBACK, plugin.outChannels, rate, opt.period, opt.periods, err))
    {
      fprintf(stderr, "%s\n", err.c_str());
      unloadPlugin(plugin);
      return 4;
    }

    if (!fromFile && !opt.inDevice.empty())
    {
      if (!openStream(cap, opt.inDevice, SND_PCM_STREAM_CAPTURE, plugin.inChannels, rate, opt.period, opt.periods, err))
      {
        fprintf(stderr, "%s\n", err.c_str());
        closeStream(play);
        unloadPlugin(plugin);
        return 4;
      }
    }
    else if (fromFile)
      printf("entree  : \"%s\" lu en direct, aucune carte d'ouverte\n", opt.inFile.c_str());
    else
      printf("entree  : aucune capture (--no-in) : le plugin recevra du silence\n");

    if (cap.pcm && cap.rate != play.rate)
    {
      fprintf(stderr, "les deux flux ne tournent pas a la meme cadence (%u Hz en entree, %u Hz en sortie)\n", cap.rate,
              play.rate);
      fprintf(stderr, "force une cadence commune avec --rate, ou choisis un seul peripherique pour les deux\n");
      closeStream(cap);
      closeStream(play);
      unloadPlugin(plugin);
      return 4;
    }

    rate = double(play.rate);
    blockFrames = play.periodFrames;

    double queuedFrames = double(play.bufferFrames) + double(play.periodFrames);

    if (cap.pcm)
    {
      blockFrames = std::max(cap.periodFrames, play.periodFrames);
      queuedFrames += double(cap.bufferFrames) + double(cap.periodFrames);
    }

    printf("%s prevu dans la carte : %.1f ms (auxquels s'ajoutent le trajet USB et l'ampli)\n",
           cap.pcm ? "aller-retour" : "latence de sortie", queuedFrames * 1000.0 / rate);

    if (fromFile && fileRate != play.rate)
      printf("attention : le fichier est a %u Hz, la carte joue a %u Hz : il sera lu a la cadence de la carte\n",
             fileRate, play.rate);
  }

  if (!startProcessing(plugin, rate, int(blockFrames)))
  {
    fprintf(stderr, "le plugin a refuse %d echantillons par bloc a %.0f Hz : essaie --period %u\n", blockFrames, rate,
            blockFrames * 2);
    closeStream(cap);
    closeStream(play);
    unloadPlugin(plugin);
    return 5;
  }

  if (opt.check)
  {
    printf("verification : tout est negocie, rien n'est joue (--check)\n");
    plugin.processor->setProcessing(0);
    plugin.component->setActive(0);
    closeStream(cap);
    closeStream(play);
    unloadPlugin(plugin);
    return 0;
  }

  Engine eng;
  eng.setup(plugin, blockFrames);

  EditorSession editor;
  bool hasEditor = false;

  if (opt.ui)
  {
    const int editorStatus = editor.open(plugin, "VoLum live");

    if (editorStatus == 0)
      hasEditor = true;
    else
      printf("  suite sans fenetre : %s\n", editorStatus == 1 ? "le plugin n'expose pas d'editeur"
                                            : editorStatus == 2
                                              ? "createView n'a rien donne"
                                              : "pas de serveur X11 (DISPLAY) - relance depuis ta session");
  }

  WavWriter writer;

  if (!opt.outFile.empty() && !writer.open(opt.outFile, unsigned(plugin.outChannels), unsigned(rate)))
  {
    fprintf(stderr, "impossible d'ecrire \"%s\"\n", opt.outFile.c_str());
    exitCode = 2;
  }

  FileSource file{fromFile ? &fileIn : nullptr, fileChannels, 0};

  const int status = offline ? runOffline(opt, fileIn, fileChannels, eng, writer, editor, hasEditor, rate)
                             : runRealtime(opt, cap, play, eng, editor, hasEditor, file, writer);

  if (status)
    exitCode = status;

  printf("blocs traites : %ld, erreurs de traitement : %ld", eng.blocks, eng.processErrors);

  if (!offline)
    printf(", xruns : entree %ld / sortie %ld", cap.xruns, play.xruns);

  printf("\n");

  if (!offline && play.xruns > 8)
    printf(
      "  conseil : la sortie se vide regulierement (craquements audibles)."
      "\n   augmente --periods pour un tampon plus grand, ou --period si tu passes par le serveur son\n");

  // An input nobody fed (--no-in, no --in, no --in-file) is silence by construction, so
  // an empty output under it is not a fault to report.
  const bool fed = offline || fromFile || cap.pcm != nullptr;

  if (eng.runPeakOut < 1e-6 && eng.blocks > 0)
  {
    if (!fed)
      printf("entree non alimentee : le silence en sortie etait attendu (--in ou --in-file pour jouer)\n");
    else
    {
      printf("sortie muette : verifie le branchement, le volume de l'ampli, ou --gain\n");
      exitCode = exitCode ? exitCode : 6;
    }
  }
  else if (eng.blocks > 0)
    printf("verdict : sortie sonore\n");

  if (hasEditor)
    editor.close();

  plugin.processor->setProcessing(0);
  plugin.component->setActive(0);

  writer.close();

  closeStream(cap);
  closeStream(play);

  unloadPlugin(plugin);

  printf("=== volum-live termine (code %d) ===\n", exitCode);
  return exitCode;
}
