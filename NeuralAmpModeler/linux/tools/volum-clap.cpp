/*
 ==============================================================================

 The CLAP surface (issue #9). The same engine as the LV2 wrapper, behind the
 format's C API: the entry point, a one-plug-in factory, and the process loop
 that walks the host's frames in block-sized chunks - two audio in, two audio
 out, the events the host hands over decoded to VST3 events and parameter
 changes, nothing more.

 The parameter ids a host sees are the VST3 ids themselves (the ones the
 block's inputParameterChanges carry), so CLAP_EVENT_PARAM_VALUE needs no
 translation: the id goes straight into queueParamChange. Normalized 0..1
 everywhere, like the LV2 control ports.

 Not here: the GUI (CLAP has no GUI on Linux in iPlug2's tree, issue #12's
 platform question) and state save/restore.

 ==============================================================================
*/

#include "wrapper-support.h"

#include "clap/clap.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace volumlinux
{
void initClapSurfaces();
}

namespace
{

using namespace volumlinux;

/** The block the engine is set up for, the same chunking rule as the LV2 run(). */
constexpr unsigned int kMaxBlockFrames = 512;

struct ClapInstance
{
  WrapperEngine engine;
};

ClapInstance* selfOf(const clap_plugin_t* plugin)
{
  return plugin && plugin->plugin_data ? static_cast<ClapInstance*>(plugin->plugin_data) : nullptr;
}

// MARK: the plug-in

bool clapInit(const clap_plugin* plugin)
{
  auto* self = selfOf(plugin);

  if (!self)
    return false;

  std::string err;
  const int status = self->engine.open(48000.0, kMaxBlockFrames, err);

  if (status != kLoadOk)
  {
    fprintf(stderr, "VoLum.clap : %s\n", err.c_str());
    return false;
  }

  return true;
}

void clapDestroy(const clap_plugin* plugin)
{
  auto* self = selfOf(plugin);

  if (!self)
    return;

  self->engine.close();
  delete self;
}

bool clapActivate(const clap_plugin* plugin, double rate, uint32_t minFrames, uint32_t maxFrames)
{
  auto* self = selfOf(plugin);

  if (!self || maxFrames == 0)
    return false;

  (void)minFrames;

  const unsigned int frames = std::min(uint32_t(kMaxBlockFrames), maxFrames);

  if (!startProcessing(self->engine.loaded, rate, int(frames), true))
    return false;

  // The engine's buffers are sized for the new block; inputParameterChanges
  // stays wired to self->engine.paramChanges across the re-setup.
  self->engine.eng.setup(self->engine.loaded, frames);
  return true;
}

void clapDeactivate(const clap_plugin* plugin)
{
  auto* self = selfOf(plugin);

  if (!self || !self->engine.loaded.processor)
    return;

  self->engine.loaded.processor->setProcessing(0);
  self->engine.loaded.component->setActive(0);
}

bool clapStartProcessing(const clap_plugin*) { return true; }

void clapStopProcessing(const clap_plugin*) {}

void clapReset(const clap_plugin*) {}

/** Walk the host's event list once per chunk, keeping what falls inside it: a
 *  parameter value goes into the block's parameter changes, a note or a MIDI
 *  message into the block's events - the same two paths the VST3 host uses. */
void clapQueueChunkEvents(ClapInstance& self, const clap_input_events_t* events, unsigned int offset,
                          unsigned int frames)
{
  WrapperEngine& we = self.engine;
  const uint32_t count = events->size(events);

  for (uint32_t i = 0; i < count; ++i)
  {
    const auto* header = events->get(events, i);

    if (header->time < offset || header->time >= offset + frames)
      continue;

    const int eventOffset = int(header->time - offset);

    switch (header->type)
    {
      case CLAP_EVENT_PARAM_VALUE:
      {
        const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(header);
        we.queueParamChange(vst::ParamID(ev->param_id), ev->value, eventOffset);
        break;
      }
      case CLAP_EVENT_NOTE_ON:
      {
        const auto* ev = reinterpret_cast<const clap_event_note_t*>(header);
        const uint8_t channel = ev->channel >= 0 && ev->channel <= 15 ? uint8_t(ev->channel) : 0;
        const uint8_t bytes[3] = {uint8_t(0x90 | channel), uint8_t(ev->key & 0x7f),
                                  uint8_t(std::min(1.0, std::max(0.0, ev->velocity)) * 127.0)};
        we.eng.queueMidi(bytes, 3, eventOffset);
        break;
      }
      case CLAP_EVENT_NOTE_OFF:
      {
        const auto* ev = reinterpret_cast<const clap_event_note_t*>(header);
        const uint8_t channel = ev->channel >= 0 && ev->channel <= 15 ? uint8_t(ev->channel) : 0;
        const uint8_t bytes[3] = {uint8_t(0x80 | channel), uint8_t(ev->key & 0x7f), 0};
        we.eng.queueMidi(bytes, 3, eventOffset);
        break;
      }
      case CLAP_EVENT_MIDI:
      {
        const auto* ev = reinterpret_cast<const clap_event_midi_t*>(header);
        we.eng.queueMidi(ev->data, 3, eventOffset);
        break;
      }
      default:
        break;
    }
  }
}

clap_process_status clapProcess(const clap_plugin* plugin, const clap_process_t* process)
{
  auto* self = selfOf(plugin);

  if (!self || !self->engine.loaded.processor)
    return CLAP_PROCESS_ERROR;

  if (!process->audio_inputs || !process->audio_outputs || process->audio_inputs_count < 1
      || process->audio_outputs_count < 1 || !process->audio_inputs[0].data32
      || !process->audio_outputs[0].data32)
    return CLAP_PROCESS_ERROR;

  WrapperEngine& we = self->engine;
  const unsigned int blockFrames = we.eng.blockFrames;

  for (unsigned int offset = 0; offset < process->frames_count; offset += blockFrames)
  {
    const unsigned int frames = std::min(blockFrames, process->frames_count - offset);
    we.connectAudio(process->audio_inputs[0].data32, process->audio_outputs[0].data32, offset);
    clapQueueChunkEvents(*self, process->in_events, offset, frames);
    we.process(frames);
  }

  return CLAP_PROCESS_CONTINUE;
}

// MARK: the extensions a host asks for

clap_plugin_params_t gParamsExt = {};
clap_plugin_audio_ports_t gAudioPortsExt = {};

uint32_t clapParamCount(const clap_plugin* plugin)
{
  auto* self = selfOf(plugin);
  return self ? uint32_t(self->engine.params.size()) : 0;
}

bool clapParamGetInfo(const clap_plugin* plugin, uint32_t index, clap_param_info_t* info)
{
  auto* self = selfOf(plugin);

  if (!self || index >= self->engine.params.size() || !info)
    return false;

  const WrapperParam& p = self->engine.params[index];
  std::memset(info, 0, sizeof(*info));
  info->id = clap_id(p.id);
  info->flags = 0;
  info->cookie = nullptr;
  std::strncpy(info->name, p.name.c_str(), CLAP_NAME_SIZE - 1);
  info->min_value = 0.0;
  info->max_value = 1.0;
  info->default_value = p.defaultValue;
  return true;
}

bool clapParamGetValue(const clap_plugin* plugin, clap_id paramId, double* outValue)
{
  auto* self = selfOf(plugin);

  if (!self || !self->engine.loaded.controller || !outValue)
    return false;

  *outValue = self->engine.loaded.controller->getParamNormalized(vst::ParamID(paramId));
  return true;
}

bool clapParamValueToText(const clap_plugin*, clap_id, double value, char* buffer, uint32_t capacity)
{
  if (!buffer || capacity == 0)
    return false;

  std::snprintf(buffer, capacity, "%.3f", value);
  return true;
}

bool clapParamTextToValue(const clap_plugin*, clap_id, const char* text, double* outValue)
{
  if (!text || !outValue)
    return false;

  *outValue = std::strtod(text, nullptr);
  return true;
}

/** A parameter change that arrives while the plug-in is not processing: the
 *  block's parameter changes are handed over with a zero-frame process, the
 *  VST3 way of flushing. */
void clapParamFlush(const clap_plugin* plugin, const clap_input_events_t* in, const clap_output_events_t*)
{
  auto* self = selfOf(plugin);

  if (!self || !self->engine.loaded.processor)
    return;

  const uint32_t count = in->size(in);

  for (uint32_t i = 0; i < count; ++i)
  {
    const auto* header = in->get(in, i);

    if (header->type != CLAP_EVENT_PARAM_VALUE)
      continue;

    const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(header);
    self->engine.queueParamChange(vst::ParamID(ev->param_id), ev->value, int(header->time));
  }

  self->engine.process(0);
}

uint32_t clapAudioPortCount(const clap_plugin*, bool isInput)
{
  (void)isInput;
  return 1;
}

bool clapAudioPortGet(const clap_plugin*, uint32_t index, bool isInput, clap_audio_port_info_t* info)
{
  if (index != 0 || !info)
    return false;

  std::memset(info, 0, sizeof(*info));
  info->id = isInput ? 0 : 1;
  std::strncpy(info->name, isInput ? "Audio In" : "Audio Out", CLAP_NAME_SIZE - 1);
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}

const void* clapGetExtension(const clap_plugin*, const char* id)
{
  if (!id)
    return nullptr;

  if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
    return &gParamsExt;

  if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
    return &gAudioPortsExt;

  return nullptr;
}

clap_plugin_t gPluginTemplate = {};

constexpr const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

clap_plugin_descriptor_t gDescriptor = {};

clap_plugin_factory_t gFactory = {};

const clap_plugin_t* clapCreatePlugin(const clap_plugin_factory*, const clap_host_t* host, const char* pluginId)
{
  if (pluginId && std::strcmp(pluginId, kPluginUri) != 0)
    return nullptr;

  auto* self = new (std::nothrow) ClapInstance();

  if (!self)
    return nullptr;

  auto* plugin = new (std::nothrow) clap_plugin_t(gPluginTemplate);

  if (!plugin)
  {
    delete self;
    return nullptr;
  }

  plugin->plugin_data = self;
  (void)host;
  return plugin;
}

uint32_t clapPluginCount(const clap_plugin_factory*) { return 1; }

const clap_plugin_descriptor_t* clapPluginDescriptor(const clap_plugin_factory*, uint32_t index)
{
  return index == 0 ? &gDescriptor : nullptr;
}

} // namespace

