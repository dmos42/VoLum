/*
 ==============================================================================

 The loop: take a period from the card, hand it to the plug-in, put the result on
 the way out, keep the editor answering X11 while that runs.

 This is the part worth reusing rather than rewriting. It is where the choices that
 decide whether the guitar feels immediate are made - one period of buffering, no
 resampling in the middle of the callback, and the editor's own thread kept out of
 the audio path - and a standalone that grew its own copy would drift from the
 host that was measured.

 runRealtime is what you use with a card; runOffline is the same block sequence
 against WAV files, which is how the tests check the signal with no hardware.

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

/** Returns the process exit code: 0 fine, 4 the device failed, 5 the plug-in
 * refused the setup, 6 the output stayed silent. Stops on stopRequested(). */
int runRealtime(const Options& opt, Stream& cap, Stream& play, Engine& eng, EditorSession& editor, bool hasEditor,
                FileSource& file, WavWriter& writer);
int runOffline(const Options& opt, const std::vector<float>& fileIn, int fileChannels, Engine& eng, WavWriter& writer,
               EditorSession& editor, bool hasEditor, double rate);

} // namespace volumlinux
