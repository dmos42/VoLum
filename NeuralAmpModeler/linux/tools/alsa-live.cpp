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

 What is left in this file is what belongs to a command line tool: the option names and
 their text. Opening the device, running the blocks and reporting the result is in
 live-runner.{h,cpp}, where the standalone host (volum-app) reaches for the same code -
 which is the point: a device that works here works there.

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

#include "live-runner.h"
#include "options-support.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace volumlinux;

namespace
{

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

  return runHost(opt, "VoLum live", "volum-live");
}
