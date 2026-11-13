/*
 ==============================================================================

 The wrapper check (issue #9). One tone, three surfaces, one answer: the direct
 engine (the VST3 way the probe drives), the LV2 module and the CLAP module
 must produce byte-identical audio, and a parameter change fed through each
 surface's own plumbing - a control port for LV2, a PARAM_VALUE event for CLAP,
 the block's parameter changes for the direct engine - must land in the DSP the
 same way on all three.

 This is the check the issue asks for in its own words: the same WAV through
 LV2 and through VST3, the same SHA256; CLAP, the same engine behind a second
 surface. It also pins the parameter count the issue quotes (233), so a
 parameter added upstream is a visible change here, not a silent port drift.

 Order matters: each surface is opened once, then left alone for a moment
 before any audio moves. VoLum stages its amp model from the 20 ms idle timer -
 wall-clock, not block-clock - and a run that starts the instant the plug-in is
 up would race the staging (the same phenomenon the CI WAV compare met). With
 the settle wait, the model is in place before the first block, the two passes
 run on the same instance so the DSP carries the same state on every surface,
 and the bytes can be compared at all.

 Usage
   volum-wrapper-check [module-lv2] [module-clap] [--params N]

 The VST3 module is found the way the wrappers find it (VOLUM_VST3_MODULE and
 the standard folders); the two wrapper modules come from the command line,
 from VOLUM_LV2_MODULE / VOLUM_CLAP_MODULE, or from the build tree layout.

 Exit codes: 0 ok, 2 a surface did not load, 3 the outputs disagree.

 ==============================================================================
*/

#include "wrapper-support.h"

#include "../../tests/third_party/sha256.h"

#include "clap/clap.h"
#include "lv2/atom/atom.h"
#include "lv2/core/lv2.h"
#include "lv2/midi/midi.h"
#include "lv2/urid/urid.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace volumlinux;

constexpr unsigned int kRate = 48000;
constexpr unsigned int kFrames = 24000; // half a second
constexpr unsigned int kMaxBlockFrames = 512;
constexpr unsigned int kDefaultParamCount = 233;

// The parameter the plumbing check moves: kOutputLevel (VST3 index 6, default
// 0.8), applied in the plug-in path by _ProcessOutput - so it changes the audio
// even where the tone stack would not, a CI bundle built without rigs included.
constexpr unsigned int kCheckParamIndex = 6;
constexpr double kCheckParamValue = 0.25;

// How long a surface waits, fully open, before its first block: the idle timer
// ticks every 20 ms and the model it stages must be in place, not landing in
// the middle of a measured pass.
constexpr int kSettleMs = 1500;

/** Half a second of stereo tone: 220 Hz left, 330 Hz right, both at 0.4 - loud
 *  enough that a gain change shows, plain enough that two engines agree to the
 *  last bit. */
std::vector<float> makeTone()
{
  std::vector<float> tone(size_t(2) * kFrames);

  for (unsigned int i = 0; i < kFrames; ++i)
  {
    const double t = double(i) / double(kRate);
    tone[i] = float(0.4 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * t));
    tone[kFrames + i] = float(0.4 * std::sin(2.0 * 3.14159265358979323846 * 330.0 * t));
  }

  return tone;
}

std::string sha256(const std::vector<float>& stereo)
{
  return volum::test::Sha256Hex(stereo.data(), stereo.size() * sizeof(float));
}

// MARK: the direct engine, the VST3 way

/** Run the tone through the engine already open in `we`, collecting the whole
 *  stereo output. `paramValue` >= 0 queues one parameter change at the very
 *  first block - one, the way a host's automation does it, not one per block. */
