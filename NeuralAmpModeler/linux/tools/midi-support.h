/*
 ==============================================================================

 The standalone's MIDI source (issue #11). A DAW hands the plug-in its MIDI;
 volum-app has nobody to, so it opens an ALSA seq client with one readable port
 - the thing `aconnect`, `qjackctl` and the desktop's patchbay list by name -
 and drains it once per block, on the audio clock the block loop already keeps.

 The port is a plain ALSA seq port, so a foot controller reaches it the same way
 it reaches any other synth: `aconnect 28:0 129:0`, or one click in qjackctl.
 Nothing here knows about the plug-in: what comes out is raw MIDI bytes with a
 sample offset inside the block now starting, and engine-support.h is what
 turns those into VST3 events.

 ==============================================================================
*/

#pragma once

#include <alsa/asoundlib.h>

#include <string>
#include <vector>

namespace volumlinux
{

/** One raw MIDI message, stamped for the block it will be processed in. */
struct RawMidiEvent
{
  std::vector<uint8_t> bytes;
  int offset = 0;
};

class MidiIn
{
public:
  MidiIn() = default;
  ~MidiIn() { close(); }

  MidiIn(const MidiIn&) = delete;
  MidiIn& operator=(const MidiIn&) = delete;

  /** Open the client ("VoLum") and its one readable port ("MIDI In"), non-blocking:
   *  draining must never wait on the audio loop. Returns false with the reason in
   *  `err` - no seq driver on the machine is a normal answer, not a fault. */
  bool open(std::string& err);

  void close();

  bool opened() const { return mSeq != nullptr; }

  /** The client:port pair a sender connects to, once open. */
  std::string portId() const;

  /** Drain every event that arrived, each stamped inside the block now starting:
   *  an event the sender handed over during the previous block is played at the
   *  front of this one, not somewhere behind the wall clock. Returns false when
   *  ALSA itself failed. */
  bool drain(std::vector<RawMidiEvent>& out, double sampleRate, int blockFrames, std::string& err);

private:
  snd_seq_t* mSeq = nullptr;
  int mPort = -1;
};

} // namespace volumlinux
