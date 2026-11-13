/*

 ==============================================================================

 VoLumLinuxAudioPanel - the Linux standalone's audio settings panel (issue #10).

 The Windows standalone opens the app host's Preferences dialog; the Linux
 standalone is its own small host (volum-app) with no dialog to open, so the
 panel is drawn by the plug-in's own IGraphics, the way the in-app message box
 and the pop-up menu already are. Same .so in a DAW and in the standalone: the
 panel only exists when the process was marked as the standalone, which the
 host does with VOLUM_APP_CONF before the module is even loaded - a DAW never
 sets it, so a DAW never pays for any of this.

 The contract between the host and the panel is environment variables, read
 when the panel opens:

   VOLUM_APP_CONF     the app.conf path the panel writes (also the marker)
   VOLUM_APP_OUT      the device the running playback stream was opened with
   VOLUM_APP_IN       the capture device, empty when there is no capture
   VOLUM_APP_PERIOD   the period the host asked for
   VOLUM_APP_PERIODS  periods in the buffer
   VOLUM_APP_GAIN     the input trim
   VOLUM_APP_RATE     the rate the device agreed to (set by runHost)
   VOLUM_APP_BLOCK    the block the plug-in was set up with (set by runHost)

 libasound is dlopen()ed, never linked: the plug-in keeps building and loading
 on machines without ALSA at all, and the panel degrades to a message instead
 of taking the DAW down with it.

 Choosing writes app.conf atomically - the file is rewritten through a temp
 file and one rename() - and says what it costs: the stream has to be reopened,
 which the host does when it notices the file change, and prints the newly
 negotiated period and total buffer. A device that cannot be opened right now
 (the desktop is holding the card, say) is probed before anything is written,
 and the answer is drawn in-app, not logged where nobody looks - this is the
 path issue #2 was about, and the panel leans on the message box that came out
 of it.

 The control carries no font metrics of its own: rows are laid out from
 MeasureText, and the row rects are readable (RowRect / ChoiceRect) so a
 headless check on Xvfb can click real rows the way it clicks the message box
 buttons.

 ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "IControl.h"
#include "IGraphics.h"

namespace volumlinuxaudio
{
// The panel is written against the IGraphics vocabulary directly; these stay
// inside this namespace, so includers see none of it.
using namespace iplug;
using namespace iplug::igraphics;

// MARK: standalone marker

/** True only in a process volum-app marked before loading the module. A DAW
 * never sets VOLUM_APP_CONF, so this is the whole DAW-safety gate. */
inline bool RunningUnderLinuxStandalone()
{
  const char* conf = std::getenv("VOLUM_APP_CONF");
  return conf && *conf;
}

// MARK: app.conf writing

/** Replace one key=value line in a config file, keeping every other line - the
 * comments and the keys this panel does not manage survive a choice. The write
 * is atomic: the new content goes to a temp file next to the target and one
 * rename() puts it in place, so a reader (the host, the next launch) never
 * sees a half-written file, and a crash mid-write loses nothing. Creates the
 * file when it does not exist yet. */
inline bool WriteAppConfValue(const std::string& path, const std::string& key, const std::string& value,
                              std::string& err)
{
  if (path.empty())
  {
    err = "chemin de reglages inconnu";
    return false;
  }

  std::filesystem::path target(path);
  std::error_code ec;

  if (target.has_parent_path())
    std::filesystem::create_directories(target.parent_path(), ec);

  std::vector<std::string> lines;
  {
    std::ifstream in(path);

    if (in)
    {
      std::string line;

      while (std::getline(in, line))
        lines.push_back(line);
    }
  }

  const std::string wanted = key + "=" + value;
  bool replaced = false;

  for (std::string& line : lines)
  {
    const size_t eq = line.find('=');

    if (eq != std::string::npos && line.substr(0, eq) == key)
    {
      line = wanted;
      replaced = true;
    }
  }

  if (!replaced)
    lines.push_back(wanted);

  const std::string temp = target.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);

    if (!out)
    {
      err = "ecriture impossible dans " + temp;
      return false;
    }

    for (const std::string& line : lines)
      out << line << "\n";

    out.flush();

    if (!out)
    {
      err = "ecriture incomplete dans " + temp;
      std::filesystem::remove(temp, ec);
      return false;
    }
  }

  std::filesystem::rename(temp, target, ec);

  if (ec)
  {
    err = "renommage impossible : " + ec.message();
    std::filesystem::remove(temp, ec);
    return false;
  }

  return true;
}

