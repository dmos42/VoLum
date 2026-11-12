/*
 * volum-msgbox-check — vérifier la boîte de message in-app de la couche plateforme
 * Linux sans personne pour regarder.
 *
 *   build/linux/volum-msgbox-check
 *
 * Le client ouvre sa propre fenêtre d'éditeur (sous Xvfb en CI, sur le bureau
 * ailleurs), demande une boîte à ShowMessageBox et y répond avec des événements
 * bouton synthétiques envoyés à la fenêtre que la plateforme a créée — la même
 * astuce que le --click du probe sur la fenêtre d'un plug-in. Ce qui est vérifié :
 *
 *   - une boîte demandée fenêtre ouverte répond kNoResult et n'a pas encore
 *     appelé le gestionnaire d'achèvement ;
 *   - une presse sur le bouton OK répond kOK par le gestionnaire, après coup ;
 *   - tant que la boîte est levée, une presse hors bouton ne répond rien ;
 *   - une boîte YESNO répond kNO quand on presse No, kYES quand on presse Yes.
 *
 * Le rect du bouton vient de la plateforme elle-même (GetMessageBoxButtonRect),
 * donc le test ne duplique aucune métrique de police. Sans serveur X le test
 * s'ignore, comme portal-test.bash.
 */

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

// iPlug 2 before X11: X.h's None and CurrentTime macros would break the enums
// otherwise - the platform layer itself includes them in that order too.
#include "IGraphicsLinux.h"
#include "IGraphicsEditorDelegate.h"

#include <X11/Xlib.h>

using namespace iplug;
using namespace iplug::igraphics;

namespace
{

constexpr int kWindowWidth = 480;
constexpr int kWindowHeight = 320;
constexpr auto kTick = std::chrono::milliseconds(10);
constexpr auto kDrawSettle = std::chrono::milliseconds(300);

/** Deux lectures espacées qui concordent : la mise en page des boutons se fait sur le
 * fil de fenêtre au premier dessin, et le fil du test ne doit pas lire un rect à moitié
 * écrit. Une valeur stable et non vide dit que le dessin est passé. */
IRECT StableButtonRect(IGraphicsLinux* g, int index, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

  while (std::chrono::steady_clock::now() < deadline)
  {
    const IRECT first = g->GetMessageBoxButtonRect(index);
    std::this_thread::sleep_for(kTick);
    const IRECT second = g->GetMessageBoxButtonRect(index);

    if (!first.Empty() && first == second)
      return first;

    std::this_thread::sleep_for(kTick);
  }

  return IRECT();
}

/** Presse et relâchement synthétiques, les mêmes que host-support.h envoie à la fenêtre
 * d'un plug-in : le masque est ce que la fenêtre a demandé, sinon le serveur jette
 * l'événement avant que quiconque le lise. */
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

/** La boîte dessine avec la police par défaut de iPlug 2 (Roboto-Regular) ; le plug-in
 * la charge lui-même, ce client prend la première police système qui existe sous le nom
 * que DEFAULT_FONT attend, et continue sans rien si aucune n'est là (le test reste
 * comportemental : les largeurs tombent à zéro, les boutons gardent leur padding). */
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

int failures = 0;

void Check(bool ok, const char* what)
{
  printf("%s : %s\n", ok ? "ok" : "ECHEC", what);

  if (!ok)
    failures++;
}

} // namespace

class MsgBoxDelegate final : public IGEditorDelegate
{
public:
  MsgBoxDelegate() : IGEditorDelegate(0)
  {
    mMakeGraphicsFunc = [&]() { return new IGraphicsLinux(*this, kWindowWidth, kWindowHeight, 60, 1.f); };
    mLayoutFunc = [](IGraphics* g) { LoadFirstSystemFont(g); };
  }

  void BeginInformHostOfParamChangeFromUI(int paramIdx) override {}
  void EndInformHostOfParamChangeFromUI(int paramIdx) override {}
};

