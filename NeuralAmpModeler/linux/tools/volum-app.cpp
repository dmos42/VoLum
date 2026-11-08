/*
 ==============================================================================

 volum-app: VoLum as an application of its own, with no DAW in front of it.

 The Windows and macOS builds of VoLum ship two artifacts from one set of sources: the
 plug-in, and an standalone that is the same amplifier with its own window, its own
 settings file and its own audio device. On Linux the plug-in half was already there
 (volum-live proved the editor, the ALSA path and the block loop), so this file is the
 second half: the decisions an application makes that a host does not have to.

 Which are three, and that is all:

  - where its plug-in comes from. Nothing was told to this process, so app-support.h
    works it out from the folder the executable is in and the usual install folders.
  - where its settings come from. ~/.config/VoLum/app.conf, so a device chosen once
    stays chosen; the keys are the same as volum-live's on purpose.
  - when it is finished. Closing the window stops the audio and leaves, the way the
    window's close button does on Windows.

 Everything past those three - opening the device, running the blocks, pumping the
 editor, reporting what came out - is runHost(), shared with volum-live. That is the
 reason to have this file be small: two hosts with their own copy of the block loop is
 two hosts that can sound different.

 It is deliberately not a DAW: one stereo pair in and out, no timeline, no routing, no
 MIDI yet (issue #6). A guitar goes in, VoLum comes out.

 Usage
   volum-app [--help]              then close the window when you are done

 Exit codes: 0 ok, 2 usage or no plug-in found, 3 no usable class, 4 the audio device
 could not be used, 5 the plug-in refused the setup.

 ==============================================================================
*/

#include "app-support.h"
#include "live-runner.h"
#include "options-support.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace volumlinux;

namespace
{

void printUsage(const char* argv0)
{
  printf(
    "usage : %s [options]\n"
    "\n"
    "  --module PATH        paquet VST3 a charger (par defaut : cherche pres de ce\n"
    "                       programme, puis ~/.vst3 et les dossiers systemes)\n"
    "  --out DEV            peripherique ALSA de sortie (defaut : default)\n"
    "  --in DEV             peripherique ALSA d'entree (defaut : la meme que --out)\n"
    "  --no-in              aucune capture : le plugin recevra du silence\n"
    "  --rate N             cadence, 0 = celle de la carte (defaut : 0)\n"
    "  --period N           images par periode, aussi la taille du bloc traite (defaut : 256)\n"
    "  --periods N          periodes dans le tampon (defaut : 2 ; sur un peripherique du serveur\n"
    "                       son, le tampon total est porte a son quantum, 1024 images)\n"
    "  --gain X             gain d'entree avant le plugin (defaut : 1)\n"
    "  --param IDX:VAL      regle un parametre normalise avant de demarrer (repetable)\n"
    "  --in-file F.wav      ecouter un fichier dans la sortie au lieu du micro\n"
    "  --out-file F.wav     ecrire aussi la sortie dans un WAV\n"
    "  --no-ui              ne pas ouvrir la fenetre\n"
    "  --check              verifier la configuration et quitter\n"
    "  --list               lister les peripheriques ALSA et quitter\n"
    "  --config PATH        fichier de reglages (defaut : ~/.config/VoLum/app.conf)\n"
    "  --help               cette aide\n"
    "\n"
    "Les cles peuvent etre ecrites dans %s, une par ligne :\n"
    "  out=default\n"
    "  in=default\n"
    "  period=256\n"
    "\n"
    "le son sort hache ? sur un peripherique du serveur son, c'est le tampon total (periode x\n"
    "periodes) qui compte : il doit atteindre le quantum du serveur, 1024 images sur PipeWire.\n"
    "Le programme le fait deja ; pour un paquet plus ancien, sans rien reconstruire :\n"
    "  echo 'period=512' >> ~/.config/VoLum/app.conf\n"
    "\n"
    "lancer l'ampli, fenetre comprise :\n"
    "  %s\n"
    "\n"
    "prendre la carte en direct (latence la plus basse, le bureau n'a plus de son) :\n"
    "  %s --list\n"
    "  echo 'out=front:CARD=THR10X,DEV=0' >> ~/.config/VoLum/app.conf\n",
    argv0,
    (getenv("HOME") ? std::string(getenv("HOME")) + "/.config/VoLum/app.conf" : std::string("~/.config/VoLum/app.conf"))
      .c_str(),
    argv0, argv0);
}

} // namespace

int main(int argc, char** argv)
{
  Options opt;

  // A bundle that came with this executable wins over the per-user default, so an
  // unpacked or freshly built VoLum runs without being installed anywhere. parseArgs
  // only fills in its own default when nothing was decided here, and --module or a
  // config file still override what was found.
  opt.module = findAppBundleModule();
  opt.meterEvery = 0.0; // an application is not a test: it does not report levels by itself

  if (!parseArgs(argc, argv, opt, printUsage, "app.conf"))
    return 2;

  if (opt.module.empty() || !std::filesystem::is_regular_file(opt.module))
  {
    fprintf(stderr, "paquet VoLum.vst3 introuvable : \"%s\"\n", opt.module.c_str());
    fprintf(stderr, "ou il a ete cherche :\n");

    for (const std::string& bundle : appBundleSearchPath())
      fprintf(stderr, "  %s\n", bundle.c_str());

    fprintf(stderr, "installe-le (cmake --install build/linux) ou donne son chemin avec --module PATH\n");
    return 2;
  }

  return runHost(opt, "VoLum", "volum-app");
}
