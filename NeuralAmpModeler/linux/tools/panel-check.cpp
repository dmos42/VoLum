/*
 * volum-panel-check — vérifier le panneau de réglages audio du standalone Linux
 * (issue #10) sans personne pour regarder.
 *
 *   build/linux/volum-panel-check
 *
 * Le client ouvre sa propre fenêtre d'éditeur (sous Xvfb en CI, sur le bureau
 * ailleurs), attache le panneau avec un modèle fabriqué (périphériques
 * inventés, app.conf temporaire, sonde injectée) et clique les vraies lignes
 * avec des événements synthétiques — la même astuce que le msgbox-check sur la
 * boîte de message. Ce qui est vérifié :
 *
 *   - l'écriture de app.conf est atomique : temp + rename, les lignes qu'on ne
 *     gère pas (commentaires, autres clés) survivent, pas de .tmp qui traîne ;
 *   - cliquer la ligne Sortie puis un périphérique écrit out= dans app.conf ;
 *   - un périphérique que la sonde refuse est répondu DANS la fenêtre (boîte de
 *     message levée) et rien n'est écrit ;
 *   - « aucune » côté entrée écrit no-in et retire la ligne in= ;
 *   - les préréglages de période écrivent period= ;
 *   - la relecture hôte (ApplyConfChanges) n'applique que ce qui a changé — une
 *     clé gagnée en ligne de commande au démarrage ne perd pas sur réécriture.
 *
 * Sans serveur X, la partie fenêtre s'ignore ; l'écriture de app.conf et la
 * relecture restent vérifiées, elles ne dépendent de personne.
 */

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

// iPlug 2 before X11: X.h's None and CurrentTime macros would break the enums
// otherwise - the platform layer itself includes them in that order too.
#include "IGraphicsLinux.h"
#include "IGraphicsEditorDelegate.h"
#include "VoLumLinuxAudioPanel.h"
#include "options-support.h"

#include <X11/Xlib.h>

using namespace iplug;
using namespace iplug::igraphics;
using namespace volumlinux;
using namespace volumlinuxaudio;

namespace
{

constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 480;
constexpr auto kTick = std::chrono::milliseconds(10);
constexpr auto kDrawSettle = std::chrono::milliseconds(300);

int failures = 0;

void Check(bool ok, const char* what)
{
  printf("%s : %s\n", ok ? "ok" : "ECHEC", what);

  if (!ok)
    failures++;
}

std::string ReadFile(const std::string& path)
{
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/** La ligne key= est-elle la ? Une recherche de sous-chaine ne suffit pas :
 * "in=" se cache dans "no-in=". */
bool HasKey(const std::string& content, const std::string& key)
{
  size_t at = 0;

  while ((at = content.find(key + "=", at)) != std::string::npos)
  {
    if (at == 0 || content[at - 1] == '\n')
      return true;

    ++at;
  }

  return false;
}

bool SendClick(Display* dpy, Window window, int x, int y)
{
  XEvent press;
  std::memset(&press, 0, sizeof(press));
  press.type = ButtonPress;
  press.xbutton.display = dpy;
  press.xbutton.window = window;
  press.xbutton.subwindow = window;
  press.xbutton.root = RootWindow(dpy, DefaultScreen(dpy));
  press.xbutton.time = CurrentTime;
  press.xbutton.x = x;
  press.xbutton.y = y;
  press.xbutton.same_screen = True;
  press.xbutton.button = Button1;

  XEvent release = press;
  release.type = ButtonRelease;

  if (XSendEvent(dpy, window, False, ButtonPressMask, &press) == 0)
    return false;

  XFlush(dpy);
  return XSendEvent(dpy, window, False, ButtonReleaseMask, &release) != 0;
}

/** Un rect stable entre deux lectures espacées : le dessin (et la mise en page
 * qui va avec) se fait sur le fil de fenêtre, le fil du test ne doit pas lire
 * un rect à moitié écrit. */
IRECT StableRect(const std::function<IRECT()>& read, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

  while (std::chrono::steady_clock::now() < deadline)
  {
    const IRECT first = read();
    std::this_thread::sleep_for(kTick);
    const IRECT second = read();

    if (!first.Empty() && first == second)
      return first;

    std::this_thread::sleep_for(kTick);
  }

  return IRECT();
}

void LoadFirstSystemFont(IGraphics* g)
{
  static const char* candidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
  };

  for (const char* path : candidates)
  {
    if (g->LoadFont(DEFAULT_FONT, path))
      return;
  }
}

/** Le conf de départ : deux clés gérées, une qui ne l'est pas, un commentaire.
 * Ce que le panneau réécrit ne doit pas les perdre. */
void WriteInitialConf(const std::string& path)
{
  std::ofstream out(path, std::ios::trunc);
  out << "# reglages volum-app\n";
  out << "out=default\n";
  out << "in=default\n";
  out << "period=256\n";
  out << "gain=1\n";
}

} // namespace