/** Drop every line carrying that key. The panel needs it because some states
 * are keys of their own: "no capture" is `no-in`, and an `in=` line left
 * behind it would win or lose depending on line order. */
inline bool RemoveAppConfKey(const std::string& path, const std::string& key, std::string& err)
{
  if (path.empty())
  {
    err = "chemin de reglages inconnu";
    return false;
  }

  std::ifstream in(path);

  if (!in)
    return true; // nothing there, nothing to remove

  std::vector<std::string> lines;
  std::string line;

  while (std::getline(in, line))
  {
    const size_t eq = line.find('=');

    if (eq == std::string::npos || line.substr(0, eq) != key)
      lines.push_back(line);
  }

  in.close();

  if (lines.size() == 0)
  {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }

  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);

    if (!out)
    {
      err = "ecriture impossible dans " + temp;
      return false;
    }

    for (const std::string& kept : lines)
      out << kept << "\n";
  }

  std::error_code ec;
  std::filesystem::rename(temp, path, ec);

  if (ec)
  {
    err = "renommage impossible : " + ec.message();
    std::filesystem::remove(temp, ec);
    return false;
  }

  return true;
}

// MARK: ALSA, dlopened

/** The playback and capture names snd_device_name_hint announces. Returns
 * false when libasound is not there at all (err says so); an ALSA that is
 * there but announces nothing is an empty list, not a failure. */
inline bool EnumerateAlsaChoices(std::vector<std::string>& outs, std::vector<std::string>& ins, std::string& err)
{
  void* asound = dlopen("libasound.so.2", RTLD_LAZY);

  if (!asound)
  {
    err = "libasound introuvable : la liste des peripheriques est indisponible";
    return false;
  }

  using NameHintFn = int (*)(int, const char*, void***);
  using HintGetFn = char* (*)(void*, const char*);
  using HintFreeFn = void (*)(char**);

  const auto nameHint = reinterpret_cast<NameHintFn>(dlsym(asound, "snd_device_name_hint"));
  const auto hintGet = reinterpret_cast<HintGetFn>(dlsym(asound, "snd_device_name_get_hint"));
  const auto hintFree = reinterpret_cast<HintFreeFn>(dlsym(asound, "snd_device_name_hint_free"));

  if (!nameHint || !hintGet || !hintFree)
  {
    err = "libasound trop ancienne : snd_device_name_hint manque";
    dlclose(asound);
    return false;
  }

  char** hint = nullptr;

  if (nameHint(-1, "pcm", reinterpret_cast<void***>(&hint)) < 0)
  {
    dlclose(asound);
    return true; // an ALSA that announces nothing is not an error to draw
  }

  for (char** h = hint; *h; ++h)
  {
    char* name = hintGet(*h, "NAME");
    char* desc = hintGet(*h, "DESCRIPTION");
    char* ioid = hintGet(*h, "IOID");

    if (name)
    {
      // IOID null means both directions; otherwise it says which one this name is.
      if (!ioid || std::strcmp(ioid, "Output") == 0)
        outs.push_back(name);

      if (!ioid || std::strcmp(ioid, "Capture") == 0)
        ins.push_back(name);
    }

    std::free(name);
    std::free(desc);
    std::free(ioid);
  }

  hintFree(hint);
  dlclose(asound);
  return true;
}

/** Can this device be opened right now? The panel asks before it writes
 * anything, so "the desktop is holding the card" is answered in-app instead of
 * being discovered as a dead stream. A missing libasound counts as "cannot
 * probe": the caller decides whether that is worth a message. */