std::vector<float> runDirect(WrapperEngine& we, const std::vector<float>& tone, double paramValue)
{
  std::vector<float> out;
  out.reserve(tone.size());

  std::vector<float> inBuf(size_t(2) * we.eng.blockFrames);
  std::vector<float> outBuf(size_t(2) * we.eng.blockFrames);
  float* inPtrs[2] = {inBuf.data(), inBuf.data() + we.eng.blockFrames};
  float* outPtrs[2] = {outBuf.data(), outBuf.data() + we.eng.blockFrames};

  for (unsigned int offset = 0; offset < kFrames; offset += we.eng.blockFrames)
  {
    const unsigned int frames = std::min(we.eng.blockFrames, kFrames - offset);
    // The tone is planar (L then R), so each channel is copied from its own plane.
    std::memcpy(inBuf.data(), tone.data() + size_t(offset), frames * sizeof(float));
    std::memcpy(inBuf.data() + we.eng.blockFrames, tone.data() + kFrames + size_t(offset), frames * sizeof(float));
    we.connectAudio(inPtrs, outPtrs, 0);

    if (paramValue >= 0.0 && offset == 0)
      we.queueParamChange(we.params[kCheckParamIndex].id, paramValue, 0);

    we.process(frames);
    // Collect planar (L then R) so every surface's WAV has the same layout.
    out.insert(out.end(), outBuf.begin(), outBuf.begin() + frames);
    out.insert(out.end(), outBuf.begin() + we.eng.blockFrames,
               outBuf.begin() + we.eng.blockFrames + frames);
  }

  return out;
}

// MARK: the LV2 surface

/** The three URIDs the wrapper asks the host's map for, answered from a fixed
 *  table - a real host numbers them its own way, the wrapper only compares. */
LV2_URID lv2MapUrid(void*, const char* uri)
{
  if (std::strcmp(uri, LV2_ATOM__Sequence) == 0)
    return 1;

  if (std::strcmp(uri, LV2_ATOM__Chunk) == 0)
    return 2;

  if (std::strcmp(uri, LV2_MIDI__MidiEvent) == 0)
    return 3;

  return 0;
}

struct Lv2Api
{
  void* lib = nullptr;
  const LV2_Descriptor* desc = nullptr;
};

/** dlopen wants a path with a slash to mean "this file", not "search the
 *  library path": a module named from the build tree needs the ./ . */
std::string dlopenPath(const std::string& path)
{
  if (path.find('/') == std::string::npos)
    return "./" + path;

  return path;
}

/** dlopen the wrapper and ask for index 0: the load check a host does first. */
bool loadLv2(const std::string& path, Lv2Api& api, std::string& err)
{
  api.lib = dlopen(dlopenPath(path).c_str(), RTLD_NOW | RTLD_LOCAL);

  if (!api.lib)
  {
    // dlerror() consumes the message: one call, or the second returns null.
    const char* reason = dlerror();
    err = std::string("dlopen(") + path + ") : " + (reason ? reason : "?");
    return false;
  }

  using DescriptorFn = const LV2_Descriptor* (*)(uint32_t);
  auto descriptor = reinterpret_cast<DescriptorFn>(dlsym(api.lib, "lv2_descriptor"));

  if (!descriptor)
  {
    err = "lv2_descriptor introuvable dans " + path;
    return false;
  }

  api.desc = descriptor(0);

  if (!api.desc)
  {
    err = "lv2_descriptor(0) a renvoie null";
    return false;
  }

  return true;
}

/** One pass through an already-instantiated LV2 handle: feed the tone, collect
 *  the whole stereo output. The control ports hold the parameters' defaults,
 *  except the check parameter when `paramValue` >= 0 - which the wrapper queues
 *  the first time it sees the port move, the first block of this pass. */
