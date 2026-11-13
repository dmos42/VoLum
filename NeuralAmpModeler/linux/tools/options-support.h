/*
 ==============================================================================

 The shared option surface: the same keys, read from the command line and from a
 config file, for every host that runs audio. One place, because the keys are also
 what a settings window will have to read and write later - an option named
 differently in two tools is a bug the user finds, not us.

 A tool keeps its own usage text and its own config file name; the parser, the
 precedence (config file first, command line wins) and the accepted values are
 shared.

 ==============================================================================
*/

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace volumlinux
{

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
  bool noMidi = false; // --no-midi: no ALSA seq port for the plug-in to listen on
  double rate = 0.0; // 0 = whatever the device prefers, starting from 48000
  // The block size handed to the plug-in: 256 frames is 5.33 ms at 48 kHz, big enough that
  // a neural model is not re-entered every 1.3 ms, small enough that a guitar still feels
  // immediate. What decides whether the sound comes out in pieces on a machine shared with a
  // sound server is the total buffer against the server quantum, and openStream() holds that
  // floor itself instead of trusting this number - which is why 64 frames is still reachable,
  // through a server as well as on a card held directly.
  unsigned int period = 256;
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

bool keyWantsValue(const std::string& key);
bool applyOption(const std::string& key, const char* value, Options& opt, std::string& err);
bool loadConfigFile(const std::string& path, Options& opt, std::string& err);

/** The defaults a half-filled Options still needs: capture falls back to the
 * playback device, the period and the period count get their working values.
 * parseArgs ends on this; the host's read-back of a rewritten config file ends
 * on it too, because the panel writes single keys and the rest of the file may
 * say nothing at all. */
void NormalizeOptions(Options& opt);

/** The four keys the audio panel (issue #10) manages, as the config file last
 * said them - the snapshot a change is diffed against. */
struct ConfSnapshot
{
  std::string out, in;
  unsigned int period = 0;
  unsigned int periods = 0;
  bool noIn = false;
};

/** Re-read the config file and apply onto opt only the keys that changed since
 * the snapshot was taken. The panel writes the file behind the host's back;
 * this is the half of that conversation the host answers. Returns false when
 * the file could not be read (err says why); a readable file with no change
 * returns true and touches nothing. */
bool ApplyConfChanges(const std::string& path, Options& opt, ConfSnapshot& seen, std::string& err);

/** Config file first, then the command line, then the defaults. printUsage and
 * defaultConfigName (the file under ~/.config/VoLum) are the tool's own. */
bool parseArgs(int argc, char** argv, Options& opt, void (*printUsage)(const char*), const char* defaultConfigName);

} // namespace volumlinux
