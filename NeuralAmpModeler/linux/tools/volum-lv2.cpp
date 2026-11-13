/*
 ==============================================================================

 The LV2 surface (issue #9). The format's C API in front of the engine the port
 already shares (wrapper-support.h): two audio in, two audio out, one control
 input port per VST3 parameter - the same 233, in the same index order the TTL
 generator declared them - and one atom port for MIDI in, the way a DAW hands
 notes and control changes over.

 The wrapper owns no DSP and no second plug-in implementation: run() walks the
 host's frames in block-sized chunks, points the engine's buses straight at the
 host's buffers, queues the control ports that moved as parameter changes and
 the atom events as MIDI, and calls the engine's process() - the same call the
 hosts make, so the audio that comes out is the audio the port already tested.

 Not here (deliberately, for now): the editor (an LV2 UI is a second shared
 object with its own X11 embedding, issue #12's platform question), and state
 save/restore (lv2:state, the next step once Carla round-trips the audio).

 ==============================================================================
*/

#include "wrapper-support.h"

#include "lv2/atom/atom.h"
#include "lv2/atom/util.h"
#include "lv2/core/lv2.h"
#include "lv2/midi/midi.h"
#include "lv2/urid/urid.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{

using namespace volumlinux;

/** The block the engine is set up for. A host asking for more gets it in
 *  chunks of this; a host asking for less gets smaller blocks, which the VST3
 *  setup allows (numSamples never goes above maxSamplesPerBlock). */
constexpr unsigned int kMaxBlockFrames = 512;

struct Lv2Instance
{
  WrapperEngine engine;
  unsigned int sampleRate = 48000;

  // Ports, in the index order the TTL declares.
  float* audioIn[2] = {nullptr, nullptr};
  float* audioOut[2] = {nullptr, nullptr};
  std::vector<float*> controlPorts;
  std::vector<float> controlSent; // last value queued per port, so a static port costs nothing
  const LV2_Atom_Sequence* midiIn = nullptr;

  // URIDs, mapped once the host hands over its map.
  LV2_URID atomSequence = 0;
  LV2_URID atomChunk = 0;
  LV2_URID midiEvent = 0;
  bool haveUridMap = false;
};

LV2_URID mapUrid(LV2_URID_Map* map, const char* uri)
{
  return map && map->map ? map->map(map->handle, uri) : 0;
}

LV2_Handle lv2Instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features)
{
  auto* self = new (std::nothrow) Lv2Instance();

  if (!self)
    return nullptr;

  self->sampleRate = rate > 0 ? static_cast<unsigned int>(rate) : 48000;

  std::string err;
  const int status = self->engine.open(double(self->sampleRate), kMaxBlockFrames, err);

  if (status != kLoadOk)
  {
    fprintf(stderr, "VoLum.lv2 : %s\n", err.c_str());
    delete self;
    return nullptr;
  }

  const size_t paramCount = self->engine.params.size();
  self->controlPorts.assign(paramCount, nullptr);
  // Sent-values start at the defaults, not at "never sent": a host that wires
  // its ports at the plug-in's defaults - they all do - must not trigger 233
  // parameter changes on the first block, several of which are MIDI messages.
  self->controlSent.reserve(paramCount);

  for (const WrapperParam& p : self->engine.params)
    self->controlSent.push_back(float(p.defaultValue));

  for (const LV2_Feature* const* f = features; f && *f; ++f)
  {
    if (std::strcmp((*f)->URI, LV2_URID__map) == 0)
    {
      auto* map = static_cast<LV2_URID_Map*>((*f)->data);
      self->atomSequence = mapUrid(map, LV2_ATOM__Sequence);
      self->atomChunk = mapUrid(map, LV2_ATOM__Chunk);
      self->midiEvent = mapUrid(map, LV2_MIDI__MidiEvent);
      self->haveUridMap = self->atomSequence && self->midiEvent;
    }
  }

  return self;
}