inline bool AlsaProbeDevice(const std::string& device, bool capture, std::string& err)
{
  void* asound = dlopen("libasound.so.2", RTLD_LAZY);

  if (!asound)
  {
    err = "libasound introuvable";
    return false;
  }

  using OpenFn = int (*)(void**, const char*, int, int);
  using CloseFn = int (*)(void*);

  const auto pcmOpen = reinterpret_cast<OpenFn>(dlsym(asound, "snd_pcm_open"));
  const auto pcmClose = reinterpret_cast<CloseFn>(dlsym(asound, "snd_pcm_close"));
  const auto strerrorFn = reinterpret_cast<const char* (*)(int)>(dlsym(asound, "snd_strerror"));

  if (!pcmOpen || !pcmClose)
  {
    err = "libasound trop ancienne : snd_pcm_open manque";
    dlclose(asound);
    return false;
  }

  // SND_PCM_STREAM_PLAYBACK = 0, SND_PCM_STREAM_CAPTURE = 1; the async flag 0
  // is what every opener passes.
  void* pcm = nullptr;
  const int rc = pcmOpen(&pcm, device.c_str(), capture ? 1 : 0, 0);

  if (rc != 0)
    err = strerrorFn ? strerrorFn(rc) : "peripherie refuse";

  if (pcm)
    pcmClose(pcm);

  dlclose(asound);
  return rc == 0;
}

// MARK: the model the panel shows

struct AudioPanelModel
{
  std::string confPath;
  std::string out; // what the running stream was opened with
  std::string in;  // "" = no capture (--no-in)
  unsigned int period = 256;
  unsigned int periods = 2;
  double gain = 1.0;
  unsigned int rate = 0;        // negotiated, what the device agreed to
  unsigned int blockFrames = 0; // negotiated, what the plug-in was set up with
  std::vector<std::string> outs;
  std::vector<std::string> ins;
  bool alsaListed = false; // false: libasound was missing, the lists are empty for that reason
};

inline unsigned int EnvUnsigned(const char* name, unsigned int fallback)
{
  if (const char* v = std::getenv(name))
  {
    const unsigned long parsed = std::strtoul(v, nullptr, 10);

    if (parsed > 0)
      return static_cast<unsigned int>(parsed);
  }

  return fallback;
}

/** Build the panel's model from the contract the host wrote into the
 * environment, plus whatever ALSA announces right now. Returns false when the
 * process is not the standalone (no marker) - the caller then shows nothing. */
inline bool ReadPanelModelFromEnv(AudioPanelModel& model)
{
  const char* conf = std::getenv("VOLUM_APP_CONF");

  if (!conf || !*conf)
    return false;

  model.confPath = conf;

  if (const char* v = std::getenv("VOLUM_APP_OUT"))
    model.out = v;

  if (const char* v = std::getenv("VOLUM_APP_IN"))
    model.in = v;

  model.period = EnvUnsigned("VOLUM_APP_PERIOD", model.period);
  model.periods = EnvUnsigned("VOLUM_APP_PERIODS", model.periods);

  if (const char* v = std::getenv("VOLUM_APP_GAIN"))
    model.gain = std::atof(v);

  if (const char* v = std::getenv("VOLUM_APP_RATE"))
    model.rate = static_cast<unsigned int>(std::strtoul(v, nullptr, 10));

  if (const char* v = std::getenv("VOLUM_APP_BLOCK"))
    model.blockFrames = static_cast<unsigned int>(std::strtoul(v, nullptr, 10));

  std::string err;
  model.alsaListed = EnumerateAlsaChoices(model.outs, model.ins, err);
  return true;
}

// MARK: the control

/** The panel itself: a dimmed canvas, one panel with a row per setting, and -
 * when a row is expanded - the choices under it. One control does all of it,
 * the way the message box does: attached last so it draws on top and takes
 * every click while it is up, laid out from MeasureText so no font metric is
 * duplicated, and readable through RowRect / ChoiceRect so a headless check on
 * Xvfb can click real rows. */
