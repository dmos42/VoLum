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
                    [--ui]             also open the editor inside a probe window
                    [--uiseconds N]    how long to keep the editor open (default 6)
                    [--ui-only]        skip the audio run

 Exit codes: 0 ok, 2 cannot load, 3 no factory/class, 4 could not instantiate,
 5 processing failed, 6 output stayed silent.

 ==============================================================================
*/

#include "host-support.h"

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

struct Options
{
  std::string path;
  double seconds = 2.0;
  double rate = 48000.0;
  bool audio = true;
  bool ui = false;
  int uiSeconds = 6;
  int maxParams = 250;
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
      editor.pump(opt.uiSeconds);
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
