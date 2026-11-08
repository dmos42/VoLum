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

#include "alsa-support.h"
#include "engine-support.h"
#include "live-runner.h"
#include "options-support.h"
#include "wav-support.h"

#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace volumlinux;
using Steinberg::Vst::AudioBusBuffers;
using Steinberg::Vst::ProcessData;
using Steinberg::Vst::Sample32;

namespace
{

void onSignal(int)
{
  requestStop();
}

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


} // namespace

int main(int argc, char** argv)
{
  Options opt;

  if (!parseArgs(argc, argv, opt, printUsage, "live.conf"))
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