class VoLumLinuxAudioPanelControl : public IControl
{
public:
  /** Answers "can this device be opened right now". The default asks ALSA;
   * the headless check replaces it, because its devices are made-up names. */
  using ProbeFn = std::function<bool(const std::string& device, bool capture, std::string& err)>;

  static constexpr int kSectionOut = 0;
  static constexpr int kSectionIn = 1;
  static constexpr int kSectionPeriod = 2;
  static constexpr int kSectionCount = 3;

  static constexpr int kMaxChoicesShown = 6;

  VoLumLinuxAudioPanelControl() : IControl(IRECT()) { Hide(true); }

  bool IsShown() const { return !IsHidden(); }

  void Show(const AudioPanelModel& model, const IRECT& canvas)
  {
    mModel = model;
    mExpanded = -1;
    mStatus.clear();
    mRECT = canvas;
    mTargetRECT = canvas;
    mLayout.rows.clear();
    mLayout.choices.clear();
    Hide(false);
  }

  void Dismiss()
  {
    Hide(true); // marks the area dirty: the next frame repaints without the panel
    mLayout.rows.clear();
    mLayout.choices.clear();
  }

  void SetProbe(ProbeFn probe) { mProbe = std::move(probe); }

  void Draw(IGraphics& g) override
  {
    if (IsHidden())
      return; // a dismissed panel must not re-layout itself: iPlug 2 still draws control 0 while hidden

    const IRECT canvas = g.GetBounds();

    if (canvas != mRECT) // the window was resized while the panel was up
    {
      mRECT = canvas;
      mTargetRECT = canvas;
      mLayout.rows.clear();
      mLayout.choices.clear();
    }

    if (mLayout.rows.empty())
      Layout(g);

    g.FillRect(kOverlayColor, mRECT);
    g.FillRoundRect(kPanelColor, mPanel, kPanelRadius);
    g.DrawRoundRect(kBorderColor, mPanel, kPanelRadius);

    g.DrawText(kTitleText, "Audio", mTitleRect);

    for (const auto& row : mLayout.rows)
    {
      const bool expanded = row.section == mExpanded;
      g.FillRoundRect(row.rect.Contains(mMouseX, mMouseY) || expanded ? kRowHoverColor : kRowColor, row.rect,
                      kRowRadius);
      g.DrawRoundRect(kBorderColor, row.rect, kRowRadius);
      g.DrawText(kRowText, row.label.c_str(), row.labelRect);
      g.DrawText(kValueText, row.value.c_str(), row.valueRect);
    }

    for (const auto& choice : mLayout.choices)
    {
      g.FillRoundRect(choice.rect.Contains(mMouseX, mMouseY) ? kChoiceHoverColor : kChoiceColor, choice.rect,
                      kRowRadius);
      g.DrawText(kChoiceText, choice.label.c_str(), choice.rect.GetHPadded(8.f));
    }

    if (!mLayout.moreLine.empty())
      g.DrawText(kMoreText, mLayout.moreLine.c_str(), mLayout.moreRect);

    if (!mStatus.empty())
      g.DrawText(kStatusText, mStatus.c_str(), mStatusRect);

    g.FillRoundRect(mCloseRect.Contains(mMouseX, mMouseY) ? kCloseHoverColor : kCloseColor, mCloseRect, kRowRadius);
    g.DrawText(kCloseText, "x", mCloseRect);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    (void)mod;

    if (mCloseRect.Contains(x, y))
    {
      Dismiss();
      return;
    }

    for (const auto& row : mLayout.rows)
    {
      if (row.rect.Contains(x, y))
      {
        mExpanded = mExpanded == row.section ? -1 : row.section;
        mLayout.rows.clear();
        mLayout.choices.clear();
        SetDirty(false);
        return;
      }
    }

    for (const auto& choice : mLayout.choices)
    {
      if (choice.rect.Contains(x, y))
      {
        Apply(choice);
        return;
      }
    }
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    (void)mod;
    mMouseX = x;
    mMouseY = y;
    SetDirty(false);
  }

  void OnMouseOut() override { SetDirty(false); }

  // MARK: introspection for the headless check

