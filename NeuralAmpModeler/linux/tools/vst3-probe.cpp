/*
 ==============================================================================

 volum-linux: a minimal headless VST3 host, used to check the Linux port of VoLum.

 It answers the two questions a real DAW would otherwise have to answer:
   1. does the module load, enumerate its classes and instantiate cleanly?
   2. does audio actually come out of it, and does an editor window appear?

 The host plumbing (handler, frame, X11 window, module loading) lives in
 host-support.h, shared with volum-live.

 Usage
   volum-vst3-probe <path>/VoLum.vst3/Contents/x86_64-linux/<module>.so
                    [--seconds N]      audio run length (default 2)
                    [--rate R]         sample rate (default 48000)
                    [--param IDX:VAL]  set a normalized parameter before running
                    [--out-file F.wav] record the plug-in's output during the run, so the
                                       level over time can be looked at afterwards
                    [--ui]             also open the editor inside a probe window
                    [--uiseconds N]    how long to keep the editor open (default 6)
                    [--resize SEC:WxH] resize the editor from the host side after SEC seconds,
                                       repeat the option to try several sizes, one per step
                    [--click SEC:X:Y]  press and release the mouse at X:Y inside the
                                       editor window after SEC seconds, no real mouse
                                       involved
                    [--ui-only]        skip the audio run

 Exit codes: 0 ok, 2 cannot load, 3 no factory/class, 4 could not instantiate,
 5 processing failed, 6 output stayed silent, 7 the editor window did not survive a resize.

 ==============================================================================
*/

#include "host-support.h"
#include "wav-support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace volumlinux;

namespace
{

/** One thing the host does inside the editor window, SEC seconds after it opened: a
 *  size change (--resize) or a press-and-release (--click). One list because they
 *  share a timeline, and the order is part of the test - a DAW clicks in the window
 *  it has just sized, and a click into an unsized window is a different situation.
 *  `a`/`b` are a width and a height for a resize, an x and a y for a click, in the
 *  plug-in window's own coordinates. */
struct UIStep
{
  double at = 0.0;
  bool resize = false;
  int a = 0;
  int b = 0;
};

struct Options
{
  std::string path;
  double seconds = 2.0;
  double rate = 48000.0;
  bool audio = true;
  bool ui = false;
  int uiSeconds = 6;
  std::vector<UIStep> steps; // sizes and clicks the host sends, in the order asked for
  int maxParams = 250;
  std::string outFile; // write the plug-in output while the tone runs, to look at it afterwards
  std::vector<std::pair<int, double>> params; // parameter index -> normalized value
};

bool parseArgs(int argc, char** argv, Options& opt)
{
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];

    if (arg.rfind("--", 0) != 0)
    {
      opt.path = arg;
      continue;
    }

    if (arg == "--ui")
      opt.ui = true;
    else if (arg == "--ui-only")
    {
      opt.ui = true;
      opt.audio = false;
    }
    else if (const char* v = optionValue(arg, "--seconds", i, argc, argv))
      opt.seconds = std::atof(v);
    else if (const char* v = optionValue(arg, "--rate", i, argc, argv))
      opt.rate = std::atof(v);
    else if (const char* v = optionValue(arg, "--uiseconds", i, argc, argv))
      opt.uiSeconds = std::atoi(v);
    else if (const char* v = optionValue(arg, "--resize", i, argc, argv))
    {
      // SEC:WxH, repeatable. A DAW walks the editor through several sizes while the user drags
      // the container, and each one is a chance for the plug-in to touch OpenGL at a bad time.
      const char* colon = std::strchr(v, ':');
      const char* cross = colon ? std::strchr(colon, 'x') : nullptr;

      if (!colon || !cross)
      {
        fprintf(stderr, "--resize attend SECONDES:LARGEURxHAUTEUR\n");
        return false;
      }

      opt.steps.push_back({std::atof(v), true, std::atoi(colon + 1), std::atoi(cross + 1)});
    }
    else if (const char* v = optionValue(arg, "--click", i, argc, argv))
    {
      // SEC:X:Y, repeatable. No mouse is involved: the host posts the press and the release
      // to the plug-in's window, which reads them off its own connection like any other
      // event. This is how a button that opens a file chooser can be pressed by a script.
      const char* first = std::strchr(v, ':');
      const char* second = first ? std::strchr(first + 1, ':') : nullptr;

      if (!first || !second)
      {
        fprintf(stderr, "--click attend SECONDES:X:Y\n");
        return false;
      }

      opt.steps.push_back({std::atof(v), false, std::atoi(first + 1), std::atoi(second + 1)});
    }
    else if (const char* v = optionValue(arg, "--out-file", i, argc, argv))
      opt.outFile = v;
    else if (const char* v = optionValue(arg, "--param", i, argc, argv))
    {
      const int idx = std::atoi(v);
      const char* colon = std::strchr(v, ':');

      if (!colon)
      {
        fprintf(stderr, "--param attend IDX:VAL\n");
        return false;
      }

      opt.params.emplace_back(idx, std::atof(colon + 1));
    }
    else
    {
      fprintf(stderr, "option inconnue : %s\n", arg.c_str());
      return false;
    }
  }

  if (opt.path.empty())
  {
    fprintf(stderr, "usage : %s <module .so> [--seconds N] [--rate R] [--param IDX:VAL] [--ui]\n", argv[0]);
    return false;
  }

  return true;
}

} // namespace