int main()
{
  // Une connexion X au client, la fenêtre à la plateforme : X n'a pas de notion de
  // propriétaire, les événements envoyés par cette connexion arrivent à la fenêtre
  // comme ceux du serveur.
  Display* dpy = XOpenDisplay(nullptr);

  if (!dpy)
  {
    printf("msgbox-check : pas de serveur X, test ignore\n");
    return 0;
  }

  MsgBoxDelegate delegate;

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

  std::this_thread::sleep_for(kDrawSettle);

  // 1. OK : la demande ne repond pas inline, le gestionnaire non plus.
  std::optional<EMsgBoxResult> okAnswer;
  const EMsgBoxResult shown = g->ShowMessageBox("Le modele n'a pas pu etre charge.", "VoLum", kMB_OK,
                                                [&](EMsgBoxResult r) { okAnswer = r; });

  Check(shown == kNoResult, "ShowMessageBox repond kNoResult quand la boite est levee");
  Check(!okAnswer.has_value(), "le gestionnaire n'a rien repondu avant un clic");

  // 2. Le bouton existe des que la boite est dessinee, et une presse hors bouton ne le
  //    lache pas.
  const IRECT okRect = StableButtonRect(g, 0, 5000);

  if (okRect.Empty())
  {
    printf("ECHEC : aucun bouton dessine cinq secondes apres la demande\n");
    return 1;
  }

  SendClick(dpy, window, static_cast<int>(kWindowWidth / 2), static_cast<int>(kWindowHeight / 2) - 40);
  std::this_thread::sleep_for(kDrawSettle);
  Check(!okAnswer.has_value(), "une presse hors bouton ne ferme pas la boite");

  // 3. La presse sur OK repond par le gestionnaire.
  SendClick(dpy, window, static_cast<int>(okRect.MW()), static_cast<int>(okRect.MH()));
  std::this_thread::sleep_for(kDrawSettle);
  Check(okAnswer.has_value() && *okAnswer == kOK, "le bouton OK repond kOK par le gestionnaire");

  // 4. YESNO : No a droite, Yes a gauche.
  std::optional<EMsgBoxResult> yesNoAnswer;
  g->ShowMessageBox("Enregistrer les changements ?", "VoLum", kMB_YESNO,
                    [&](EMsgBoxResult r) { yesNoAnswer = r; });

  const IRECT yesRect = StableButtonRect(g, 0, 5000);
  const IRECT noRect = StableButtonRect(g, 1, 5000);

  if (yesRect.Empty() || noRect.Empty())
  {
    printf("ECHEC : la boite YESNO n'a pas ses deux boutons\n");
    return 1;
  }

  SendClick(dpy, window, static_cast<int>(noRect.MW()), static_cast<int>(noRect.MH()));
  std::this_thread::sleep_for(kDrawSettle);
  Check(yesNoAnswer.has_value() && *yesNoAnswer == kNO, "le bouton No repond kNO");

  std::optional<EMsgBoxResult> yesAnswer;
  g->ShowMessageBox("Enregistrer les changements ?", "VoLum", kMB_YESNO,
                    [&](EMsgBoxResult r) { yesAnswer = r; });

  const IRECT yesRect2 = StableButtonRect(g, 0, 5000);

  if (yesRect2.Empty())
  {
    printf("ECHEC : la deuxieme boite YESNO n'a pas ses boutons\n");
    return 1;
  }

  SendClick(dpy, window, static_cast<int>(yesRect2.MW()), static_cast<int>(yesRect2.MH()));
  std::this_thread::sleep_for(kDrawSettle);
  Check(yesAnswer.has_value() && *yesAnswer == kYES, "le bouton Yes repond kYES");

  delegate.CloseWindow();

  if (failures)
  {
    printf("msgbox-check : %d verification(s) en echec\n", failures);
    return 1;
  }

  printf("msgbox-check : boite de message conforme\n");
  return 0;
}