  IRECT RowRect(int section) const
  {
    for (const auto& row : mLayout.rows)
      if (row.section == section)
        return row.rect;

    return IRECT();
  }

  IRECT ChoiceRect(int section, int index) const
  {
    if (section != mExpanded)
      return IRECT();

    int seen = 0;

    for (const auto& choice : mLayout.choices)
    {
      if (choice.section == section)
      {
        if (seen == index)
          return choice.rect;

        ++seen;
      }
    }

    return IRECT();
  }

  int Expanded() const { return mExpanded; }
  const std::string& StatusText() const { return mStatus; }
  const AudioPanelModel& Model() const { return mModel; }
  IRECT CloseRect() const { return mCloseRect; }

private:
  struct Row
  {
    int section;
    IRECT rect;
    IRECT labelRect;
    IRECT valueRect;
    std::string label;
    std::string value;
  };

  struct Choice
  {
    int section;
    int index;
    IRECT rect;
    std::string label;
    std::string value;
  };

  struct PanelLayout
  {
    std::vector<Row> rows;
    std::vector<Choice> choices;
    std::string moreLine;
    IRECT moreRect;
  };

  std::string CurrentLabel(int section) const
  {
    switch (section)
    {
      case kSectionOut: return mModel.out.empty() ? "default" : mModel.out;
      case kSectionIn: return mModel.in.empty() ? "aucune" : mModel.in;
      case kSectionPeriod: default: return std::to_string(mModel.period) + " images";
    }
  }

  static std::string SectionTitle(int section)
  {
    switch (section)
    {
      case kSectionOut: return "Sortie";
      case kSectionIn: return "Entree";
      case kSectionPeriod: default: return "Periode";
    }
  }

  /** The choices a section offers, the current one first marked when drawn.
   * The IN section carries "(aucune)" so a wrong capture can be un-chosen;
   * its value is the empty string, which the writer turns into `no-in`. */
  std::vector<std::string> ChoicesFor(int section) const
  {
    if (section == kSectionOut)
      return mModel.outs;

    if (section == kSectionIn)
    {
      std::vector<std::string> choices;
      choices.push_back("");
      choices.insert(choices.end(), mModel.ins.begin(), mModel.ins.end());
      return choices;
    }

    std::vector<std::string> presets;

    for (unsigned int p : {64u, 128u, 256u, 512u})
      presets.push_back(std::to_string(p));

    return presets;
  }

