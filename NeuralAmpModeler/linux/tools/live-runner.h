/*
 ==============================================================================

 The loop: take a period from the card, hand it to the plug-in, put the result on
 the way out, keep the editor answering X11 while that runs.

 This is the part worth reusing rather than rewriting. It is where the choices that
 decide whether the guitar feels immediate are made - one period of buffering, no
 resampling in the middle of the callback, and the editor's own thread kept out of
 the audio path - and a standalone that grew its own copy would drift from the
 host that was measured.

 openAudio is the other half: reading --in-file if one was given, opening the
 card's streams, and letting the plug-in accept the rate and block size the device
 agreed to. volum-live and the standalone go through the same door, so a device
 that works in one works in the other, and the closing report says the same thing
 in both.

 ==============================================================================
*/

#pragma once

#include <vector>

#include "alsa-support.h"
#include "engine-support.h"
#include "host-support.h"
#include "options-support.h"
#include "wav-support.h"

namespace volumlinux
{

using Steinberg::Vst::Sample32;

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

bool fillFromFile(FileSource& src, unsigned int frames, const std::vector<Sample32*>& ptr, double gain);

/** What has to exist before the block loop can turn: the card's two streams, or the WAV
 * standing in for the capture, and the rate and block size the device settled on.
 *
 * Both hosts keep this in one place because the order the streams are opened in and the
 * numbers that come back out of the negotiation are not something to have two opinions
 * about: the block size the plug-in was set up with has to be the one the loop asks for. */
struct Session
{
  Stream cap, play;
  std::vector<float> fileIn;
  int fileChannels = 2;
  unsigned int fileRate = 48000;
  bool fromFile = false; // --in-file: something other than the card feeds the input
  bool offline = false; // nothing plays through a device; the WAV in becomes a WAV out
  double rate = 0.0;
  unsigned int blockFrames = 0;

  /** Was the plug-in given anything to work on? An input nobody fed is silence by
   * construction, so a silent output under it is not a fault worth reporting. */
  bool fed() const { return offline || fromFile || cap.pcm != nullptr; }
};

/** Read --in-file, open the capture and playback streams unless the run is offline, and
 * set the plug-up at the rate and block size that came out. Returns 0 with the session
 * ready to run, or the exit code the caller should leave with: 2 an unreadable WAV,
 * 4 a device that could not be used, 5 the plug-in refused the setup. */
int openAudio(const Options& opt, Loaded& plugin, Session& session, std::string& err);

/** Close both streams. Safe to call on a session that never got as far as opening one. */
void closeAudio(Session& session);

/** Returns the process exit code: 0 fine, 4 the device failed, 5 the plug-in
 * refused the setup, 6 the output stayed silent. Stops on stopRequested(). */
int runRealtime(const Options& opt, Session& session, Engine& eng, EditorSession& editor, bool hasEditor,
                WavWriter& writer);
int runOffline(const Options& opt, Session& session, Engine& eng, WavWriter& writer, EditorSession& editor,
               bool hasEditor);

/** What both hosts print when the loop stops: how many blocks went through, how many
 * times the device ran dry, what to do about that, and whether anything came out. Takes
 * the exit code so far and returns the one to leave with. */
int reportRun(const Session& session, Engine& eng, int exitCode);

/** The whole run of a host that drives the plug-in: answer --list, take over the
 * signals, load the module, open the audio, put the editor in a window called
 * `windowTitle`, run the blocks, report, and let everything go in the order the SDK
 * wants. `toolName` is what the banner and the closing line say.
 *
 * volum-live and the standalone call this rather than each carrying their own copy of
 * the sequence, because the order is not free: the editor has to be attached before the
 * loop starts pumping it, and the plug-in has to be put back to sleep before the module
 * is unloaded or it takes the window with it. */
int runHost(const Options& opt, const char* windowTitle, const char* toolName);

} // namespace volumlinux
