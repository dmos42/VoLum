#include "live-runner.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace volumlinux
{

namespace
{

void onSignal(int)
{
  requestStop();
}

/** The audio panel writes app.conf behind the host's back (issue #10); the
 * file's mtime is the channel back. Polled at most twice a second: a stat()
 * per block would be a syscall in the loop that paces the audio, for nothing.
 * Absence is a state too - the panel creates the file on its first write. */
class ConfWatcher
{
public:
  void Arm(std::string path)
  {
    mPath = std::move(path);
    mLast = StatMtime();
  }

  /** True once per actual change of the file, false while it stands still. */
  bool Changed()
  {
    if (mPath.empty())
      return false;

    const long long now = StatMtime();

    if (now == mLast)
      return false;

    mLast = now;
    return true;
  }

private:
  long long StatMtime() const
  {
    struct stat st;

    if (::stat(mPath.c_str(), &st) != 0)
      return -1;

    return static_cast<long long>(st.st_mtim.tv_sec) * 1000000000ll + st.st_mtim.tv_nsec;
  }

  std::string mPath;
  long long mLast = -1;
};

/** The stream has to be reopened for a device or period change to mean
 * anything: the block size the plug-in was set up with and the buffers the
 * card holds are the old ones until this runs. The gap this costs is exactly
 * what the panel promised in its status line. A device that refuses to open
 * (the desktop is holding the card) is answered by giving the previous
 * settings back - the run continues on what was working, and the reason is on
 * the terminal, where the panel said the verdict would be printed. */
int ReopenStream(Options& opt, const Options& previous, Loaded& plugin, Session& session, Engine& eng,
                 WavWriter& writer)
{
  const unsigned int oldBlock = session.blockFrames;
  const unsigned int oldRate = session.rate;

  plugin.processor->setProcessing(0);
  plugin.component->setActive(0);
  closeAudio(session);

  std::string err;
  int rc = openAudio(opt, plugin, session, err);

  if (rc != 0)
  {
    fprintf(stderr, "reouverture impossible sur le nouveau reglage : %s\n", err.c_str());
    fprintf(stderr, "retour aux reglages precedents\n");

    opt = previous; // the run continues on what was working; the options say so too
    session = Session{};
    rc = openAudio(opt, plugin, session, err);

    if (rc != 0)
    {
      fprintf(stderr, "l'ancien peripherique refuse lui aussi : %s\n", err.c_str());
      return rc;
    }
  }

  eng.setup(plugin, session.blockFrames);

  if (writer.isOpen() && session.rate != oldRate)
  {
    writer.close();
    printf("out-file : ferme, la cadence a change (%u Hz -> %u Hz)\n", oldRate, session.rate);
  }

  if (session.blockFrames != oldBlock || session.rate != oldRate)
    printf("reouverture : period negocie %u, cadence %u Hz, tampon %u images\n", session.blockFrames, session.rate,
           session.play.bufferFrames);
  else
    printf("reouverture : period negocie %u, tampon %u images\n", session.blockFrames, session.play.bufferFrames);

  return 0;
}

} // namespace

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

int openAudio(const Options& opt, Loaded& plugin, Session& session, std::string& err)
{
  session.fromFile = !opt.inFile.empty();
  // A WAV input feeds the card as soon as an output device was asked for; with only
  // another WAV asked for, it is a plain offline conversion.
  session.offline = session.fromFile && !opt.outRequested;

  double rate = opt.rate;
  unsigned int blockFrames = opt.period;

  if (session.fromFile)
  {
    if (!readWav(opt.inFile, session.fileIn, session.fileChannels, session.fileRate, err))
      return 2;

    printf("entree : %s, %d canaux, %u Hz, %ld images\n", opt.inFile.c_str(), session.fileChannels, session.fileRate,
           long(session.fileIn.size()) / std::max(1, session.fileChannels));

    if (session.offline)
      rate = opt.rate > 0 ? opt.rate : double(session.fileRate);
  }

  if (!session.offline)
  {
    if (!openStream(
          session.play, opt.outDevice, SND_PCM_STREAM_PLAYBACK, plugin.outChannels, rate, opt.period, opt.periods, err))
      return 4;

    if (!session.fromFile && !opt.inDevice.empty())
    {
      if (!openStream(
            session.cap, opt.inDevice, SND_PCM_STREAM_CAPTURE, plugin.inChannels, rate, opt.period, opt.periods, err))
        return 4;
    }
    else if (session.fromFile)
      printf("entree  : \"%s\" lu en direct, aucune carte d'ouverte\n", opt.inFile.c_str());
    else
      printf("entree  : aucune capture (--no-in) : le plugin recevra du silence\n");

    if (session.cap.pcm && session.cap.rate != session.play.rate)
    {
      fprintf(stderr, "les deux flux ne tournent pas a la meme cadence (%u Hz en entree, %u Hz en sortie)\n",
              session.cap.rate, session.play.rate);
      fprintf(stderr, "force une cadence commune avec --rate, ou choisis un seul peripherique pour les deux\n");
      return 4;
    }

    rate = double(session.play.rate);
    blockFrames = session.play.periodFrames;

    // What the user hears as latency: what is queued in the card before the plug-in
    // sees a sample, plus what we hand the card ahead of time. The USB trip and the
    // amplifier are on top of this, and neither is ours to measure.
    double queuedFrames = double(session.play.bufferFrames) + double(session.play.periodFrames);

    if (session.cap.pcm)
    {
      blockFrames = std::max(session.cap.periodFrames, session.play.periodFrames);
      queuedFrames += double(session.cap.bufferFrames) + double(session.cap.periodFrames);
    }

    printf("%s prevu dans la carte : %.1f ms (auxquels s'ajoutent le trajet USB et l'ampli)\n",
           session.cap.pcm ? "aller-retour" : "latence de sortie", queuedFrames * 1000.0 / rate);

    if (session.fromFile && session.fileRate != session.play.rate)
      printf("attention : le fichier est a %u Hz, la carte joue a %u Hz : il sera lu a la cadence de la carte\n",
             session.fileRate, session.play.rate);
  }

  session.rate = rate;
  session.blockFrames = blockFrames;

  if (!startProcessing(plugin, session.rate, int(session.blockFrames)))
  {
    fprintf(stderr, "le plugin a refuse %d echantillons par bloc a %.0f Hz : essaie --period %u\n", session.blockFrames,
            session.rate, session.blockFrames * 2);
    return 5;
  }

  return 0;
}

void closeAudio(Session& session)
{
  closeStream(session.cap);
  closeStream(session.play);
}

int runRealtime(const Options& opt, Session& session, Engine& eng, EditorSession& editor, bool hasEditor,
                WavWriter& writer, Loaded& plugin, MidiIn& midi)
{
  Stream& cap = session.cap;
  Stream& play = session.play;
  FileSource file{session.fromFile ? &session.fileIn : nullptr, session.fileChannels, 0};
  const auto start = std::chrono::steady_clock::now();
  auto nextMeter = start + std::chrono::duration<double>(opt.meterEvery);
  const auto deadline =
    opt.seconds > 0 ? start + std::chrono::duration<double>(opt.seconds) : std::chrono::steady_clock::time_point::max();
  int status = 0;

  // The audio panel (issue #10) writes app.conf behind the host's back; the
  // watcher is the host's half of that conversation. A run fed from a file has
  // no device to reopen, and an offline run has no live settings to change.
  Options live = opt;
  ConfWatcher conf;
  ConfSnapshot seen;
  seen.out = live.outDevice;
  seen.in = live.inDevice;
  seen.period = live.period;
  seen.periods = live.periods;
  seen.noIn = live.noIn;
  const bool watchConf = !session.offline && !session.fromFile && !live.configFile.empty();

  if (watchConf)
    conf.Arm(live.configFile);

  auto pollConf = [&]() {
    if (!watchConf || !conf.Changed())
      return;

    Options previous = live;
    std::string err;

    if (!ApplyConfChanges(live.configFile, live, seen, err))
    {
      fprintf(stderr, "reglages relus mais inutilisables : %s\n", err.c_str());
      return;
    }

    printf("reglage audio change dans %s : reouverture du flux\n", live.configFile.c_str());

    if (ReopenStream(live, previous, plugin, session, eng, writer) != 0)
      status = 4;
  };

  std::vector<RawMidiEvent> midiEvents;
  std::string midiErr;

  while (!stopRequested() && status == 0 && std::chrono::steady_clock::now() < deadline)
  {
    pollConf();

    // The port is drained once per block, on the audio clock: what a foot
    // controller sent is stamped into the block now starting, and handed to the
    // plug-in the way a DAW would - through the event list the block carries.
    if (midi.opened() && !midi.drain(midiEvents, session.rate, int(eng.blockFrames), midiErr))
    {
      fprintf(stderr, "midi : %s\n", midiErr.c_str());
      status = 4;
      break;
    }

    for (const RawMidiEvent& ev : midiEvents)
      eng.queueMidi(ev.bytes.data(), ev.bytes.size(), ev.offset);

    const unsigned int frames = eng.blockFrames;

    if (file)
    {
      if (!fillFromFile(file, frames, eng.inPtr, live.gain))
      {
        printf("fin du fichier d'entree : arret\n");
        break;
      }
    }
    else if (cap.pcm != nullptr) // re-read each pass: a reopen can add or drop capture
    {
      const snd_pcm_sframes_t got = readFrames(cap, cap.raw.data(), frames);

      if (got < 0)
      {
        fprintf(stderr, "lecture de l'entree interrompue : %s\n", snd_strerror(int(got)));
        status = 4;
        break;
      }

      if (stopRequested())
        break;

      fillFromDevice(cap, frames, eng.inPtr, live.gain);
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

    if (live.meterEvery > 0 && std::chrono::steady_clock::now() >= nextMeter)
    {
      reportLevels(eng, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
      resetLevels(eng);
      nextMeter += std::chrono::duration<double>(live.meterEvery);
    }
  }

  return status;
}

int runOffline(const Options& opt, Session& session, Engine& eng, WavWriter& writer, EditorSession& editor,
               bool hasEditor, MidiIn& midi)
{
  const std::vector<float>& fileIn = session.fileIn;
  const int fileChannels = session.fileChannels;
  const double rate = session.rate;
  const unsigned int frames = eng.blockFrames;
  const size_t totalFrames = fileIn.size() / size_t(std::max(1, fileChannels));

  const auto start = std::chrono::steady_clock::now();
  auto nextMeter = start + std::chrono::duration<double>(opt.meterEvery);
  FileSource file{&fileIn, fileChannels, 0};
  long doneFrames = 0;

  std::vector<RawMidiEvent> midiEvents;
  std::string midiErr;

  while (!stopRequested())
  {
    if (midi.opened() && !midi.drain(midiEvents, rate, int(frames), midiErr))
    {
      fprintf(stderr, "midi : %s\n", midiErr.c_str());
      return 4;
    }

    for (const RawMidiEvent& ev : midiEvents)
      eng.queueMidi(ev.bytes.data(), ev.bytes.size(), ev.offset);

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

int reportRun(const Session& session, Engine& eng, int exitCode)
{
  printf("blocs traites : %ld, erreurs de traitement : %ld", eng.blocks, eng.processErrors);

  if (!session.offline)
    printf(", xruns : entree %ld / sortie %ld", session.cap.xruns, session.play.xruns);

  printf("\n");

  // One underrun is one gap a listener can hear, so this does not wait for a big
  // number to start talking about it.
  if (!session.offline && session.play.xruns > 0)
    printf(
      "  conseil : la sortie s'est videe %ld fois (craquements, son hache)."
      "\n   le tampon annonce plus haut doit atteindre le quantum du serveur son - 1024 images"
      "\n   sur PipeWire. S'il y est deja, ce n'est plus le drapeau : c'est le reste du systeme"
      "\n   qui prend le temps de la periode. Baisse --periods, ou remonte le quantum du"
      "\n   serveur (pw-metadata -n settings 0 clock.quantum).\n",
      session.play.xruns);

  // An input nobody fed (--no-in, no --in, no --in-file) is silence by construction, so
  // an empty output under it is not a fault to report.
  if (eng.runPeakOut < 1e-6 && eng.blocks > 0)
  {
    if (!session.fed())
      printf("entree non alimentee : le silence en sortie etait attendu (--in ou --in-file pour jouer)\n");
    else
    {
      printf("sortie muette : verifie le branchement, le volume de l'ampli, ou --gain\n");
      exitCode = exitCode ? exitCode : 6;
    }
  }
  else if (eng.blocks > 0)
    printf("verdict : sortie sonore\n");

  return exitCode;
}

int runHost(const Options& opt, const char* windowTitle, const char* toolName)
{
  if (opt.listDevices)
  {
    listAlsaDevices();
    return 0;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  printf("=== %s : %s ===\n", toolName, opt.module.c_str());

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

  Session session;
  std::string err;

  if ((exitCode = openAudio(opt, plugin, session, err)) != 0)
  {
    if (!err.empty())
      fprintf(stderr, "%s\n", err.c_str());

    closeAudio(session);
    unloadPlugin(plugin);
    return exitCode;
  }

  // The standalone's MIDI source (issue #11): one readable ALSA seq port, named
  // so a patchbay finds it. No seq driver on the machine is a normal answer - the
  // run continues without MIDI, and the reason is on the terminal. Opened before
  // --check answers, so a check reports the port along with the rest.
  MidiIn midi;

  if (!opt.noMidi)
  {
    std::string midiErr;

    if (midi.open(midiErr))
      printf("midi : port \"VoLum\" \"MIDI In\" ouvert (%s) - branche une source : aconnect <client:port> %s\n",
             midi.portId().c_str(), midi.portId().c_str());
    else
      printf("midi : pas de port (%s) - le plugin tournera sans MIDI\n", midiErr.c_str());
  }

  if (opt.check)
  {
    printf("verification : tout est negocie, rien n'est joue (--check)\n");
    plugin.processor->setProcessing(0);
    plugin.component->setActive(0);
    closeAudio(session);
    unloadPlugin(plugin);
    return 0;
  }

  // The audio panel (issue #10) reads the negotiated truth from the environment
  // when it opens: what the device actually agreed to, not what was asked for.
  // A DAW host never sets VOLUM_APP_CONF, so these names mean nothing there.
  if (const char* marker = getenv("VOLUM_APP_CONF"); marker && *marker)
  {
    setenv("VOLUM_APP_RATE", std::to_string(session.rate).c_str(), 1);
    setenv("VOLUM_APP_BLOCK", std::to_string(session.blockFrames).c_str(), 1);
    setenv("VOLUM_APP_BUFFER", std::to_string(session.play.bufferFrames).c_str(), 1);
  }

  Engine eng;
  eng.setup(plugin, session.blockFrames);

  EditorSession editor;
  bool hasEditor = false;

  if (opt.ui)
  {
    const int editorStatus = editor.open(plugin, windowTitle);

    if (editorStatus == 0)
      hasEditor = true;
    else
      printf("  suite sans fenetre : %s\n", editorStatus == 1 ? "le plugin n'expose pas d'editeur"
                                            : editorStatus == 2
                                              ? "createView n'a rien donne"
                                              : "pas de serveur X11 (DISPLAY) - relance depuis ta session");
  }

  WavWriter writer;

  if (!opt.outFile.empty() && !writer.open(opt.outFile, unsigned(plugin.outChannels), unsigned(session.rate)))
  {
    fprintf(stderr, "impossible d'ecrire \"%s\"\n", opt.outFile.c_str());
    exitCode = 2;
  }

  const int status = session.offline ? runOffline(opt, session, eng, writer, editor, hasEditor, midi)
                                     : runRealtime(opt, session, eng, editor, hasEditor, writer, plugin, midi);

  if (status)
    exitCode = status;

  exitCode = reportRun(session, eng, exitCode);

  if (hasEditor)
    editor.close();

  plugin.processor->setProcessing(0);
  plugin.component->setActive(0);

  writer.close();

  closeAudio(session);

  unloadPlugin(plugin);

  printf("=== %s termine (code %d) ===\n", toolName, exitCode);
  return exitCode;
}
} // namespace volumlinux
