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
  double rate = 0.0; // 0 = whatever the device prefers, starting from 48000
  // 256 frames, not 64. On a server-backed device - "default", which is what an
  // installed VoLum and the AppImage both get - the server runs its own quantum (1024
  // on this PipeWire) and cuts our period out of it. Measured here, full duplex with
  // the window open: at 64 the playback stream runs dry several times a minute, and a
  // guitar comes out in pieces; at 256 and 512 it never did. A 64-frame period is still
  // what a directly held card wants (1.3 ms), so that is one flag away, not a rebuild.
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

/** Config file first, then the command line, then the defaults. printUsage and
 * defaultConfigName (the file under ~/.config/VoLum) are the tool's own. */
bool parseArgs(int argc, char** argv, Options& opt, void (*printUsage)(const char*), const char* defaultConfigName);

} // namespace volumlinux