  void Layout(IGraphics& g)
  {
    mLayout.rows.clear();
    mLayout.choices.clear();
    mLayout.moreLine.clear();

    // Height first, from the content: a panel sized to its content reads as a
    // dialog, a panel sized to the window reads as an empty page.
    float contentH = kPadding + kTitleHeight + kTitleGap;

    for (int section = 0; section < kSectionCount; ++section)
    {
      contentH += kRowHeight + kRowGap;

      if (section != mExpanded)
        continue;

      const size_t choiceCount = std::min<size_t>(ChoicesFor(section).size(), kMaxChoicesShown);
      contentH += float(choiceCount) * (kChoiceHeight + 2.f);

      const size_t total = ChoicesFor(section).size();

      if (total > kMaxChoicesShown || (section == kSectionIn && !mModel.alsaListed))
        contentH += kMoreHeight + 4.f;
    }

    contentH += kStatusHeight + kPadding;

    const float panelW = std::min(mRECT.W() - 2 * kMargin, kPanelMaxWidth);
    const float panelH = std::min(mRECT.H() - 2 * kMargin, contentH);
    mPanel = IRECT(mRECT.MW() - panelW / 2, mRECT.MH() - panelH / 2, mRECT.MW() + panelW / 2, mRECT.MH() + panelH / 2);

    mTitleRect = IRECT(mPanel.L + kPadding, mPanel.T + kPadding, mPanel.R - kPadding - kCloseSize - 6.f,
                       mPanel.T + kPadding + kTitleHeight);
    mCloseRect = IRECT(mPanel.R - kPadding - kCloseSize, mPanel.T + kPadding, mPanel.R - kPadding,
                       mPanel.T + kPadding + kCloseSize);

    float y = mTitleRect.B + kTitleGap;

    for (int section = 0; section < kSectionCount; ++section)
    {
      const IRECT rowRect(mPanel.L + kPadding, y, mPanel.R - kPadding, y + kRowHeight);
      y += kRowHeight + kRowGap;

      Row row{section,
              rowRect,
              IRECT(rowRect.L + 8.f, rowRect.T, rowRect.L + 8.f + kLabelWidth, rowRect.B),
              IRECT(rowRect.R - kValueWidth - 8.f, rowRect.T, rowRect.R - 8.f, rowRect.B),
              SectionTitle(section),
              CurrentLabel(section)};
      mLayout.rows.push_back(row);

      if (section != mExpanded)
        continue;

      const std::vector<std::string> choices = ChoicesFor(section);
      const std::string current = CurrentLabel(section);
      const int shown = std::min<int>(static_cast<int>(choices.size()), kMaxChoicesShown);

      for (int i = 0; i < shown; ++i)
      {
        const std::string& value = choices[i];
        const bool isCurrent = section == kSectionPeriod ? std::to_string(mModel.period) == value
                                                         : value == (section == kSectionOut ? mModel.out : mModel.in);
        const std::string label = (isCurrent ? "> " : "   ") + (value.empty() ? "(aucune)" : value);
        const IRECT rect(mPanel.L + kPadding + kChoiceIndent, y, mPanel.R - kPadding - kChoiceIndent,
                         y + kChoiceHeight);
        y += kChoiceHeight + 2.f;
        mLayout.choices.push_back({section, i, rect, label, value});
      }

      if (choices.size() > size_t(kMaxChoicesShown))
      {
        mLayout.moreLine = "... et " + std::to_string(choices.size() - kMaxChoicesShown) + " autres : volum-app --list";
        mLayout.moreRect = IRECT(mPanel.L + kPadding + kChoiceIndent, y, mPanel.R - kPadding, y + kMoreHeight);
        y += kMoreHeight + 4.f;
      }
      else if (section == kSectionIn && !mModel.alsaListed)
      {
        mLayout.moreLine = "libasound introuvable : liste indisponible";
        mLayout.moreRect = IRECT(mPanel.L + kPadding + kChoiceIndent, y, mPanel.R - kPadding, y + kMoreHeight);
        y += kMoreHeight + 4.f;
      }
    }

    mStatusRect = IRECT(mPanel.L + kPadding, mPanel.B - kPadding - kStatusHeight, mPanel.R - kPadding,
                        mPanel.B - kPadding);
  }

  void Apply(const Choice& choice)
  {
    if (choice.section == kSectionPeriod)
    {
      if (WriteConf("period", choice.value))
        mStatus = "periode enregistree : le flux est rouvert, tampon vise " + choice.value + " x " +
                  std::to_string(mModel.periods) + " images";

      return;
    }

    const bool capture = choice.section == kSectionIn;
    std::string err;

    if (mProbe && !choice.value.empty() && !mProbe(choice.value, capture, err))
    {
      // The card is held by the desktop, say, and nothing was written: the
      // answer has to be drawn, not logged.
      GetUI()->ShowMessageBox(("Peripherie \"" + choice.value + "\" refusera de s'ouvrir : " + err +
                               ".\nRien n'a ete change.")
                                .c_str(),
                              "VoLum", EMsgBoxType::kMB_OK);
      return;
    }

    if (capture)
    {
      // "no capture" is a key of its own (no-in), and an empty in= would fail
      // the whole config file on the next read - so the two states manage each
      // other's lines instead of writing an empty value.
      if (choice.value.empty())
      {
        if (!WriteConf("no-in", ""))
          return;

        if (!RemoveAppConfKey(mModel.confPath, "in", err))
        {
          Fail("in", err);
          return;
        }
      }
      else
      {
        if (!RemoveAppConfKey(mModel.confPath, "no-in", err))
        {
          Fail("no-in", err);
          return;
        }

        if (!WriteConf("in", choice.value))
          return;
      }

      mModel.in = choice.value;
    }
    else
    {
      if (!WriteConf("out", choice.value))
        return;

      mModel.out = choice.value;
    }

    mStatus = capture ? (choice.value.empty() ? "capture desactivee : le flux est rouvert sans entree"
                                              : "entree enregistree : le flux est rouvert sur \"" + choice.value + "\"")
                      : "sortie enregistree : le flux est rouvert sur \"" + choice.value + "\"";
    mExpanded = -1;
    mLayout.rows.clear();
    mLayout.choices.clear();
    SetDirty(false);
  }

