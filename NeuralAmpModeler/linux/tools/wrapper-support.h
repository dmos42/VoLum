/*
 ==============================================================================

 What an LV2 or CLAP binary of VoLum is made of, once, instead of once per
 format: where the VST3 module lives, and the parameter surface both formats
 expose. A wrapper instance owns an Engine (engine-support.h) - the same block
 loop volum-live and volum-app drive - so a host that loads the wrapper gets
 the plug-in this port already tested, quirks included: the 20 ms idle timer
 that stages the amp model, the MIDI decode, the recall CC.

 There is deliberately no second plug-in implementation here. The wrapper is a
 format surface, not a port of the DSP: what it adds is port plumbing and the
 one thing a format asks for that the hosts never did - feeding parameter
 changes the way a DAW does, through the block's inputParameterChanges.

 ==============================================================================
*/

#pragma once

#include "engine-support.h"
#include "host-support.h"

#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include <cctype>
#include <dlfcn.h>
#include <filesystem>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace volumlinux
{

// MARK: the names every surface agrees on

/** The plug-in's identity outside the VST3 world: the LV2 URI, the CLAP id and
 *  the bundle names all derive from it. */
constexpr const char* kPluginName = "VoLum";
constexpr const char* kPluginUri = "https://github.com/guitarlum/VoLum";
constexpr const char* kPluginVersion = "1.3.1";

// MARK: finding the module the wrapper drives

/** The VST3 module a wrapper loads, looked for in the order a user arranges a
 *  machine: an explicit override, the standard VST3 folders, then beside the
 *  wrapper itself - which is how the AppImage pairs the two without any
 *  environment. Returns an empty string with the places tried in `tried` when
 *  nothing is there. */
inline std::string locateVst3Module(std::string& tried)
{
  namespace fs = std::filesystem;

  const char* moduleName = "VoLum.vst3.so";
  auto moduleIn = [](const fs::path& bundle) {
    return bundle / "Contents" / "x86_64-linux" / "VoLum.vst3.so";
  };

  tried.clear();

  if (const char* env = std::getenv("VOLUM_VST3_MODULE"); env && env[0])
    return env;

  std::vector<fs::path> bundles;

  if (const char* env = std::getenv("VOLUM_VST3_BUNDLE"); env && env[0])
    bundles.push_back(env);

  if (const char* home = std::getenv("HOME"); home && home[0])
    bundles.push_back(fs::path(home) / ".vst3" / kPluginName);

  bundles.push_back(fs::path("/usr/local/lib/vst3") / kPluginName);
  bundles.push_back(fs::path("/usr/lib/vst3") / kPluginName);

  // Beside this library: the AppImage installs VoLum.lv2 and VoLum.vst3 under
  // the same tree, and a hand-rolled folder keeps the pair together too.
  Dl_info self{};

  if (dladdr(reinterpret_cast<void*>(&locateVst3Module), &self) && self.dli_fname)
  {
    const fs::path here = fs::path(self.dli_fname).parent_path();
    bundles.push_back(here / (std::string(kPluginName) + ".vst3"));
    bundles.push_back(here / "vst3" / kPluginName);
    bundles.push_back(here.parent_path() / "vst3" / kPluginName);
  }

  for (const fs::path& bundle : bundles)
  {
    const fs::path module = moduleIn(bundle);
    tried += " " + module.string();

    std::error_code ec;

    if (fs::exists(module, ec))
      return module.string();
  }

  return {};
}

// MARK: the parameter surface

/** One parameter as every wrapper port exposes it: the VST3 id the block's
 *  parameter changes carry, the normalized default, and the ASCII name and
 *  port symbol both the TTL and the descriptors derive the same way. */
struct WrapperParam
{
  vst::ParamID id = 0;
  double defaultValue = 0.0;
  std::string name;
  std::string symbol;
};

/** An LV2 port symbol: lower-case, [a-z0-9_], never empty, never duplicated -
 *  the TTL generator and the descriptors call this in the same order, so the
 *  port lists cannot drift apart. */
inline std::string wrapperParamSymbol(int index, const std::string& title)
{
  std::string symbol;

  for (const char c : title)
  {
    if (std::isalnum(static_cast<unsigned char>(c)))
      symbol += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    else if (!symbol.empty() && symbol.back() != '_')
      symbol += '_';
  }

  while (!symbol.empty() && symbol.back() == '_')
    symbol.pop_back();

  if (symbol.empty() || symbol[0] < 'a' || symbol[0] > 'z')
    symbol = "p" + symbol;

  return symbol + "_" + std::to_string(index);
}

/** The controller's parameters, in index order: the order the ports are
 *  declared in, for every format at once. */
inline std::vector<WrapperParam> enumerateWrapperParams(Loaded& L)
{
  std::vector<WrapperParam> params;

  if (!L.controller)
    return params;

  const sdk::int32 count = L.controller->getParameterCount();
  params.reserve(size_t(count));

  for (sdk::int32 i = 0; i < count; ++i)
  {
    vst::ParameterInfo info;

    if (L.controller->getParameterInfo(i, info) != sdk::kResultOk)
      continue;

    WrapperParam p;
    p.id = info.id;
    p.defaultValue = info.defaultNormalizedValue;
    p.name = toAscii(info.title);
    p.symbol = wrapperParamSymbol(int(i), p.name);
    params.push_back(std::move(p));
  }

  return params;
}

// MARK: the engine, dressed for a host that owns the clock

/** The block-loop engine plus the two things a format host adds: parameter
 *  changes queued the way a DAW queues them, and audio buffers that point at
 *  the host's own memory instead of a store of ours. */
struct WrapperEngine
{
  Loaded loaded;
  Engine eng;
  vst::ParameterChanges paramChanges;
  std::vector<WrapperParam> params;

  /** Load the module, bring the plug-in up at `rate` and `maxFrames`, and take
   *  the parameter surface. Returns a LoadStatus (kLoadOk on success). */
  int open(double rate, unsigned int maxFrames, std::string& err)
  {
    std::string tried;
    const std::string module = locateVst3Module(tried);

    if (module.empty())
    {
      err = "module VST3 introuvable (cherche :" + tried + " ; VOLUM_VST3_MODULE pour forcer)";
      return kLoadCannotOpen;
    }

    LoadOptions opt;
    opt.listClasses = false;
    opt.quiet = true;

    const int status = loadPlugin(module, loaded, opt);

    if (status != kLoadOk)
    {
      err = "chargement du module VST3 impossible (code " + std::to_string(status) + ")";
      return status;
    }

    if (loaded.inChannels != 2 || loaded.outChannels != 2)
    {
      err = "le module annonce " + std::to_string(loaded.inChannels) + " entree(s) / "
            + std::to_string(loaded.outChannels) + " sortie(s), pas le stereo attendu";
      return kLoadNoInstance;
    }

    params = enumerateWrapperParams(loaded);

    if (!startProcessing(loaded, rate, int(maxFrames), true))
    {
      err = "setupProcessing refuse (" + std::to_string(int(rate)) + " Hz, " + std::to_string(maxFrames) + " images)";
      return kLoadNoInstance;
    }

    eng.setup(loaded, maxFrames);
    // Engine::setup only wires inputEvents; the block's parameter changes are
    // this wrapper's addition, and ProcessParameterChanges reads them here.
    eng.data.inputParameterChanges = &paramChanges;
    return kLoadOk;
  }

  void close()
  {
    if (loaded.component)
    {
      loaded.processor->setProcessing(0);
      loaded.component->setActive(0);
    }

    eng.inPtr.clear();
    eng.outPtr.clear();
    unloadPlugin(loaded);
  }

  /** Point the engine's buses at the host's own buffers for one chunk starting
   *  at `offset` - no copy in either direction, which is what makes the
   *  wrapper's audio path the same one the hosts measure. */
  void connectAudio(float* const* in, float* const* out, unsigned int offset)
  {
    for (int c = 0; c < loaded.inChannels; ++c)
      eng.inPtr[size_t(c)] = in[c] + offset;

    for (int c = 0; c < loaded.outChannels; ++c)
      eng.outPtr[size_t(c)] = out[c] + offset;
  }

  /** One parameter change, the way a DAW hands it over: normalized value, an
   *  offset inside the chunk now starting. */
  void queueParamChange(vst::ParamID id, double normalized, int offsetSamples)
  {
    sdk::int32 index = 0;

    if (vst::IParamValueQueue* queue = paramChanges.addParameterData(id, index))
      queue->addPoint(offsetSamples, normalized, index);
  }

  /** Process `frames` frames (never more than the setup's block). The block's
   *  events and parameter changes are handed over exactly once. */
  bool process(unsigned int frames)
  {
    eng.data.numSamples = frames;
    const bool ok = eng.process();
    paramChanges.clearQueue();
    return ok;
  }
};

} // namespace volumlinux
