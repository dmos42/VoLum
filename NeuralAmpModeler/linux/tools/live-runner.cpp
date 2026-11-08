#include "live-runner.h"

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
                WavWriter& writer)
{
  Stream& cap = session.cap;
  Stream& play = session.play;
  FileSource file{session.fromFile ? &session.fileIn : nullptr, session.fileChannels, 0};
  const unsigned int frames = eng.blockFrames;
  const auto start = std::chrono::steady_clock::now();
  auto nextMeter = start + std::chrono::duration<double>(opt.meterEvery);
  const auto deadline =
    opt.seconds > 0 ? start + std::chrono::duration<double>(opt.seconds) : std::chrono::steady_clock::time_point::max();
  int status = 0;
  // --in is optional: without it the plug-in is fed silence, which is enough to
  // hear what an amp model does to a preset and to test the output path alone.
  const bool haveCapture = cap.pcm != nullptr;

  while (!stopRequested() && std::chrono::steady_clock::now() < deadline)
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

      if (stopRequested())
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

int runOffline(const Options& opt, Session& session, Engine& eng, WavWriter& writer, EditorSession& editor,
               bool hasEditor)
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

  while (!stopRequested())
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

int reportRun(const Session& session, Engine& eng, int exitCode)
{
  printf("blocs traites : %ld, erreurs de traitement : %ld", eng.blocks, eng.processErrors);

  if (!session.offline)
    printf(", xruns : entree %ld / sortie %ld", session.cap.xruns, session.play.xruns);

  printf("\n");

  if (!session.offline && session.play.xruns > 8)
    printf(
      "  conseil : la sortie se vide regulierement (craquements audibles)."
      "\n   augmente --periods pour un tampon plus grand, ou --period si tu passes par le serveur son\n");

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

  if (opt.check)
  {
    printf("verification : tout est negocie, rien n'est joue (--check)\n");
    plugin.processor->setProcessing(0);
    plugin.component->setActive(0);
    closeAudio(session);
    unloadPlugin(plugin);
    return 0;
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

  const int status = session.offline ? runOffline(opt, session, eng, writer, editor, hasEditor)
                                     : runRealtime(opt, session, eng, editor, hasEditor, writer);

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