int main(int argc, char** argv)
{
  Options opt;

  if (!parseArgs(argc, argv, opt))
    return 2;

  printf("=== volum-vst3-probe : %s ===\n", opt.path.c_str());

  Loaded plugin;
  LoadOptions loadOpts;
  loadOpts.listParams = opt.maxParams;

  const int loadStatus = loadPlugin(opt.path, plugin, loadOpts);
  int exitCode = loadStatus;

  if (exitCode)
  {
    unloadPlugin(plugin);
    printf("=== probe termine (code %d) ===\n", exitCode);
    return exitCode;
  }

  applyParams(plugin, opt.params);

  // MARK: audio
  if (opt.audio && plugin.processor)
  {
    const int blockLength = 512;

    if (!startProcessing(plugin, opt.rate, blockLength))
      exitCode = 5;

    const int blocks = static_cast<int>(opt.seconds * opt.rate / blockLength) + 1;

    std::vector<float> inStorage(static_cast<size_t>(plugin.inChannels) * blockLength, 0.f);
    std::vector<float> outStorage(static_cast<size_t>(plugin.outChannels) * blockLength, 0.f);
    std::vector<Steinberg::Vst::Sample32*> inPointers(static_cast<size_t>(plugin.inChannels), nullptr);
    std::vector<Steinberg::Vst::Sample32*> outPointers(static_cast<size_t>(plugin.outChannels), nullptr);

    for (int c = 0; c < plugin.inChannels; ++c)
      inPointers[c] = inStorage.data() + static_cast<size_t>(c) * blockLength;

    for (int c = 0; c < plugin.outChannels; ++c)
      outPointers[c] = outStorage.data() + static_cast<size_t>(c) * blockLength;

    Steinberg::Vst::AudioBusBuffers inputs;
    inputs.numChannels = plugin.inChannels;
    inputs.silenceFlags = 0;
    inputs.channelBuffers32 = inPointers.data();

    Steinberg::Vst::AudioBusBuffers outputs;
    outputs.numChannels = plugin.outChannels;
    outputs.silenceFlags = 0;
    outputs.channelBuffers32 = outPointers.data();

    Steinberg::Vst::ProcessData data;
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = blockLength;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inputs;
    data.outputs = &outputs;

    double peakIn = 0.0, peakOut = 0.0, sumOut = 0.0, sumIn = 0.0;
    long countedSamples = 0;
    // Recorded as it runs, not reconstructed afterwards: what matters here is the level
    // per second, which is how an amp that feeds on itself shows up (a quiet input coming
    // out as loud as a loud one) and no number printed at the end can tell that.
    volumlinux::WavWriter writer;

    if (!opt.outFile.empty()
        && !writer.open(
          opt.outFile, static_cast<unsigned int>(plugin.outChannels), static_cast<unsigned int>(opt.rate)))
    {
      fprintf(stderr, "impossible d'ecrire \"%s\"\n", opt.outFile.c_str());
      return 2;
    }
    long resultErrors = 0;
    double phase = 0.0;
    const double twoPi = 2.0 * 3.14159265358979312;
    const double phaseInc = twoPi * 440.0 / opt.rate;
    const int halfBlocks = blocks / 2 > 0 ? blocks / 2 : 1;
    // A real host feeds blocks as the audio device needs them. Without pacing the whole
    // run finishes in a few tens of milliseconds and the plug-in's 20 ms idle timer -
    // OnIdle(), where VoLum stages its amp model - never gets a single tick.
    const auto runStart = std::chrono::steady_clock::now();

    for (int b = 0; b < blocks; ++b)
    {
      // A 440 Hz test tone: loud for half the run, quiet for the other half, so the
      // difference between "pass-through" and "modelled / compressed" is visible.
      const double amp = b < halfBlocks ? 0.2 : 0.05;

      for (int i = 0; i < blockLength; ++i)
      {
        phase += phaseInc;

        if (phase > twoPi)
          phase -= twoPi;

        const double tone = std::sin(phase) * amp;

        for (int c = 0; c < plugin.inChannels; ++c)
          inPointers[c][i] = static_cast<float>(c == 0 ? tone : 0.0);
      }

      const Steinberg::tresult r = plugin.processor->process(data);

      if (writer.isOpen())
        writer.block(outPointers, blockLength);

      if (r != Steinberg::kResultOk)
        ++resultErrors;

      for (int i = 0; i < blockLength; ++i)
      {
        const double inSample = inPointers[0][i];
        const double outSample = outPointers[0][i];

        if (b > 0) // skip the very first block: model loading and ramps land there
        {
          peakIn = peakIn > std::abs(inSample) ? peakIn : std::abs(inSample);
          peakOut = peakOut > std::abs(outSample) ? peakOut : std::abs(outSample);
          sumIn += inSample * inSample;
          sumOut += outSample * outSample;
          ++countedSamples;
        }
      }

      const auto due = runStart
                       + std::chrono::microseconds(
                         static_cast<long long>((static_cast<double>(b + 1) * blockLength * 1e6) / opt.rate));

      if (const auto now = std::chrono::steady_clock::now(); now < due)
        std::this_thread::sleep_until(due);
    }

    const double rmsIn = countedSamples ? std::sqrt(sumIn / countedSamples) : 0.0;
    const double rmsOut = countedSamples ? std::sqrt(sumOut / countedSamples) : 0.0;

    // Patched before the numbers are printed: a WAV whose sizes are still zero plays in a
    // streaming player and is refused by anything that reads the header first, which is
    // exactly what the file is recorded for.
    writer.close();

    printf("traitement : %d blocs de %d, erreurs=%ld\n", blocks, blockLength, resultErrors);
    printf("entree : crete=%.4f rms=%.4f | sortie : crete=%.4f rms=%.4f\n", peakIn, rmsIn, peakOut, rmsOut);

    if (resultErrors)
      exitCode = 5;

    if (peakOut < 1e-6)
    {
      printf("verdict : SILENCE en sortie\n");
      exitCode = exitCode ? exitCode : 6;
    }
    else
      printf("verdict : sortie sonore\n");

    plugin.processor->setProcessing(0);
    plugin.component->setActive(0);
  }
  else if (opt.audio)
  {
    printf("pas de traitement audio : la classe n'implemente pas IAudioProcessor\n");
    exitCode = 5;
  }

  // MARK: editor
  if (opt.ui)
  {
    EditorSession editor;
    const int editorStatus = editor.open(plugin, "VoLum Linux probe host");

    if (editorStatus == 1)
      printf("pas d'editeur : la classe n'expose pas IEditController\n");
    else if (editorStatus == 3)
      printf("impossible d'ouvrir une fenetre X11 pour l'editeur\n");
    else if (editorStatus == 0)
    {
      // A DAW sizes the editor inside its own container, between two of the plug-in's frames:
      // the plug-in only ever sees a ConfigureNotify for a window whose size it does not own.
      // Walk the window through the sizes given on the command line, and check it is still
      // there afterwards - a resize is where a plug-in reaches for OpenGL while its window
      // thread has no context current. Clicks ride the same clock: they are what gets a
      // plug-in to open a dialog, a folder chooser, a preset load.
      std::stable_sort(
        opt.steps.begin(), opt.steps.end(), [](const UIStep& one, const UIStep& other) { return one.at < other.at; });

      double clock = 0.0;

      for (const UIStep& step : opt.steps)
      {
        editor.pump(step.at > clock ? step.at - clock : 0.0);
        clock = step.at;

        if (step.resize)
        {
          if (!editor.resizeEmbedded(static_cast<unsigned>(step.a), static_cast<unsigned>(step.b)))
            printf("[hote] pas de fenetre de plugin a redimensionner a %.1f s\n", step.at);
          else if (editor.closed())
            printf("[hote] editeur abandonne au redimensionnement a %.1f s\n", step.at);
          else
            printf("[hote] editeur toujours present apres %d x %d\n", step.a, step.b);
        }
        else
        {
          if (!editor.clickEmbedded(step.a, step.b))
            printf("[hote] pas de fenetre de plugin pour un clic a %.1f s\n", step.at);

          // Whatever the click was for, the editor has to still be there when it is over:
          // a plug-in that takes a SIGSEGV inside a file dialog never comes back to report it.
          editor.pump(0.25);
        }

        if (!editor.closed())
          continue;

        if (exitCode == kLoadOk)
          exitCode = 7;

        break;
      }

      if (!editor.closed())
        editor.pump(opt.uiSeconds > clock ? opt.uiSeconds - clock : 0.0);

      editor.sizeReport(opt.uiSeconds);
      editor.close();
    }

    if (exitCode == kLoadOk && editorStatus)
      exitCode = 6;
  }

  unloadPlugin(plugin);

  printf("=== probe termine (code %d) ===\n", exitCode);
  return exitCode;
}