std::vector<float> runLv2Pass(const Lv2Api& api, LV2_Handle handle, const std::vector<float>& tone,
                              const std::vector<WrapperParam>& params, double paramValue)
{
  std::vector<float> out;
  out.reserve(tone.size());

  std::vector<float> inBuf(size_t(2) * kMaxBlockFrames);
  std::vector<float> outBuf(size_t(2) * kMaxBlockFrames, 0.f);
  float* inPtrs[2] = {inBuf.data(), inBuf.data() + kMaxBlockFrames};
  float* outPtrs[2] = {outBuf.data(), outBuf.data() + kMaxBlockFrames};
  std::vector<float> controlPorts(params.size(), 0.f);

  for (size_t p = 0; p < params.size(); ++p)
    controlPorts[p] = float(params[p].defaultValue);

  if (paramValue >= 0.0)
    controlPorts[kCheckParamIndex] = float(paramValue);

  // An empty sequence: no MIDI this pass, the port is only wired.
  LV2_Atom_Sequence midiSeq {};
  midiSeq.atom.size = 0;
  midiSeq.atom.type = 1; // the map's answer for atom:Sequence

  for (unsigned int offset = 0; offset < kFrames; offset += kMaxBlockFrames)
  {
    const unsigned int frames = std::min(kMaxBlockFrames, kFrames - offset);
    // A real LV2 host hands the plugin fresh buffers each run() and the plugin
    // writes [0, frames) of them - so the tone is copied in, the ports are
    // re-wired, and the output collected per block, like runDirect does.
    std::memcpy(inBuf.data(), tone.data() + size_t(offset), frames * sizeof(float));
    std::memcpy(inBuf.data() + kMaxBlockFrames, tone.data() + kFrames + size_t(offset), frames * sizeof(float));

    api.desc->connect_port(handle, 0, inPtrs[0]);
    api.desc->connect_port(handle, 1, inPtrs[1]);
    api.desc->connect_port(handle, 2, outPtrs[0]);
    api.desc->connect_port(handle, 3, outPtrs[1]);

    // A control port is wired to the float itself, not to a pointer to it: the
    // wrapper reads *port, and a float** here would hand it the pointer's bytes.
    for (size_t p = 0; p < params.size(); ++p)
      api.desc->connect_port(handle, unsigned(4 + p), &controlPorts[p]);

    api.desc->connect_port(handle, unsigned(4 + params.size()), &midiSeq);

    api.desc->run(handle, frames);

    out.insert(out.end(), outBuf.begin(), outBuf.begin() + frames);
    out.insert(out.end(), outBuf.begin() + kMaxBlockFrames, outBuf.begin() + kMaxBlockFrames + frames);
  }

  return out;
}

// MARK: the CLAP surface

struct ClapApi
{
  void* lib = nullptr;
  const clap_plugin_entry_t* entry = nullptr;
  const clap_plugin_factory* factory = nullptr;
};

bool loadClap(const std::string& path, ClapApi& api, std::string& err)
{
  api.lib = dlopen(dlopenPath(path).c_str(), RTLD_NOW | RTLD_LOCAL);

  if (!api.lib)
  {
    const char* reason = dlerror();
    err = std::string("dlopen(") + path + ") : " + (reason ? reason : "?");
    return false;
  }

  auto entry = static_cast<const clap_plugin_entry_t*>(dlsym(api.lib, "clap_entry"));

  if (!entry)
  {
    err = "clap_entry introuvable dans " + path;
    return false;
  }

  if (!clap_version_is_compatible(entry->clap_version))
  {
    err = "clap_entry annonce une version incompatible";
    return false;
  }

  if (!entry->init(dlopenPath(path).c_str()))
  {
    err = "clap_entry.init a renvoie false";
    return false;
  }

  api.entry = entry;
  api.factory = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));

  if (!api.factory)
  {
    err = "clap.plugin-factory introuvable";
    return false;
  }

  return true;
}

/** The host the plugin sees. The wrapper never calls back into it, but the
 *  struct is part of the contract, so it is filled all the same. */
const void* clapNoHostExtension(const clap_host*, const char*) { return nullptr; }
void clapNoHostRequest(const clap_host*) {}

const clap_host_t gHost = {CLAP_VERSION_INIT, nullptr, "volum-wrapper-check", "VoLum", kPluginUri,
                           kPluginVersion,    clapNoHostExtension, clapNoHostRequest,
                           clapNoHostRequest, clapNoHostRequest};

struct ClapEventList
{
  std::vector<const clap_event_header_t*> events;
  clap_input_events_t api = {};
};

uint32_t clapEventSize(const clap_input_events* list)
{
  return uint32_t(static_cast<ClapEventList*>(list->ctx)->events.size());
}

const clap_event_header_t* clapEventGet(const clap_input_events* list, uint32_t index)
{
  return static_cast<ClapEventList*>(list->ctx)->events[index];
}

/** One pass through an already-activated CLAP plugin: feed the tone, collect
 *  the whole stereo output. The parameter change, when there is one, is one
 *  PARAM_VALUE event at the very first block. */
