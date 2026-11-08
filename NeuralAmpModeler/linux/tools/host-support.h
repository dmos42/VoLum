/*
 ==============================================================================

 Shared plumbing for the two small Linux hosts in this folder:

   volum-vst3-probe  headless: proves the port loads, enumerates and makes sound.
   volum-live        ALSA capture/playback: runs the plug-in without a DAW.

 A host owes a plug-in three things before any audio moves: a component handler
 (the channel the editor uses to report edits and size requests), a plug frame,
 and - for an editor - a window to be embedded in. This header holds those, plus
 the dlopen / factory / instantiate walk, so each tool only carries what is
 actually different about it.

 Threading note: the window here is our own X11 connection. The plug-in's editor
 runs on a connection and a thread of its own (iPlug 2's Linux backend starts it
 in IGraphicsLinux::Open), so a host may keep its own thread busy with audio as
 long as it answers X11 on ours.

 ==============================================================================
*/

#pragma once

#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vsttypes.h"

// X11 comes last: its bare typedefs (Window, Display, Status) must not shadow
// anything the SDK brought in.
#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace volumlinux
{

namespace sdk = ::Steinberg;
namespace vst = ::Steinberg::Vst;

// XEmbed, from the freedesktop spec (no X11 header defines these).
constexpr int kXEmbedEmbeddedNotify = 0;

/** Exit codes shared by the hosts, so a script can tell the failures apart. */
enum LoadStatus
{
  kLoadOk = 0,
  kLoadCannotOpen = 2,
  kLoadNoFactory = 3,
  kLoadNoInstance = 4
};

inline std::string toAscii(const vst::TChar* s, int maxLen = 128)
{
  std::string out;

  for (int i = 0; i < maxLen && s[i]; ++i)
    out += (s[i] < 0x80) ? static_cast<char>(s[i]) : '?';

  return out;
}

inline std::string cidToString(const Steinberg::TUID cid)
{
  char buf[3 * 16 + 1] = {0};
  int n = 0;

  for (int i = 0; i < 16 && cid[i]; ++i)
    n += std::snprintf(buf + n, sizeof(buf) - n, n ? " %02x" : "%02x", static_cast<unsigned char>(cid[i]));

  return std::string(buf, n);
}

/** Accepts both "--flag" and "--opt=value" and "--opt value" on the command line.
 * Returns the value (and consumes the next argv entry) or null when the option was
 * not the one asked for. */
inline const char* optionValue(const std::string& arg, const char* name, int& i, int argc, char** argv)
{
  const size_t len = std::strlen(name);

  if (arg.size() > len && arg[len] == '=' && arg.rfind(name, 0) == 0)
    return argv[i] + len + 1;

  if (arg == name && i + 1 < argc)
    return argv[++i];

  return nullptr;
}

/** Host side of the plug-in/UI handshake.
 * In this SDK revision IComponentHandler2 does not derive from IComponentHandler:
 * they are two separate interfaces, so one object answers for both and hands out the
 * matching sub-object on request. */
class HostComponentHandler : public vst::IComponentHandler, public vst::IComponentHandler2
{
public:
  // IComponentHandler
  sdk::tresult PLUGIN_API beginEdit(vst::ParamID) override { return sdk::kResultOk; }
  sdk::tresult PLUGIN_API performEdit(vst::ParamID, vst::ParamValue) override { return sdk::kResultOk; }
  sdk::tresult PLUGIN_API endEdit(vst::ParamID) override { return sdk::kResultOk; }

  sdk::tresult PLUGIN_API restartComponent(sdk::int32 flags) override
  {
    printf("  [hote] restartComponent(0x%x)\n", flags);
    return sdk::kResultOk;
  }

  // IComponentHandler2
  sdk::tresult PLUGIN_API setDirty(sdk::TBool state) override
  {
    printf("  [hote] setDirty(%d)\n", state ? 1 : 0);
    return sdk::kResultOk;
  }

  sdk::tresult PLUGIN_API requestOpenEditor(sdk::FIDString name) override
  {
    printf("  [hote] requestOpenEditor(%s)\n", name ? name : "?");
    return sdk::kResultOk;
  }

  sdk::tresult PLUGIN_API startGroupEdit() override { return sdk::kResultOk; }
  sdk::tresult PLUGIN_API finishGroupEdit() override { return sdk::kResultOk; }

  sdk::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override
  {
    if (!obj)
      return sdk::kInvalidArgument;

    *obj = nullptr;

    if (sdk::FUnknownPrivate::iidEqual(iid, vst::IComponentHandler2::iid))
      *obj = static_cast<vst::IComponentHandler2*>(this);
    else if (sdk::FUnknownPrivate::iidEqual(iid, vst::IComponentHandler::iid))
      *obj = static_cast<vst::IComponentHandler*>(this);
    else if (sdk::FUnknownPrivate::iidEqual(iid, sdk::FUnknown::iid))
      *obj = static_cast<vst::IComponentHandler*>(this);
    else
      return sdk::kNoInterface;

    return sdk::kResultOk;
  }

  sdk::uint32 PLUGIN_API addRef() override { return 1; }
  sdk::uint32 PLUGIN_API release() override { return 1; }
};

/** The plug-in asks for this when its editor wants a different size. */
class HostFrame : public sdk::IPlugFrame
{
public:
  sdk::tresult PLUGIN_API resizeView(sdk::IPlugView* view, sdk::ViewRect* newSize) override
  {
    if (!view || !newSize)
      return sdk::kInvalidArgument;

    printf("  [hote] resizeView demande : %d x %d\n", newSize->right - newSize->left, newSize->bottom - newSize->top);

    // A real host would resize its window here, then confirm the new geometry.
    const sdk::tresult r = view->onSize(newSize);
    printf("  [hote] onSize -> %d\n", int(r));

    return sdk::kResultOk;
  }

  sdk::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override
  {
    if (!obj)
      return sdk::kInvalidArgument;

    *obj = nullptr;

    if (sdk::FUnknownPrivate::iidEqual(iid, sdk::IPlugFrame::iid))
      *obj = static_cast<sdk::IPlugFrame*>(this);
    else if (sdk::FUnknownPrivate::iidEqual(iid, sdk::FUnknown::iid))
      *obj = static_cast<sdk::IPlugFrame*>(this);
    else
      return sdk::kNoInterface;

    return sdk::kResultOk;
  }

  sdk::uint32 PLUGIN_API addRef() override { return 1; }
  sdk::uint32 PLUGIN_API release() override { return 1; }
};

/** A plain X11 top-level window to host the plug-in view in, plus the socket half of
 * XEmbed: when the plug-in reparents its window in here we confirm the embedding.
 * Closing the window - ours or the plug-in's - is reported through closed(), which is
 * how volum-live knows the user is done. */
class HostWindow
{
public:
  bool open(const char* title, int width, int height)
  {
    mDisplay = XOpenDisplay(nullptr);

    if (!mDisplay)
    {
      fprintf(stderr, "  [hote] XOpenDisplay a echoue (DISPLAY=%s)\n", getenv("DISPLAY") ? getenv("DISPLAY") : "vide");
      return false;
    }

    const int screen = DefaultScreen(mDisplay);

    XSetWindowAttributes attrs;
    std::memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixel = 0xff202020;
    attrs.event_mask = StructureNotifyMask | SubstructureNotifyMask | ExposureMask;

    mWindow =
      XCreateWindow(mDisplay, RootWindow(mDisplay, screen), 80, 60, width, height, 0, DefaultDepth(mDisplay, screen),
                    InputOutput, DefaultVisual(mDisplay, screen), CWBackPixel | CWEventMask, &attrs);

    XStoreName(mDisplay, mWindow, title);

    mWmDelete = XInternAtom(mDisplay, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(mDisplay, mWindow, &mWmDelete, 1);

    mXEmbedAtom = XInternAtom(mDisplay, "_XEMBED", False);

    XMapWindow(mDisplay, mWindow);
    XSync(mDisplay, False);

    printf("  [hote] fenetre 0x%lx (%d x %d) - titre \"%s\"\n", mWindow, width, height, title);

    return true;
  }

  /** Answer whatever is queued on our connection, without waiting. */
  void pumpEvents()
  {
    if (!mDisplay)
      return;

    while (XPending(mDisplay))
    {
      XEvent ev;
      XNextEvent(mDisplay, &ev);
      handleEvent(ev);
    }
  }

  /** Keep the connection alive for a while, answering embed requests as they land.
   * Returns false as soon as the window is gone, true if the time is up. */
  bool pump(double seconds)
  {
    if (!mDisplay)
      return false;

    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);

    while (std::chrono::steady_clock::now() < end)
    {
      pumpEvents();

      if (mClosed)
        return false;

      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return !mClosed;
  }

  void* parentHandle() const { return reinterpret_cast<void*>(static_cast<uintptr_t>(mWindow)); }
  unsigned long childID() const { return mChild; }
  bool embedded() const { return mEmbedded; }
  bool closed() const { return mClosed; }

  /** Give the embedded editor a new size from the host side, which is what a DAW does with the
   * container it put the plug-in in: the plug-in receives a ConfigureNotify for a window whose
   * size it does not own. */
  bool resizeChild(unsigned int width, unsigned int height)
  {
    if (!mDisplay || !mChild)
      return false;

    XResizeWindow(mDisplay, mChild, width, height);
    XSync(mDisplay, False);

    printf("  [hote] fenetre du plugin portee a %u x %u\n", width, height);

    return true;
  }

  void close()
  {
    if (mDisplay)
      XCloseDisplay(mDisplay);

    mDisplay = nullptr;
  }

private:
  void handleEvent(XEvent& ev)
  {
    if (ev.type == ReparentNotify && ev.xreparent.parent == mWindow && !mEmbedded)
    {
      mEmbedded = true;
      mChild = ev.xreparent.window; // ReparentNotify: window == the reparented child

      XEvent msg;
      std::memset(&msg, 0, sizeof(msg));
      msg.xclient.type = ClientMessage;
      msg.xclient.display = mDisplay;
      msg.xclient.window = mChild;
      msg.xclient.message_type = mXEmbedAtom;
      msg.xclient.format = 32;
      msg.xclient.data.l[0] = static_cast<long>(CurrentTime);
      msg.xclient.data.l[1] = kXEmbedEmbeddedNotify;
      msg.xclient.data.l[2] = 0;
      msg.xclient.data.l[3] = static_cast<long>(mWindow);

      XSendEvent(mDisplay, mChild, False, StructureNotifyMask, &msg);
      XSync(mDisplay, False);

      printf("  [hote] fenetre du plugin enfoncee : 0x%lx (XEmbed confirme)\n", mChild);
      return;
    }

    // A parent of None means the plug-in took its window back out, i.e. it is gone.
    if (ev.type == ReparentNotify && ev.xreparent.window == mChild && ev.xreparent.parent != mWindow)
      mClosed = true;
    else if (ev.type == DestroyNotify
             && (ev.xdestroywindow.window == mWindow || (mChild && ev.xdestroywindow.window == mChild)))
      mClosed = true;
    else if (ev.type == UnmapNotify && (ev.xunmap.window == mWindow || (mChild && ev.xunmap.window == mChild)))
      mClosed = true;
    else if (ev.type == ClientMessage && static_cast<Atom>(ev.xclient.data.l[0]) == mWmDelete)
      mClosed = true;
  }

  Display* mDisplay = nullptr;
  ::Window mWindow = 0;
  Atom mXEmbedAtom = 0;
  Atom mWmDelete = 0;
  ::Window mChild = 0;
  bool mEmbedded = false;
  bool mClosed = false;
};

struct LoadOptions
{
  bool listClasses = true; // print every exported class
  int listParams = 0; // how many parameter lines to print (0: none)
};

/** Everything a host has to keep alive while it uses the plug-in. */
struct Loaded
{
  void* lib = nullptr;
  sdk::IPluginFactory* factory = nullptr;
  vst::IComponent* component = nullptr;
  vst::IAudioProcessor* processor = nullptr;
  vst::IEditController* controller = nullptr;
  std::string className;
  std::string cid;
  int inChannels = 1;
  int outChannels = 1;
  HostComponentHandler handler; // must outlive the controller and the editor
  HostFrame frame;
};

/** Open the module, find a usable class, instantiate and initialize it, activate the
 * audio buses and report the channel counts. Returns a LoadStatus. */
inline int loadPlugin(const std::string& path, Loaded& L, const LoadOptions& opt)
{
  L.lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);

  if (!L.lib)
  {
    fprintf(stderr, "dlopen : %s\n", dlerror());
    return kLoadCannotOpen;
  }

  using InitFn = sdk::tresult (*)();
  using FactoryFn = sdk::IPluginFactory* (*)();

  InitFn initModule = reinterpret_cast<InitFn>(dlsym(L.lib, "_Z10InitModulev"));

  if (!initModule)
    initModule = reinterpret_cast<InitFn>(dlsym(L.lib, "InitModule"));

  FactoryFn getFactory = reinterpret_cast<FactoryFn>(dlsym(L.lib, "GetPluginFactory"));

  if (!getFactory)
    getFactory = reinterpret_cast<FactoryFn>(dlsym(L.lib, "_Z17GetPluginFactoryv"));

  if (!getFactory)
  {
    fprintf(stderr, "GetPluginFactory introuvable dans le module\n");
    return kLoadNoFactory;
  }

  if (initModule)
    printf("InitModule() -> %d\n", int(initModule()));
  else
    printf("InitModule() absent (le module n'en a peut-etre pas besoin)\n");

  L.factory = getFactory();

  if (!L.factory)
  {
    fprintf(stderr, "GetPluginFactory a renvoie null\n");
    return kLoadNoFactory;
  }

  sdk::PFactoryInfo factoryInfo;

  if (L.factory->getFactoryInfo(&factoryInfo) == sdk::kResultOk)
    printf("fabrique : vendor=%s url=%s\n", factoryInfo.vendor, factoryInfo.url);

  const sdk::int32 count = L.factory->countClasses();
  printf("classes exportees : %d\n", int(count));

  for (sdk::int32 i = 0; i < count; ++i)
  {
    sdk::PClassInfo classInfo;

    if (L.factory->getClassInfo(i, &classInfo) != sdk::kResultOk)
      continue;

    if (opt.listClasses)
      printf("  [%d] categorie=%s nom=%s cid=%s\n", int(i), classInfo.category, classInfo.name,
             cidToString(classInfo.cid).c_str());

    if (L.component)
      continue; // keep the first usable class; VoLum exports its single VST3 class

    void* obj = nullptr;

    if (L.factory->createInstance(classInfo.cid, vst::IComponent::iid, &obj) != sdk::kResultOk || !obj)
      continue;

    vst::IComponent* candidate = static_cast<vst::IComponent*>(obj);

    // This SDK revision passes the context (the instance itself) to initialize().
    if (candidate->initialize(candidate) != sdk::kResultOk)
    {
      printf("  initialize() a echoue pour cette classe\n");
      candidate->release();
      continue;
    }

    void* procObj = nullptr;
    candidate->queryInterface(vst::IAudioProcessor::iid, &procObj);

    void* ctrlObj = nullptr;
    candidate->queryInterface(vst::IEditController::iid, &ctrlObj);

    if (!procObj && !ctrlObj)
    {
      candidate->terminate();
      candidate->release();
      continue;
    }

    L.component = candidate;
    L.processor = static_cast<vst::IAudioProcessor*>(procObj);
    L.controller = static_cast<vst::IEditController*>(ctrlObj);
    // Unlike the UTF-16 names carried by bus and parameter structures, the factory
    // class name is plain ASCII in this SDK revision.
    L.className = std::string(classInfo.name, strnlen(classInfo.name, sizeof(classInfo.name)));
    L.cid = cidToString(classInfo.cid);
  }

  if (!L.component)
  {
    fprintf(stderr, "aucune classe utilisable (IComponent + initialize)\n");
    return kLoadNoInstance;
  }

  printf("instance : %s | processor=%s controller=%s\n", L.className.c_str(), L.processor ? "oui" : "non",
         L.controller ? "oui" : "non");

  if (L.controller)
    L.controller->setComponentHandler(static_cast<vst::IComponentHandler*>(&L.handler));
  else
    printf("  pas de controller : pas de poignee d'evenements possible\n");

  // Buses, so we know what to feed the plug-in.
  for (int dir = 0; dir < 2; ++dir)
  {
    const vst::BusDirection direction = dir == 0 ? vst::kInput : vst::kOutput;
    const int busCount = L.component->getBusCount(vst::MediaTypes::kAudio, direction);

    for (int b = 0; b < busCount; ++b)
    {
      vst::BusInfo busInfo;

      if (L.component->getBusInfo(vst::MediaTypes::kAudio, direction, b, busInfo) == sdk::kResultOk)
      {
        printf("  bus %s %d : \"%s\", %d canaux\n", dir == 0 ? "entree" : "sortie", b, toAscii(busInfo.name).c_str(),
               busInfo.channelCount);

        if (b == 0)
          (dir == 0 ? L.inChannels : L.outChannels) = busInfo.channelCount > 0 ? busInfo.channelCount : 1;

        L.component->activateBus(vst::MediaTypes::kAudio, direction, b, 1);
      }
    }
  }

  if (L.controller && opt.listParams > 0)
  {
    const sdk::int32 paramCount = L.controller->getParameterCount();
    printf("parametres : %d\n", int(paramCount));

    for (sdk::int32 p = 0; p < paramCount && p < opt.listParams; ++p)
    {
      vst::ParameterInfo info;

      if (L.controller->getParameterInfo(p, info) == sdk::kResultOk)
        printf("  [%2d] id=%u \"%s\" defaut=%.3f pas=%d\n", int(p), info.id, toAscii(info.title).c_str(),
               info.defaultNormalizedValue, info.stepCount);
    }
  }

  return kLoadOk;
}

/** --param IDX:VAL pairs, applied before the engine starts. */
inline void applyParams(Loaded& L, const std::vector<std::pair<int, double>>& sets)
{
  if (!L.controller || sets.empty())
    return;

  const sdk::int32 paramCount = L.controller->getParameterCount();

  for (const auto& set : sets)
  {
    if (set.first < 0 || set.first >= paramCount)
    {
      fprintf(stderr, "index de parametre hors plage : %d\n", set.first);
      continue;
    }

    vst::ParameterInfo info;

    if (L.controller->getParameterInfo(set.first, info) == sdk::kResultOk)
    {
      L.controller->setParamNormalized(info.id, set.second);
      printf("  -> \"%s\" = %.3f (relu %.3f)\n", toAscii(info.title).c_str(), set.second,
             L.controller->getParamNormalized(info.id));
    }
  }
}

/** Bring the plug-in up at a given rate and block size. Returns false when the plug-in
 * refused the setup (a model that needs another rate would show up here). */
inline bool startProcessing(Loaded& L, double rate, int blockLength)
{
  vst::ProcessSetup setup;
  setup.processMode = vst::kRealtime;
  setup.symbolicSampleSize = vst::kSample32;
  setup.maxSamplesPerBlock = blockLength;
  setup.sampleRate = rate;

  const sdk::tresult setupResult = L.processor->setupProcessing(setup);
  printf("setupProcessing -> %d (%d Hz, %d echantillons par bloc)\n", int(setupResult), int(rate), blockLength);

  if (setupResult != sdk::kResultOk)
    return false;

  L.processor->setProcessing(1);
  L.component->setActive(1);

  return true;
}

/** The editor, hosted in a window of its own on our X11 connection. */
class EditorSession
{
public:
  /** 0 = created and attached; 1 = no controller; 2 = no view; 3 = no window. */
  int open(Loaded& L, const char* title)
  {
    if (!L.controller)
      return 1;

    mView = L.controller->createView(vst::ViewType::kEditor);

    if (!mView)
    {
      fprintf(stderr, "createView(\"editor\") a renvoie null\n");
      return 2;
    }

    sdk::ViewRect rect;
    mView->getSize(&rect);
    printf("vue creee : %d x %d\n", rect.right - rect.left, rect.bottom - rect.top);

    printf("isPlatformTypeSupported(\"x11\") -> %d\n", int(mView->isPlatformTypeSupported("x11")));

    mView->setFrame(&L.frame);

    if (!mWindow.open(title, rect.right - rect.left > 0 ? rect.right - rect.left : 900,
                      rect.bottom - rect.top > 0 ? rect.bottom - rect.top : 600))
    {
      mView->release();
      mView = nullptr;
      return 3;
    }

    const auto parentID = static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mWindow.parentHandle()));
    const sdk::tresult attachResult = mView->attached(mWindow.parentHandle(), sdk::kPlatformTypeX11EmbedWindowID);
    printf("attached(0x%lx, X11EmbedWindowID) -> %d\n", parentID, int(attachResult));

    return 0;
  }

  bool pump(double seconds) { return mWindow.pump(seconds); }
  void pumpEvents() { mWindow.pumpEvents(); }
  bool closed() const { return mWindow.closed(); }
  bool embedded() const { return mWindow.embedded(); }
  bool resizeEmbedded(unsigned int width, unsigned int height) { return mWindow.resizeChild(width, height); }

  void sizeReport(int afterSeconds)
  {
    if (!mView)
      return;

    sdk::ViewRect after;
    mView->getSize(&after);
    printf("taille de la vue apres %d s : %d x %d\n", afterSeconds, after.right - after.left, after.bottom - after.top);
    printf("fenetre attendue du plugin : %s\n", mWindow.embedded() ? "oui" : "non");
  }

  void close()
  {
    if (mView)
    {
      mView->removed();
      mWindow.close();
      mView->release();
      mView = nullptr;
    }
  }

private:
  sdk::IPlugView* mView = nullptr;
  HostWindow mWindow;
};

/** Tear everything down in the order the SDK wants. */
inline void unloadPlugin(Loaded& L)
{
  if (L.controller)
    L.controller->setComponentHandler(nullptr);

  if (L.component)
  {
    L.component->terminate();
    L.component->release();
  }

  if (L.lib)
  {
    if (auto deinit = reinterpret_cast<void (*)()>(dlsym(L.lib, "_Z12DeinitModulev")))
      deinit();

    dlclose(L.lib);
    L.lib = nullptr;
  }
}

} // namespace volumlinux
