/*
 ==============================================================================

 Implementation of midi-support.h. The seq client is one handle and one port;
 the parts worth care are the non-blocking drain and the stamping, because both
 run in the loop that paces the audio.

 ==============================================================================
*/

#include "midi-support.h"

#include <ctime>

namespace volumlinux
{

namespace
{

long long nowNs()
{
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    return 0;

  return static_cast<long long>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

void push(std::vector<RawMidiEvent>& out, const uint8_t* bytes, size_t len, int offset)
{
  RawMidiEvent ev;
  ev.bytes.assign(bytes, bytes + len);
  ev.offset = offset;
  out.push_back(std::move(ev));
}

/** One seq event to raw MIDI bytes. What a running plug-in can act on is what a
 *  cable would carry; the rest (queue control, tick clocks) is dropped here. */
void seqEventToBytes(const snd_seq_event_t& ev, std::vector<RawMidiEvent>& out, int offset)
{
  const uint8_t ch = static_cast<uint8_t>(ev.data.note.channel & 0x0F);

  switch (ev.type)
  {
    case SND_SEQ_EVENT_NOTEON:
    {
      const uint8_t bytes[3] = { static_cast<uint8_t>(0x90 | ch), static_cast<uint8_t>(ev.data.note.note & 0x7F),
                                 static_cast<uint8_t>(ev.data.note.velocity & 0x7F) };
      push(out, bytes, 3, offset);
      break;
    }
    case SND_SEQ_EVENT_NOTEOFF:
    {
      const uint8_t bytes[3] = { static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(ev.data.note.note & 0x7F),
                                 static_cast<uint8_t>(ev.data.note.velocity & 0x7F) };
      push(out, bytes, 3, offset);
      break;
    }
    case SND_SEQ_EVENT_KEYPRESS:
    {
      const uint8_t bytes[3] = { static_cast<uint8_t>(0xA0 | ch), static_cast<uint8_t>(ev.data.note.note & 0x7F),
                                 static_cast<uint8_t>(ev.data.note.velocity & 0x7F) };
      push(out, bytes, 3, offset);
      break;
    }
    case SND_SEQ_EVENT_CONTROLLER:
    {
      const uint8_t bytes[3] = { static_cast<uint8_t>(0xB0 | ch), static_cast<uint8_t>(ev.data.control.param & 0x7F),
                                 static_cast<uint8_t>(ev.data.control.value & 0x7F) };
      push(out, bytes, 3, offset);
      break;
    }
    case SND_SEQ_EVENT_PGMCHANGE:
    {
      const uint8_t bytes[2] = { static_cast<uint8_t>(0xC0 | ch), static_cast<uint8_t>(ev.data.control.value & 0x7F) };
      push(out, bytes, 2, offset);
      break;
    }
    case SND_SEQ_EVENT_CHANPRESS:
    {
      const uint8_t bytes[2] = { static_cast<uint8_t>(0xD0 | ch), static_cast<uint8_t>(ev.data.control.value & 0x7F) };
      push(out, bytes, 2, offset);
      break;
    }
    case SND_SEQ_EVENT_PITCHBEND:
    {
      // ALSA hands the bend over as -8192..8191; a cable would carry the two 7-bit
      // halves, and the VST3 legacy event wants them the same way.
      const int bend = ev.data.control.value + 8192;
      const uint8_t bytes[3] = { static_cast<uint8_t>(0xE0 | ch), static_cast<uint8_t>(bend & 0x7F),
                                 static_cast<uint8_t>((bend >> 7) & 0x7F) };
      push(out, bytes, 3, offset);
      break;
    }
    default:
      break;
  }
}

} // namespace

bool MidiIn::open(std::string& err)
{
  if (mSeq)
    return true;

  if (snd_seq_open(&mSeq, "default", SND_SEQ_OPEN_INPUT, 0) != 0)
  {
    mSeq = nullptr;
    err = "snd_seq_open a refuse le client (pas de pilote seq ?)";
    return false;
  }

  snd_seq_set_client_name(mSeq, "VoLum");
  mPort = snd_seq_create_simple_port(mSeq, "MIDI In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                     SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

  if (mPort < 0)
  {
    err = "impossible de creer le port MIDI In";
    snd_seq_close(mSeq);
    mSeq = nullptr;
    return false;
  }

  // The drain runs in the audio loop: a sender that stalls must never make the
  // loop wait, so reading never blocks.
  snd_seq_nonblock(mSeq, 1);
  err.clear();
  return true;
}

void MidiIn::close()
{
  if (mSeq)
    snd_seq_close(mSeq);

  mSeq = nullptr;
  mPort = -1;
}

std::string MidiIn::portId() const
{
  if (!mSeq)
    return {};

  return std::to_string(snd_seq_client_id(mSeq)) + ":" + std::to_string(mPort);
}

bool MidiIn::drain(std::vector<RawMidiEvent>& out, double sampleRate, int blockFrames, std::string& err)
{
  out.clear();

  if (!mSeq)
    return true;

  const long long base = nowNs();

  while (snd_seq_event_input_pending(mSeq, 1) > 0)
  {
    snd_seq_event_t* ev = nullptr;

    if (snd_seq_event_input(mSeq, &ev) < 0 || !ev)
    {
      err = "lecture du port MIDI interrompue";
      return false;
    }

    // The event's own real-time stamp, read against the moment this block starts:
    // something sent while the previous block was still being processed lands at
    // the front of this one, something scheduled ahead lands where it asked.
    long long offset = 0;

    if (ev->flags & SND_SEQ_TIME_STAMP_REAL)
    {
      const long long at = static_cast<long long>(ev->time.time.tv_sec) * 1000000000ll + ev->time.time.tv_nsec;
      offset = static_cast<long long>((static_cast<double>(at - base) / 1e9) * sampleRate);
    }

    if (offset < 0)
      offset = 0;

    if (offset > blockFrames - 1)
      offset = blockFrames - 1;

    seqEventToBytes(*ev, out, static_cast<int>(offset));
  }

  err.clear();
  return true;
}

} // namespace volumlinux