class PanelDelegate final : public IGEditorDelegate
{
public:
  PanelDelegate() : IGEditorDelegate(0)
  {
    mMakeGraphicsFunc = [&]() { return new IGraphicsLinux(*this, kWindowWidth, kWindowHeight, 60, 1.f); };
    mLayoutFunc = [](IGraphics* g) { LoadFirstSystemFont(g); };
  }

  void BeginInformHostOfParamChangeFromUI(int paramIdx) override {}
  void EndInformHostOfParamChangeFromUI(int paramIdx) override {}
};

int main()
{
  namespace fs = std::filesystem;

  // Un dossier unique : le test écrit un vrai app.conf, mais dans le sien.
  const std::string dir = "/tmp/volum-panel-check-" + std::to_string(static_cast<long>(::getpid()));
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string conf = dir + "/app.conf";

  // ---- 1. l'écriture atomique, sans fenêtre : elle ne dépend de personne ----
  WriteInitialConf(conf);

  std::string err;
  Check(WriteAppConfValue(conf, "out", "hw:USB", err), "WriteAppConfValue remplace une cle existante");
  Check(ReadFile(conf).find("out=hw:USB") != std::string::npos, "la nouvelle valeur est ecrite");
  Check(ReadFile(conf).find("in=default") != std::string::npos, "les autres cles survivent");
  Check(ReadFile(conf).find("# reglages volum-app") != std::string::npos, "les commentaires survivent");
  Check(ReadFile(conf).find("period=256") != std::string::npos, "period survit");
  Check(!fs::exists(conf + ".tmp", ec), "aucun fichier temporaire qui traine apres un rename");

  Check(WriteAppConfValue(conf, "periods", "2", err), "WriteAppConfValue ajoute une cle absente");
  Check(ReadFile(conf).find("periods=2") != std::string::npos, "la cle ajoutee est la");

  Check(RemoveAppConfKey(conf, "periods", err), "RemoveAppConfKey retire une cle");
  Check(ReadFile(conf).find("periods=") == std::string::npos, "la cle retiree n'y est plus");
  Check(ReadFile(conf).find("out=hw:USB") != std::string::npos, "le reste survit a la suppression");

  // ---- 2. la relecture hote : seules les cles changees sont appliquees ----
  {
    Options opt;
    opt.outDevice = "default";
    opt.inDevice = "default";
    opt.period = 256;
    opt.periods = 2;
    ConfSnapshot seen;
    seen.out = "default";
    seen.in = "default";
    seen.period = 256;
    seen.periods = 2;

    WriteAppConfValue(conf, "out", "hw:USB", err);

    Check(ApplyConfChanges(conf, opt, seen, err), "ApplyConfChanges relit le fichier");
    Check(opt.outDevice == "hw:USB", "la sortie changee est appliquee");
    Check(opt.period == 256, "une periode non changee n'est pas touchee");
    Check(opt.inDevice == "default", "une entree non changee n'est pas touchee");

    Check(ApplyConfChanges(conf, opt, seen, err), "une seconde lecture sans changement reussit");
    Check(opt.outDevice == "hw:USB", "et ne change rien");

    WriteAppConfValue(conf, "period", "128", err);
    Check(ApplyConfChanges(conf, opt, seen, err), "ApplyConfChanges voit le changement de periode");
    Check(opt.period == 128, "la nouvelle periode est appliquee");
    Check(opt.outDevice == "hw:USB", "la sortie garde sa valeur");
  }

  // ---- 3. le panneau lui-meme, dans une vraie fenetre ----
  Display* dpy = XOpenDisplay(nullptr);

  if (!dpy)
  {
    printf("panel-check : pas de serveur X, fin de la partie fenetre\n");
    fs::remove_all(dir, ec);

    if (failures)
    {
      printf("panel-check : %d verification(s) en echec\n", failures);
      return 1;
    }

    printf("panel-check : ecriture et relecture conformes (partie fenetre ignoree)\n");
    return 0;
  }

  PanelDelegate delegate;

  if (!delegate.OpenWindow(nullptr))
  {
    printf("ECHEC : la fenetre de l'editeur ne s'est pas ouverte\n");
    return 1;
  }

  auto* g = static_cast<IGraphicsLinux*>(delegate.GetUI());
  Window window = reinterpret_cast<Window>(g->GetWindow());

  if (!window)
  {
    printf("ECHEC : pas de fenetre X cote plateforme\n");
    return 1;
  }

  AudioPanelModel model;
  model.confPath = conf;
  model.out = "default";
  model.in = "default";
  model.period = 256;
  model.periods = 2;
  model.rate = 48000;
  model.blockFrames = 256;
  model.alsaListed = true;
  model.outs = {"default", "hw:USB", "hw:Occupe"};
  model.ins = {"default", "hw:USB"};

  auto* panel = new VoLumLinuxAudioPanelControl();
  g->AttachControl(panel);
  panel->Show(model, g->GetBounds());
  g->SetAllControlsDirty();
  std::this_thread::sleep_for(kDrawSettle);

  Check(panel->IsShown(), "le panneau est leve");
  const IRECT outRow = StableRect([&] { return panel->RowRect(VoLumLinuxAudioPanelControl::kSectionOut); }, 5000);

  if (outRow.Empty())
  {
    printf("ECHEC : la ligne Sortie n'est pas dessinee cinq secondes apres l'ouverture\n");
    return 1;
  }

  auto click = [&](float x, float y) {
    SendClick(dpy, window, static_cast<int>(x), static_cast<int>(y));
    std::this_thread::sleep_for(kDrawSettle);
  };

  // 3a. La ligne Sortie se deploye, un choix ecrit le conf. La sonde reelle
  // ne connait pas hw:USB : on injecte une sonde qui accepte, le refus est
  // le sujet de 3b. Le conf repart de l'etat initial : les tests sans
  // fenetre l'ont deja modifie.
  WriteInitialConf(conf);
  panel->SetProbe([](const std::string&, bool, std::string&) { return true; });

  click(outRow.MW(), outRow.MH());
  Check(panel->Expanded() == VoLumLinuxAudioPanelControl::kSectionOut, "cliquer Sortie deploye la liste");

  const IRECT choice = StableRect([&] { return panel->ChoiceRect(VoLumLinuxAudioPanelControl::kSectionOut, 1); }, 5000);
  Check(!choice.Empty(), "le choix hw:USB a un rect");

  click(choice.MW(), choice.MH());
  Check(ReadFile(conf).find("out=hw:USB") != std::string::npos, "le choix a ecrit out=hw:USB dans app.conf");
  Check(panel->StatusText().find("sortie") != std::string::npos, "le panneau dit ce que le choix a coute");
  Check(panel->Expanded() == -1, "la liste est repliee apres le choix");

  // 3b. Un peripherie que la sonde refuse : reponse dans la fenetre, rien d'ecrit.
  panel->SetProbe([](const std::string&, bool, std::string& err) {
    err = "Device or resource busy";
    return false;
  });

  click(outRow.MW(), outRow.MH());

  const IRECT busy = StableRect([&] { return panel->ChoiceRect(VoLumLinuxAudioPanelControl::kSectionOut, 2); }, 5000);
  Check(!busy.Empty(), "le choix hw:Occupe a un rect");

  click(busy.MW(), busy.MH());

  const IRECT okButton = StableRect([&] { return g->GetMessageBoxButtonRect(0); }, 5000);
  Check(!okButton.Empty(), "le refus est repondu par une boite de message in-app");
  Check(ReadFile(conf).find("out=hw:USB") != std::string::npos, "un peripherie refuse n'ecrit rien");

  if (!okButton.Empty())
    click(okButton.MW(), okButton.MH());

  // 3c. « aucune » a l'entree : no-in ecrit, la ligne in= retiree.
  panel->SetProbe([](const std::string&, bool, std::string&) { return true; });

  const IRECT inRow = StableRect([&] { return panel->RowRect(VoLumLinuxAudioPanelControl::kSectionIn); }, 5000);
  click(inRow.MW(), inRow.MH());

  const IRECT none = StableRect([&] { return panel->ChoiceRect(VoLumLinuxAudioPanelControl::kSectionIn, 0); }, 5000);
  Check(!none.Empty(), "le choix aucune a un rect");

  click(none.MW(), none.MH());
  Check(HasKey(ReadFile(conf), "no-in"), "aucune a l'entree ecrit no-in=");
  Check(!HasKey(ReadFile(conf), "in"), "la ligne in= est retiree avec");

  // 3d. Les prereglages de periode.
  const IRECT periodRow = StableRect([&] { return panel->RowRect(VoLumLinuxAudioPanelControl::kSectionPeriod); }, 5000);
  click(periodRow.MW(), periodRow.MH());

  const IRECT p128 = StableRect([&] { return panel->ChoiceRect(VoLumLinuxAudioPanelControl::kSectionPeriod, 1); }, 5000);
  Check(!p128.Empty(), "le prereglage 128 a un rect");

  click(p128.MW(), p128.MH());
  Check(ReadFile(conf).find("period=128") != std::string::npos, "le prereglage a ecrit period=128");
  Check(panel->StatusText().find("128") != std::string::npos, "le statut annonce le tampon vise");

  // 3e. La croix ferme le panneau.
  const IRECT close = StableRect([&] { return panel->CloseRect(); }, 5000);
  Check(!close.Empty(), "la croix a un rect");

  click(close.MW(), close.MH());
  Check(!panel->IsShown(), "la croix ferme le panneau");

  delegate.CloseWindow();
  fs::remove_all(dir, ec);

  if (failures)
  {
    printf("panel-check : %d verification(s) en echec\n", failures);
    return 1;
  }

  printf("panel-check : panneau audio conforme\n");
  return 0;
}