std::vector<float> runClapPass(const clap_plugin_t* plugin, const std::vector<float>& tone,
                               const std::vector<WrapperParam>& params, double paramValue, std::string& err)
{
  std::vector<float> out;
  out.reserve(tone.size());

  std::vector<float> inBuf(size_t(2) * kMaxBlockFrames);
  std::vector<float> outBuf(size_t(2) * kMaxBlockFrames, 0.f);
  float* inPtrs[2] = {inBuf.data(), inBuf.data() + kMaxBlockFrames};
  float* outPtrs[2] = {outBuf.data(), outBuf.data() + kMaxBlockFrames};

  clap_audio_buffer_t audioIn = {};
  audioIn.data32 = inPtrs;
  audioIn.channel_count = 2;

  clap_audio_buffer_t audioOut = {};
  audioOut.data32 = outPtrs;
  audioOut.channel_count = 2;

  ClapEventList eventList;
  clap_event_param_value_t paramEvent;

  if (paramValue >= 0.0)
  {
    std::memset(&paramEvent, 0, sizeof(paramEvent));
    paramEvent.header.size = sizeof(clap_event_param_value_t);
    paramEvent.header.time = 0;
    paramEvent.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    paramEvent.header.type = CLAP_EVENT_PARAM_VALUE;
    paramEvent.header.flags = 0;
    paramEvent.param_id = clap_id(params[kCheckParamIndex].id);
    paramEvent.value = paramValue;
    eventList.events.push_back(&paramEvent.header);
  }

  eventList.api.ctx = &eventList;
  eventList.api.size = clapEventSize;
  eventList.api.get = clapEventGet;

  clap_process_t process = {};
  process.transport = nullptr;
  process.audio_inputs = &audioIn;
  process.audio_outputs = &audioOut;
  process.audio_inputs_count = 1;
  process.audio_outputs_count = 1;
  process.in_events = &eventList.api;

  for (unsigned int offset = 0; offset < kFrames; offset += kMaxBlockFrames)
  {
    const unsigned int frames = std::min(kMaxBlockFrames, kFrames - offset);
    // Like LV2: the plugin writes [0, frames) of the buffers the host points it
    // at, so the tone is copied in and the output collected per block.
    std::memcpy(inBuf.data(), tone.data() + size_t(offset), frames * sizeof(float));
    std::memcpy(inBuf.data() + kMaxBlockFrames, tone.data() + kFrames + size_t(offset), frames * sizeof(float));
    process.frames_count = frames;
    process.steady_time = offset;

    if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR)
    {
      err = "plugin->process a renvoie CLAP_PROCESS_ERROR";
      return {};
    }

    out.insert(out.end(), outBuf.begin(), outBuf.begin() + frames);
    out.insert(out.end(), outBuf.begin() + kMaxBlockFrames, outBuf.begin() + kMaxBlockFrames + frames);
  }

  return out;
}

} // namespace