extern "C" {

// MARK: the entry point

const void* clapGetFactory(const char* factoryId)
{
  if (!factoryId)
    return nullptr;

  // A host may scan without calling init first: the descriptor is filled on
  // the first factory request either way.
  volumlinux::initClapSurfaces();

  if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
    return &gFactory;

  return nullptr;
}

bool clapEntryInit(const char*) { return true; }

void clapEntryDeinit() {}

// The symbol a host dlsym()s: the struct itself, not a pointer to it. extern
// because a const global would otherwise have internal linkage in C++, and a
// hidden symbol is one no host finds.
extern CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
  CLAP_VERSION_INIT, clapEntryInit, clapEntryDeinit, clapGetFactory,
};

} // extern "C"

namespace volumlinux
{

/** Filled once, on the first call: the descriptors are static, the parameter
 *  surface belongs to an instance. */
void initClapSurfaces()
{
  if (gDescriptor.id)
    return;

  gDescriptor.clap_version = CLAP_VERSION_INIT;
  gDescriptor.id = kPluginUri;
  gDescriptor.name = kPluginName;
  gDescriptor.vendor = "Lum";
  gDescriptor.url = kPluginUri;
  gDescriptor.version = kPluginVersion;
  gDescriptor.features = kFeatures;

  gPluginTemplate.desc = &gDescriptor;
  gPluginTemplate.init = clapInit;
  gPluginTemplate.destroy = clapDestroy;
  gPluginTemplate.activate = clapActivate;
  gPluginTemplate.deactivate = clapDeactivate;
  gPluginTemplate.start_processing = clapStartProcessing;
  gPluginTemplate.stop_processing = clapStopProcessing;
  gPluginTemplate.reset = clapReset;
  gPluginTemplate.process = clapProcess;
  gPluginTemplate.get_extension = clapGetExtension;
  gPluginTemplate.on_main_thread = [](const clap_plugin*) {};

  gParamsExt.count = clapParamCount;
  gParamsExt.get_info = clapParamGetInfo;
  gParamsExt.get_value = clapParamGetValue;
  gParamsExt.value_to_text = clapParamValueToText;
  gParamsExt.text_to_value = clapParamTextToValue;
  gParamsExt.flush = clapParamFlush;

  gAudioPortsExt.count = clapAudioPortCount;
  gAudioPortsExt.get = clapAudioPortGet;

  gFactory.get_plugin_count = clapPluginCount;
  gFactory.get_plugin_descriptor = clapPluginDescriptor;
  gFactory.create_plugin = clapCreatePlugin;
}

} // namespace volumlinux