void lv2ConnectPort(LV2_Handle handle, uint32_t port, void* data)
{
  auto* self = static_cast<Lv2Instance*>(handle);

  if (!self)
    return;

  if (port == 0)
    self->audioIn[0] = static_cast<float*>(data);
  else if (port == 1)
    self->audioIn[1] = static_cast<float*>(data);
  else if (port == 2)
    self->audioOut[0] = static_cast<float*>(data);
  else if (port == 3)
    self->audioOut[1] = static_cast<float*>(data);
  else if (port < 4 + self->controlPorts.size())
    self->controlPorts[port - 4] = static_cast<float*>(data);
  else
    self->midiIn = static_cast<const LV2_Atom_Sequence*>(data);
}

void lv2Activate(LV2_Handle) {}

void lv2Run(LV2_Handle handle, uint32_t nframes)
{
  auto* self = static_cast<Lv2Instance*>(handle);

  if (!self || !self->engine.loaded.processor)
    return;

  WrapperEngine& we = self->engine;
  const unsigned int blockFrames = we.eng.blockFrames;

  // The atom port is walked once per chunk, so an event lands in the chunk that
  // carries its offset - the same stamping rule the ALSA drain keeps.
  for (unsigned int offset = 0; offset < nframes; offset += blockFrames)
  {
    const unsigned int frames = std::min(blockFrames, nframes - offset);
    we.connectAudio(self->audioIn, self->audioOut, offset);

    for (size_t p = 0; p < we.params.size(); ++p)
    {
      const float value = self->controlPorts[p] ? *self->controlPorts[p] : 0.f;

      if (self->controlSent[p] != value)
      {
        we.queueParamChange(we.params[p].id, value, 0);
        self->controlSent[p] = value;
      }
    }

    if (self->haveUridMap && self->midiIn)
    {
      LV2_ATOM_SEQUENCE_FOREACH(self->midiIn, ev)
      {
        if (ev->time.frames < offset || ev->time.frames >= offset + frames)
          continue;

        if (ev->body.type == self->midiEvent && ev->body.size >= 1 && ev->body.size <= 4)
        {
          const auto* bytes = static_cast<const uint8_t*>(LV2_ATOM_BODY_CONST(&ev->body));
          we.eng.queueMidi(bytes, ev->body.size, int(ev->time.frames - offset));
        }
      }
    }

    we.process(frames);
  }
}

void lv2Deactivate(LV2_Handle) {}

void lv2Cleanup(LV2_Handle handle)
{
  auto* self = static_cast<Lv2Instance*>(handle);

  if (!self)
    return;

  self->engine.close();
  delete self;
}

const void* lv2ExtensionData(const char*) { return nullptr; }

constexpr unsigned int kAudioPorts = 4; // 2 in, 2 out
constexpr unsigned int kMidiPort = 1;

/** The port count the TTL declares: audio, then one control port per parameter,
 *  then the MIDI atom port. The wrapper and the TTL generator derive this from
 *  the same enumeration, so they cannot disagree. */
unsigned int lv2PortCount(size_t paramCount) { return kAudioPorts + unsigned(paramCount) + kMidiPort; }

LV2_Descriptor gDescriptor = {};

} // namespace

extern "C" LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
  if (index != 0)
    return nullptr;

  // Filled once, on the first call: the parameter surface belongs to an
  // instance, but the port list only needs its size.
  if (!gDescriptor.URI)
  {
    gDescriptor.URI = kPluginUri;
    gDescriptor.instantiate = lv2Instantiate;
    gDescriptor.connect_port = lv2ConnectPort;
    gDescriptor.activate = lv2Activate;
    gDescriptor.run = lv2Run;
    gDescriptor.deactivate = lv2Deactivate;
    gDescriptor.cleanup = lv2Cleanup;
    gDescriptor.extension_data = lv2ExtensionData;
  }

  return &gDescriptor;
}

/** What the TTL generator and the check tool need without instantiating: the
 *  port count for a given parameter count, agreed here so no caller guesses. */
extern "C" LV2_SYMBOL_EXPORT
unsigned int volum_lv2_port_count(unsigned int paramCount)
{
  return lv2PortCount(paramCount);
}