int main(int argc, char** argv)
{
  std::string lv2Path, clapPath;
  unsigned int expectedParams = kDefaultParamCount;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];

    if (arg == "--params" && i + 1 < argc)
      expectedParams = unsigned(std::atoi(argv[++i]));
    else if (lv2Path.empty())
      lv2Path = arg;
    else if (clapPath.empty())
      clapPath = arg;
    else
    {
      fprintf(stderr, "argument en trop : %s\n", argv[i]);
      return 2;
    }
  }

  if (const char* env = std::getenv("VOLUM_LV2_MODULE"); env && env[0] && lv2Path.empty())
    lv2Path = env;

  if (const char* env = std::getenv("VOLUM_CLAP_MODULE"); env && env[0] && clapPath.empty())
    clapPath = env;

  std::string tried;
  const std::string vst3Module = locateVst3Module(tried);

  if (vst3Module.empty())
  {
    fprintf(stderr, "volum-wrapper-check : module VST3 introuvable (cherche :%s)\n", tried.c_str());
    return 2;
  }

  printf("=== volum-wrapper-check : %s ===\n", vst3Module.c_str());

  // The wrappers locate the module themselves; in the build tree the .lv2 and
  // .clap sit where the search does not reach, so the check shares its answer
  // with them the way a CI script would with VOLUM_VST3_MODULE.
  setenv("VOLUM_VST3_MODULE", vst3Module.c_str(), 1);

  const std::vector<float> tone = makeTone();

  // MARK: the direct engine
  std::string err;
  WrapperEngine direct;
  int status = direct.open(double(kRate), kMaxBlockFrames, err);

  if (status != kLoadOk)
  {
    fprintf(stderr, "volum-wrapper-check : moteur direct - %s\n", err.c_str());
    return 2;
  }

  if (direct.params.size() != expectedParams)
  {
    fprintf(stderr, "volum-wrapper-check : %zu parametres, %u attendus\n", direct.params.size(), expectedParams);
    return 3;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(kSettleMs));

  const std::vector<float> directA = runDirect(direct, tone, -1.0);
  const std::vector<float> directB = runDirect(direct, tone, kCheckParamValue);
  const std::string directHashA = sha256(directA);
  const std::string directHashB = sha256(directB);

  direct.close();

  printf("moteur direct  : %u parametres, defaut %.16s, %s=%.2f %.16s\n", expectedParams,
         directHashA.c_str(), direct.params[kCheckParamIndex].name.c_str(), kCheckParamValue,
         directHashB.c_str());

  // Without a rig the plug-in's fallback is silence, so no parameter can be
  // audible: the equality check still means something, the effect check cannot
  // run. A CI bundle is built without the rigs, a developer's is not.
  const bool directAudible = std::any_of(directA.begin(), directA.end(), [](float v) { return v != 0.f; });

  if (directHashA == directHashB && directAudible)
  {
    fprintf(stderr, "volum-wrapper-check : le changement de parametre n'a rien change sur le moteur direct\n");
    return 3;
  }

  if (!directAudible)
    printf("pas de modele charge : sortie muette, le changement de parametre n'est pas verifie\n");

  // MARK: LV2
  Lv2Api lv2;

  if (!loadLv2(lv2Path, lv2, err))
  {
    fprintf(stderr, "volum-wrapper-check : LV2 - %s\n", err.c_str());
    return 2;
  }

  LV2_URID_Map lv2Map = {nullptr, lv2MapUrid};
  LV2_Feature lv2MapFeature = {LV2_URID__map, &lv2Map};
  LV2_Feature* lv2Features[] = {&lv2MapFeature, nullptr};

  LV2_Handle lv2Handle = lv2.desc->instantiate(lv2.desc, double(kRate), "/tmp", lv2Features);

  if (!lv2Handle)
  {
    fprintf(stderr, "volum-wrapper-check : LV2 instantiate a renvoie null\n");
    return 2;
  }

  lv2.desc->activate(lv2Handle);
  std::this_thread::sleep_for(std::chrono::milliseconds(kSettleMs));

  const std::vector<float> lv2A = runLv2Pass(lv2, lv2Handle, tone, direct.params, -1.0);
  const std::vector<float> lv2B = runLv2Pass(lv2, lv2Handle, tone, direct.params, kCheckParamValue);

  lv2.desc->deactivate(lv2Handle);
  lv2.desc->cleanup(lv2Handle);
  dlclose(lv2.lib);

  if (lv2A.empty() || lv2B.empty())
  {
    fprintf(stderr, "volum-wrapper-check : LV2 n'a rien rendu\n");
    return 2;
  }

  const std::string lv2HashA = sha256(lv2A);
  const std::string lv2HashB = sha256(lv2B);
  printf("LV2            : defaut %.16s, %s=%.2f %.16s\n", lv2HashA.c_str(),
         direct.params[kCheckParamIndex].name.c_str(), kCheckParamValue, lv2HashB.c_str());

  // MARK: CLAP
  ClapApi clap;

  if (!loadClap(clapPath, clap, err))
  {
    fprintf(stderr, "volum-wrapper-check : CLAP - %s\n", err.c_str());
    return 2;
  }

  const clap_plugin_t* clapPlugin = clap.factory->create_plugin(clap.factory, &gHost, kPluginUri);

  if (!clapPlugin || !clapPlugin->init(clapPlugin))
  {
    fprintf(stderr, "volum-wrapper-check : CLAP create_plugin/init a echoue\n");
    return 2;
  }

  if (!clapPlugin->activate(clapPlugin, double(kRate), 64, kMaxBlockFrames)
      || !clapPlugin->start_processing(clapPlugin))
  {
    fprintf(stderr, "volum-wrapper-check : CLAP activate/start_processing a echoue\n");
    return 2;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(kSettleMs));

  const std::vector<float> clapA = runClapPass(clapPlugin, tone, direct.params, -1.0, err);
  const std::vector<float> clapB = runClapPass(clapPlugin, tone, direct.params, kCheckParamValue, err);

  clapPlugin->stop_processing(clapPlugin);
  clapPlugin->deactivate(clapPlugin);
  clapPlugin->destroy(clapPlugin);
  clap.entry->deinit();
  dlclose(clap.lib);

  if (clapA.empty() || clapB.empty())
  {
    fprintf(stderr, "volum-wrapper-check : CLAP - %s\n", err.c_str());
    return 2;
  }

  const std::string clapHashA = sha256(clapA);
  const std::string clapHashB = sha256(clapB);
  printf("CLAP           : defaut %.16s, %s=%.2f %.16s\n", clapHashA.c_str(),
         direct.params[kCheckParamIndex].name.c_str(), kCheckParamValue, clapHashB.c_str());

  // MARK: the verdict
  if (getenv("VOLUM_CHECK_DEBUG"))
  {
    auto dump = [](const char* tag, const std::vector<float>& a, const std::vector<float>& b) {
      size_t diff = a.size();

      for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0)
        {
          diff = i;
          break;
        }

      printf("[dbg] %s : premier echantillon different = %zu (%.9f vs %.9f)\n", tag, diff,
             diff < a.size() ? a[diff] : 0.f, diff < b.size() ? b[diff] : 0.f);

      size_t firstNonZero = a.size();

      for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != 0.f)
        {
          firstNonZero = i;
          break;
        }

      printf("[dbg] %s : A premier non-nul = %zu, B premier non-nul = %zu\n", tag, firstNonZero,
             [&] {
               for (size_t i = 0; i < b.size(); ++i)
                 if (b[i] != 0.f)
                   return i;
               return b.size();
             }());
      printf("[dbg] %s A :", tag);

      for (size_t i = 0; i < 8 && i < a.size(); ++i)
        printf(" %.9f", a[i]);

      printf("\n[dbg] %s B :", tag);

      for (size_t i = 0; i < 8 && i < b.size(); ++i)
        printf(" %.9f", b[i]);

      printf("\n");
    };

    auto profil = [](const char* tag, const std::vector<float>& a) {
      printf("[dbg] profil %s :", tag);

      for (unsigned int b = 0; b < 60 && (size_t(b) * 512) < a.size(); ++b)
      {
        float peak = 0.f;

        for (unsigned int i = 0; i < 512; ++i)
          peak = std::max(peak, std::abs(a[size_t(b) * 512 + i]));

        printf(" %u:%.4f", b, peak);
      }

      printf("\n");
    };

    profil("directA", directA);
    profil("lv2A", lv2A);
    profil("clapA", clapA);
    profil("directB", directB);
    profil("lv2B", lv2B);
    dump("direct/lv2", directA, lv2A);
    dump("direct/clap", directA, clapA);
    dump("lv2/clap", lv2A, clapA);
    printf("[dbg] tailles : direct=%zu lv2=%zu clap=%zu\n", directA.size(), lv2A.size(), clapA.size());
  }

  bool ok = true;

  if (lv2HashA != directHashA || lv2HashB != directHashB)
  {
    fprintf(stderr, "volum-wrapper-check : le WAV LV2 differe du WAV VST3\n");
    ok = false;
  }

  if (clapHashA != directHashA || clapHashB != directHashB)
  {
    fprintf(stderr, "volum-wrapper-check : le WAV CLAP differe du WAV VST3\n");
    ok = false;
  }

  if (directAudible && (lv2HashA == lv2HashB || clapHashA == clapHashB))
  {
    fprintf(stderr, "volum-wrapper-check : le changement de parametre n'a rien change sur un wrapper\n");
    ok = false;
  }

  if (!ok)
    return 3;

  printf("volum-wrapper-check : les trois surfaces rendent le meme WAV (defaut %s, %s=%.2f %s)\n",
         directHashA.c_str(), direct.params[kCheckParamIndex].name.c_str(), kCheckParamValue,
         directHashB.c_str());
  return 0;
}
