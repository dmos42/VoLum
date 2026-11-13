/*
 ==============================================================================

 volum-midi-check - verifier le port MIDI du standalone sans personne, ni carte
 son (issue #11).

 Le client attendu en face d'un controleur est un port ALSA seq nomme, que
 aconnect branche. Ce check fait les deux bouts dans le meme processus : le port
 "VoLum" "MIDI In" que volum-app ouvre (midi-support.h), et un port d'envoi
 connecte dessus. Il verifie :

   - le port s'ouvre, porte le nom annonce, et un aconnect le trouverait
     (le client "VoLum" apparait dans /proc/asound/seq/clients) ;
   - note on, note off, CC, program change, pitch bend arrivent en octets MIDI
     exacts, estampilles dans le bloc courant ;
   - ce qu'un cable ne porte pas (horloge, queue) est laisse tomber sans erreur ;
   - le drain ne bloque pas : un port sans rien a lire rend la main de suite.

 Pas de seq sur la machine (pas de /dev/snd, module non charge) : le check dit
 "test ignore" et sort 3 - c'est une machine sans MIDI, pas un port casse.

 ==============================================================================
*/

#include "midi-support.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace volumlinux;

namespace
{

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const char* what)
{
  ++gChecks;
  printf("%s : %s\n", ok ? "ok" : "ECHEC", what);

  if (!ok)
    ++gFailures;
}

bool expectBytes(const std::vector<RawMidiEvent>& events, size_t index, std::initializer_list<uint8_t> want)
{
  if (index >= events.size())
    return false;

  const std::vector<uint8_t> wantVec(want);
  return events[index].bytes == wantVec;
}

/** Send one event now, the way a controller does: direct output, no queue. */
void sendDirect(snd_seq_t* sender, int port, snd_seq_event_type_t type, uint8_t channel, int a, int b)
{
  snd_seq_event_t ev;
  snd_seq_ev_clear(&ev);
  ev.type = type;
  ev.flags = SND_SEQ_TIME_STAMP_REAL;
  ev.time.time.tv_sec = 0;
  ev.time.time.tv_nsec = 0;
  snd_seq_ev_set_source(&ev, port);
  snd_seq_ev_set_dest(&ev, SND_SEQ_ADDRESS_SUBSCRIBERS, SND_SEQ_ADDRESS_UNKNOWN);

  switch (type)
  {
    case SND_SEQ_EVENT_NOTEON:
    case SND_SEQ_EVENT_NOTEOFF:
    case SND_SEQ_EVENT_KEYPRESS:
      ev.data.note.channel = channel;
      ev.data.note.note = a;
      ev.data.note.velocity = b;
      break;
    case SND_SEQ_EVENT_CONTROLLER:
    case SND_SEQ_EVENT_PGMCHANGE:
    case SND_SEQ_EVENT_CHANPRESS:
    case SND_SEQ_EVENT_PITCHBEND:
      ev.data.control.channel = channel;
      ev.data.control.param = a;
      ev.data.control.value = b;
      break;
    default:
      break;
  }

  snd_seq_event_output_direct(sender, &ev);
}

} // namespace

int main()
{
  MidiIn midi;
  std::string err;

  if (!midi.open(err))
  {
    printf("midi-check : alsa seq indisponible (%s) : test ignore\n", err.c_str());
    return 3;
  }

  Check(midi.opened(), "le port s'ouvre");
  Check(!midi.portId().empty(), "le port a un identifiant client:port");

  bool nameListed = false;

  if (std::ifstream clients("/proc/asound/seq/clients"); clients)
  {
    std::string line;

    while (std::getline(clients, line))
      if (line.find("\"VoLum\"") != std::string::npos)
        nameListed = true;
  }

  Check(nameListed, "le client \"VoLum\" est listable (aconnect le trouvera)");

  // The sending half: a second client in this process, connected to the port a
  // controller would connect to. Same driver, same subscription path, no hardware.
  snd_seq_t* sender = nullptr;

  if (snd_seq_open(&sender, "default", SND_SEQ_OPEN_OUTPUT, 0) != 0)
  {
    printf("midi-check : impossible d'ouvrir un port d'envoi : test ignore\n");
    return 3;
  }

  snd_seq_set_client_name(sender, "volum-midi-check");
  const int senderPort = snd_seq_create_simple_port(sender, "sortie", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                                    SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  const int senderClient = snd_seq_client_id(sender);
  (void)senderClient;

  int receiverClient = -1, receiverPort = -1;

  if (sscanf(midi.portId().c_str(), "%d:%d", &receiverClient, &receiverPort) == 2)
    Check(snd_seq_connect_to(sender, senderPort, receiverClient, receiverPort) == 0, "l'envoi se connecte au port");
  else
    Check(false, "l'envoi se connecte au port");

  std::vector<RawMidiEvent> events;

  sendDirect(sender, senderPort, SND_SEQ_EVENT_NOTEON, 0, 60, 100);
  sendDirect(sender, senderPort, SND_SEQ_EVENT_NOTEOFF, 0, 60, 0);
  sendDirect(sender, senderPort, SND_SEQ_EVENT_CONTROLLER, 0, 102, 7);
  sendDirect(sender, senderPort, SND_SEQ_EVENT_PGMCHANGE, 0, 3, 0);
  sendDirect(sender, senderPort, SND_SEQ_EVENT_PITCHBEND, 0, 0, 2000);
  sendDirect(sender, senderPort, SND_SEQ_EVENT_QFRAME, 0, 1, 0);
  sendDirect(sender, senderPort, SND_SEQ_EVENT_START, 0, 0, 0);
  snd_seq_drain_output(sender);

  Check(midi.drain(events, 48000.0, 256, err), "le drain reussit");
  Check(events.size() == 5, "cinq evenements utiles, l'horloge et la queue laisses tomber");
  Check(expectBytes(events, 0, {0x90, 60, 100}), "note on en octets exacts");
  Check(expectBytes(events, 1, {0x80, 60, 0}), "note off en octets exacts");
  Check(expectBytes(events, 2, {0xB0, 102, 7}), "CC 102 (le recall par defaut) en octets exacts");
  Check(expectBytes(events, 3, {0xC0, 3}), "program change en octets exacts");
  Check(expectBytes(events, 4, {0xE0, 80, 79}), "pitch bend en deux octets de sept bits");
  Check(std::all_of(events.begin(), events.end(), [](const RawMidiEvent& e) { return e.offset >= 0 && e.offset < 256; }),
        "chaque evenement est estampille dans le bloc");

  Check(midi.drain(events, 48000.0, 256, err), "le drain reussit");
  Check(events.empty(), "un port sans rien a lire rend la main de suite");

  midi.close();
  snd_seq_delete_simple_port(sender, senderPort);
  snd_seq_close(sender);

  printf("midi-check : %d verification(s), %d echec(s)\n", gChecks, gFailures);
  printf("midi-check : port alsa conforme\n");
  return gFailures ? 1 : 0;
}