  bool WriteConf(const std::string& key, const std::string& value)
  {
    std::string err;

    if (!WriteAppConfValue(mModel.confPath, key, value, err))
    {
      Fail(key, err);
      return false;
    }

    if (key == "period")
      mModel.period = static_cast<unsigned int>(std::strtoul(value.c_str(), nullptr, 10));

    return true;
  }

  void Fail(const std::string& key, const std::string& err)
  {
    GetUI()->ShowMessageBox(("Le reglage n'a pas pu etre ecrit dans " + mModel.confPath + " (" + key + ") : " + err)
                              .c_str(),
                            "VoLum", EMsgBoxType::kMB_OK);
  }

  AudioPanelModel mModel;
  ProbeFn mProbe = AlsaProbeDevice;
  PanelLayout mLayout;
  std::string mStatus;
  int mExpanded = -1;
  float mMouseX = -1.f, mMouseY = -1.f;
  IRECT mPanel;
  IRECT mTitleRect, mCloseRect, mStatusRect;

  static constexpr float kMargin = 24.f;
  static constexpr float kPadding = 20.f;
  static constexpr float kPanelMaxWidth = 460.f;
  static constexpr float kPanelRadius = 6.f;
  static constexpr float kTitleHeight = 24.f;
  static constexpr float kTitleGap = 12.f;
  static constexpr float kRowHeight = 34.f;
  static constexpr float kRowGap = 8.f;
  static constexpr float kRowRadius = 4.f;
  static constexpr float kLabelWidth = 90.f;
  static constexpr float kValueWidth = 240.f;
  static constexpr float kChoiceHeight = 26.f;
  static constexpr float kChoiceIndent = 14.f;
  static constexpr float kMoreHeight = 18.f;
  static constexpr float kStatusHeight = 30.f;
  static constexpr float kCloseSize = 22.f;

  const IColor kOverlayColor = IColor(110, 0, 0, 0);
  const IColor kPanelColor = IColor(242, 250, 250, 250);
  const IColor kBorderColor = IColor(255, 90, 90, 90);
  const IColor kRowColor = IColor(255, 228, 228, 228);
  const IColor kRowHoverColor = IColor(255, 205, 205, 205);
  const IColor kChoiceColor = IColor(255, 240, 240, 240);
  const IColor kChoiceHoverColor = IColor(255, 210, 210, 210);
  const IColor kCloseColor = IColor(255, 228, 228, 228);
  const IColor kCloseHoverColor = IColor(255, 200, 200, 200);

  IText kTitleText = IText(17.f, COLOR_BLACK, nullptr, EAlign::Near, EVAlign::Middle);
  IText kRowText = IText(14.f, COLOR_BLACK, nullptr, EAlign::Near, EVAlign::Middle);
  IText kValueText = IText(13.f, IColor(255, 60, 60, 60), nullptr, EAlign::Far, EVAlign::Middle);
  IText kChoiceText = IText(13.f, COLOR_BLACK, nullptr, EAlign::Near, EVAlign::Middle);
  IText kMoreText = IText(11.f, IColor(255, 110, 110, 110), nullptr, EAlign::Near, EVAlign::Middle);
  IText kStatusText = IText(12.f, IColor(255, 20, 110, 90), nullptr, EAlign::Near, EVAlign::Middle);
  IText kCloseText = IText(14.f, COLOR_BLACK, nullptr, EAlign::Center, EVAlign::Middle);
};

} // namespace volumlinuxaudio
