#include "live-runner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace volumlinux
{

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

int runRealtime(const Options& opt, Stream& cap, Stream& play, Engine& eng, EditorSession& editor, bool hasEditor,
                FileSource& file, WavWriter& writer)
{
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

int runOffline(const Options& opt, const std::vector<float>& fileIn, int fileChannels, Engine& eng, WavWriter& writer,
               EditorSession& editor, bool hasEditor, double rate)
{
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
} // namespace volumlinux
