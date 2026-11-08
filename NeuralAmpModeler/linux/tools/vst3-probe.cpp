/*
 ==============================================================================

 volum-linux: a minimal headless VST3 host, used to check the Linux port of VoLum.

 It answers the two questions a real DAW would otherwise have to answer:
   1. does the module load, enumerate its classes and instantiate cleanly?
   2. does audio actually come out of it, and does an editor window appear?

 Usage
   volum-vst3-probe <path>/VoLum.vst3/Contents/x86_64-linux/<module>.so
                    [--seconds N]      audio run length (default 2)
                    [--rate R]         sample rate (default 48000)
                    [--param IDX:VAL]  set a normalized parameter before running
                    [--ui]             also open the editor inside a probe window
                    [--uiseconds N]    how long to keep the editor open (default 6)
                    [--ui-only]        skip the audio run

 Exit codes: 0 ok, 2 cannot load, 3 no factory/class, 4 could not instantiate,
 5 processing failed, 6 output stayed silent.

 ==============================================================================
*/

#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vsttypes.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

// X11 comes last: its bare typedefs (Window, Display, Status) must not shadow
// anything the SDK brought in.
#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace
{

// XEmbed, from the freedesktop spec (no X11 header defines these).
constexpr int kXEmbedEmbeddedNotify = 0;

std::string toAscii(const TChar* s, int maxLen = 128)
{
  std::string out;

  for (int i = 0; i < maxLen && s[i]; ++i)
    out += (s[i] < 0x80) ? static_cast<char>(s[i]) : '?';

  return out;
}

std::string cidToString(const TUID cid)
{
  char buf[3 * 16 + 1] = {0};
  int n = 0;

  for (int i = 0; i < 16 && cid[i]; ++i)
    n += std::snprintf(buf + n, sizeof(buf) - n, n ? " %02x" : "%02x", static_cast<unsigned char>(cid[i]));

  return std::string(buf, n);
}

/** Host side of the plug-in/UI handshake.
 * In this SDK revision IComponentHandler2 does not derive from IComponentHandler:
 * they are two separate interfaces, so one object answers for both and hands out the
 * matching sub-object on request. */
class ProbeComponentHandler : public IComponentHandler, public IComponentHandler2
{
public:
  // IComponentHandler
  tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
  tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
  tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }

  tresult PLUGIN_API restartComponent(int32 flags) override
  {
    printf("  [hote] restartComponent(0x%x)\n", flags);
    return kResultOk;
  }

  // IComponentHandler2
  tresult PLUGIN_API setDirty(TBool state) override
  {
    printf("  [hote] setDirty(%d)\n", state ? 1 : 0);
    return kResultOk;
  }

  tresult PLUGIN_API requestOpenEditor(FIDString name) override
  {
    printf("  [hote] requestOpenEditor(%s)\n", name ? name : "?");
    return kResultOk;
  }

  tresult PLUGIN_API startGroupEdit() override { return kResultOk; }
  tresult PLUGIN_API finishGroupEdit() override { return kResultOk; }

  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
  {
    if (!obj)
      return kInvalidArgument;

    *obj = nullptr;

    if (FUnknownPrivate::iidEqual(iid, IComponentHandler2::iid))
      *obj = static_cast<IComponentHandler2*>(this);
    else if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid))
      *obj = static_cast<IComponentHandler*>(this);
    else if (FUnknownPrivate::iidEqual(iid, FUnknown::iid))
      *obj = static_cast<IComponentHandler*>(this);
    else
      return kNoInterface;

    return kResultOk;
  }

  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
};

/** The plug-in asks for this when its editor wants a different size. */
class ProbeFrame : public IPlugFrame
{
public:
  tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override
  {
    if (!view || !newSize)
      return kInvalidArgument;

    printf("  [hote] resizeView demande : %d x %d\n", newSize->right - newSize->left,
           newSize->bottom - newSize->top);

    // A real host would resize its window here, then confirm the new geometry.
    const tresult r = view->onSize(newSize);
    printf("  [hote] onSize -> %d\n", int(r));

    return kResultOk;
  }

  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
  {
    if (!obj)
      return kInvalidArgument;

    *obj = nullptr;

    if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid))
      *obj = static_cast<IPlugFrame*>(this);
    else if (FUnknownPrivate::iidEqual(iid, FUnknown::iid))
      *obj = static_cast<IPlugFrame*>(this);
    else
      return kNoInterface;

    return kResultOk;
  }

  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
};

/** A plain X11 top-level window to host the plug-in view in, plus the socket half of
 * XEmbed: when the plug-in reparents its window in here we confirm the embedding. */
class ProbeWindow
{
public:
  bool open(int width, int height)
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

    mWindow = XCreateWindow(mDisplay, RootWindow(mDisplay, screen), 80, 60, width, height, 0,
                            DefaultDepth(mDisplay, screen), InputOutput, DefaultVisual(mDisplay, screen),
                            CWBackPixel | CWEventMask, &attrs);

    XStoreName(mDisplay, mWindow, "VoLum Linux probe host");

    Atom wmDelete = XInternAtom(mDisplay, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(mDisplay, mWindow, &wmDelete, 1);

    mXEmbedAtom = XInternAtom(mDisplay, "_XEMBED", False);

    XMapWindow(mDisplay, mWindow);
    XSync(mDisplay, False);

    printf("  [hote] fenetre 0x%lx (%d x %d) - titre \"VoLum Linux probe host\"\n", mWindow, width, height);

    return true;
  }

  /** Keep our own connection alive for a while, and answer any embed request that lands. */
  void pump(double seconds)
  {
    if (!mDisplay)
      return;

    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);

    while (std::chrono::steady_clock::now() < end)
    {
      while (XPending(mDisplay))
      {
        XEvent ev;
        XNextEvent(mDisplay, &ev);

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
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void* parentHandle() const { return reinterpret_cast<void*>(static_cast<uintptr_t>(mWindow)); }
  unsigned long childID() const { return mChild; }
  bool embedded() const { return mEmbedded; }

  void close()
  {
    if (mDisplay)
      XCloseDisplay(mDisplay);
  }

private:
  Display* mDisplay = nullptr;
  ::Window mWindow = 0;
  Atom mXEmbedAtom = 0;
  ::Window mChild = 0;
  bool mEmbedded = false;
};

struct Options
{
  std::string path;
  double seconds = 2.0;
  double rate = 48000.0;
  bool audio = true;
  bool ui = false;
  int uiSeconds = 6;
  int maxParams = 250;
  std::vector<std::pair<int, double>> params; // parameter index -> normalized value
};

bool parseArgs(int argc, char** argv, Options& opt)
{
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];

    if (arg.rfind("--", 0) != 0)
    {
      opt.path = arg;
      continue;
    }

    // Accept both "--seconds=2" and "--seconds 2".
    auto valueAt = [&](const char* prefix) -> const char* {
      const size_t len = std::strlen(prefix);

      if (arg.size() > len && arg[len] == '=')
        return argv[i] + len + 1;

      if (arg == prefix && i + 1 < argc)
        return argv[++i];

      return nullptr;
    };

    if (arg == "--ui")
      opt.ui = true;
    else if (arg == "--ui-only")
    {
      opt.ui = true;
      opt.audio = false;
    }
    else if (const char* v = valueAt("--seconds"))
      opt.seconds = std::atof(v);
    else if (const char* v = valueAt("--rate"))
      opt.rate = std::atof(v);
    else if (const char* v = valueAt("--uiseconds"))
      opt.uiSeconds = std::atoi(v);
    else if (const char* v = valueAt("--param"))
    {
      const int idx = std::atoi(v);
      const char* colon = std::strchr(v, ':');

      if (!colon)
      {
        fprintf(stderr, "--param attend IDX:VAL\n");
        return false;
      }

      opt.params.emplace_back(idx, std::atof(colon + 1));
    }
    else
    {
      fprintf(stderr, "option inconnue : %s\n", arg.c_str());
      return false;
    }
  }

  if (opt.path.empty())
  {
    fprintf(stderr, "usage : %s <module .so> [--seconds N] [--rate R] [--param IDX:VAL] [--ui]\n", argv[0]);
    return false;
  }

  return true;
}

} // namespace

int main(int argc, char** argv)
{
  Options opt;

  if (!parseArgs(argc, argv, opt))
    return 2;

  printf("=== volum-vst3-probe : %s ===\n", opt.path.c_str());

  void* lib = dlopen(opt.path.c_str(), RTLD_NOW | RTLD_LOCAL);

  if (!lib)
  {
    fprintf(stderr, "dlopen : %s\n", dlerror());
    return 2;
  }

  using InitFn = tresult (*)();
  using FactoryFn = IPluginFactory* (*)();

  InitFn initModule = reinterpret_cast<InitFn>(dlsym(lib, "_Z10InitModulev"));

  if (!initModule)
    initModule = reinterpret_cast<InitFn>(dlsym(lib, "InitModule"));

  FactoryFn getFactory = reinterpret_cast<FactoryFn>(dlsym(lib, "GetPluginFactory"));

  if (!getFactory)
    getFactory = reinterpret_cast<FactoryFn>(dlsym(lib, "_Z17GetPluginFactoryv"));

  if (!getFactory)
  {
    fprintf(stderr, "GetPluginFactory introuvable dans le module\n");
    return 3;
  }

  if (initModule)
    printf("InitModule() -> %d\n", int(initModule()));
  else
    printf("InitModule() absent (le module n'en a peut-etre pas besoin)\n");

  IPluginFactory* factory = getFactory();

  if (!factory)
  {
    fprintf(stderr, "GetPluginFactory a renvoie null\n");
    return 3;
  }

  PFactoryInfo factoryInfo;

  if (factory->getFactoryInfo(&factoryInfo) == kResultOk)
    printf("fabrique : vendor=%s url=%s\n", factoryInfo.vendor, factoryInfo.url);

  const int32 count = factory->countClasses();
  printf("classes exportees : %d\n", int(count));

  IComponent* component = nullptr;
  IAudioProcessor* processor = nullptr;
  IEditController* controller = nullptr;

  for (int32 i = 0; i < count; ++i)
  {
    PClassInfo classInfo;

    if (factory->getClassInfo(i, &classInfo) != kResultOk)
      continue;

    printf("  [%d] categorie=%s nom=%s cid=%s\n", int(i), classInfo.category, classInfo.name,
           cidToString(classInfo.cid).c_str());

    if (component)
      continue; // keep the first usable class; VoLum exports its single VST3 class

    void* obj = nullptr;

    if (factory->createInstance(classInfo.cid, IComponent::iid, &obj) != kResultOk || !obj)
      continue;

    IComponent* candidate = static_cast<IComponent*>(obj);

    // This SDK revision passes the context (the instance itself) to initialize().
    if (candidate->initialize(candidate) != kResultOk)
    {
      printf("  initialize() a echoue pour cette classe\n");
      candidate->release();
      continue;
    }

    void* procObj = nullptr;
    candidate->queryInterface(IAudioProcessor::iid, &procObj);

    void* ctrlObj = nullptr;
    candidate->queryInterface(IEditController::iid, &ctrlObj);

    if (!procObj && !ctrlObj)
    {
      candidate->terminate();
      candidate->release();
      continue;
    }

    component = candidate;
    processor = static_cast<IAudioProcessor*>(procObj);
    controller = static_cast<IEditController*>(ctrlObj);
  }

  if (!component)
  {
    fprintf(stderr, "aucune classe utilisable (IComponent + initialize)\n");
    return 4;
  }

  printf("instance : processor=%s controller=%s\n", processor ? "oui" : "non", controller ? "oui" : "non");

  ProbeComponentHandler handler;
  ProbeFrame frame;

  if (controller)
    controller->setComponentHandler(static_cast<IComponentHandler*>(&handler));
  else
    printf("  pas de controller : pas de poignee d'evenements possible\n");

  // Buses, so we know what to feed the plug-in.
  int inChannels = 1, outChannels = 1;

  for (int dir = 0; dir < 2; ++dir)
  {
    const BusDirection direction = dir == 0 ? kInput : kOutput;
    const int busCount = component->getBusCount(MediaTypes::kAudio, direction);

    for (int b = 0; b < busCount; ++b)
    {
      BusInfo busInfo;

      if (component->getBusInfo(MediaTypes::kAudio, direction, b, busInfo) == kResultOk)
      {
        printf("  bus %s %d : \"%s\", %d canaux\n", dir == 0 ? "entree" : "sortie", b,
               toAscii(busInfo.name).c_str(), busInfo.channelCount);

        if (b == 0)
          (dir == 0 ? inChannels : outChannels) = busInfo.channelCount > 0 ? busInfo.channelCount : 1;

        component->activateBus(MediaTypes::kAudio, direction, b, 1);
      }
    }
  }

  if (controller)
  {
    const int32 paramCount = controller->getParameterCount();
    printf("parametres : %d\n", int(paramCount));

    for (int32 p = 0; p < paramCount && p < (opt.maxParams > 0 ? opt.maxParams : paramCount); ++p)
    {
      ParameterInfo info;

      if (controller->getParameterInfo(p, info) == kResultOk)
        printf("  [%2d] id=%u \"%s\" defaut=%.3f pas=%d\n", int(p), info.id, toAscii(info.title).c_str(),
               info.defaultNormalizedValue, info.stepCount);
    }

    for (const auto& set : opt.params)
    {
      if (set.first < 0 || set.first >= paramCount)
      {
        fprintf(stderr, "index de parametre hors plage : %d\n", set.first);
        continue;
      }

      ParameterInfo info;

      if (controller->getParameterInfo(set.first, info) == kResultOk)
      {
        controller->setParamNormalized(info.id, set.second);
        printf("  -> \"%s\" = %.3f (relu %.3f)\n", toAscii(info.title).c_str(), set.second,
               controller->getParamNormalized(info.id));
      }
    }
  }

  int exitCode = 0;

  // MARK: audio
  if (opt.audio && processor)
  {
    ProcessSetup setup;
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = opt.rate;

    const tresult setupResult = processor->setupProcessing(setup);
    printf("setupProcessing -> %d (%d Hz, 512 echantillons)\n", int(setupResult), int(opt.rate));

    if (setupResult != kResultOk)
      exitCode = 5;

    processor->setProcessing(1);
    component->setActive(1);

    const int blockLength = 512;
    const int blocks = static_cast<int>(opt.seconds * opt.rate / blockLength) + 1;

    std::vector<float> inStorage(static_cast<size_t>(inChannels) * blockLength, 0.f);
    std::vector<float> outStorage(static_cast<size_t>(outChannels) * blockLength, 0.f);
    std::vector<Sample32*> inPointers(static_cast<size_t>(inChannels), nullptr);
    std::vector<Sample32*> outPointers(static_cast<size_t>(outChannels), nullptr);

    for (int c = 0; c < inChannels; ++c)
      inPointers[c] = inStorage.data() + static_cast<size_t>(c) * blockLength;

    for (int c = 0; c < outChannels; ++c)
      outPointers[c] = outStorage.data() + static_cast<size_t>(c) * blockLength;

    AudioBusBuffers inputs;
    inputs.numChannels = inChannels;
    inputs.silenceFlags = 0;
    inputs.channelBuffers32 = inPointers.data();

    AudioBusBuffers outputs;
    outputs.numChannels = outChannels;
    outputs.silenceFlags = 0;
    outputs.channelBuffers32 = outPointers.data();

    ProcessData data;
    data.processMode = Vst::kRealtime;
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = blockLength;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inputs;
    data.outputs = &outputs;

    double peakIn = 0.0, peakOut = 0.0, sumOut = 0.0, sumIn = 0.0;
    long countedSamples = 0;
    long resultErrors = 0;
    double phase = 0.0;
    const double twoPi = 2.0 * 3.14159265358979312;
    const double phaseInc = twoPi * 440.0 / opt.rate;
    const int halfBlocks = blocks / 2 > 0 ? blocks / 2 : 1;
    // A real host feeds blocks as the audio device needs them. Without pacing the whole
    // run finishes in a few tens of milliseconds and the plug-in's 20 ms idle timer -
    // OnIdle(), where VoLum stages its amp model - never gets a single tick.
    const auto runStart = std::chrono::steady_clock::now();

    for (int b = 0; b < blocks; ++b)
    {
      // A 440 Hz test tone: loud for half the run, quiet for the other half, so the
      // difference between "pass-through" and "modelled / compressed" is visible.
      const double amp = b < halfBlocks ? 0.2 : 0.05;

      for (int i = 0; i < blockLength; ++i)
      {
        phase += phaseInc;

        if (phase > twoPi)
          phase -= twoPi;

        const double tone = std::sin(phase) * amp;

        for (int c = 0; c < inChannels; ++c)
          inPointers[c][i] = static_cast<float>(c == 0 ? tone : 0.0);
      }

      const tresult r = processor->process(data);

      if (r != kResultOk)
        ++resultErrors;

      for (int i = 0; i < blockLength; ++i)
      {
        const double inSample = inPointers[0][i];
        const double outSample = outPointers[0][i];

        if (b > 0) // skip the very first block: model loading and ramps land there
        {
          peakIn = peakIn > std::abs(inSample) ? peakIn : std::abs(inSample);
          peakOut = peakOut > std::abs(outSample) ? peakOut : std::abs(outSample);
          sumIn += inSample * inSample;
          sumOut += outSample * outSample;
          ++countedSamples;
        }
      }

      const auto due = runStart + std::chrono::microseconds (static_cast<long long> (
        (static_cast<double> (b + 1) * blockLength * 1e6) / opt.rate));

      if (const auto now = std::chrono::steady_clock::now (); now < due)
        std::this_thread::sleep_until (due);
    }

    const double rmsIn = countedSamples ? std::sqrt(sumIn / countedSamples) : 0.0;
    const double rmsOut = countedSamples ? std::sqrt(sumOut / countedSamples) : 0.0;

    printf("traitement : %d blocs de %d, erreurs=%ld\n", blocks, blockLength, resultErrors);
    printf("entree : crete=%.4f rms=%.4f | sortie : crete=%.4f rms=%.4f\n", peakIn, rmsIn, peakOut, rmsOut);

    if (resultErrors)
      exitCode = 5;

    if (peakOut < 1e-6)
    {
      printf("verdict : SILENCE en sortie\n");
      exitCode = exitCode ? exitCode : 6;
    }
    else
      printf("verdict : sortie sonore\n");

    processor->setProcessing(0);
    component->setActive(0);
  }
  else if (opt.audio)
  {
    printf("pas de traitement audio : la classe n'implemente pas IAudioProcessor\n");
    exitCode = 5;
  }

  // MARK: editor
  if (opt.ui && controller)
  {
    IPlugView* view = controller->createView(ViewType::kEditor);

    if (!view)
    {
      fprintf(stderr, "createView(\"editor\") a renvoie null\n");
      exitCode = exitCode ? exitCode : 6;
    }
    else
    {
      ViewRect rect;
      view->getSize(&rect);
      printf("vue creee : %d x %d\n", rect.right - rect.left, rect.bottom - rect.top);

      const tresult typeOk = view->isPlatformTypeSupported("x11");
      printf("isPlatformTypeSupported(\"x11\") -> %d\n", int(typeOk));

      view->setFrame(&frame);

      ProbeWindow window;

      if (window.open(rect.right - rect.left > 0 ? rect.right - rect.left : 900,
                      rect.bottom - rect.top > 0 ? rect.bottom - rect.top : 600))
      {
        const auto parentID = static_cast<unsigned long>(reinterpret_cast<uintptr_t>(window.parentHandle()));
        const tresult attachResult = view->attached(window.parentHandle(), kPlatformTypeX11EmbedWindowID);
        printf("attached(0x%lx, X11EmbedWindowID) -> %d\n", parentID, int(attachResult));

        window.pump(opt.uiSeconds);

        ViewRect after;
        view->getSize(&after);
        printf("taille de la vue apres %d s : %d x %d\n", opt.uiSeconds, after.right - after.left,
               after.bottom - after.top);
        printf("fenetre attendue du plugin : %s\n", window.embedded() ? "oui" : "non");

        view->removed();
        window.close();
      }
      else
      {
        printf("impossible d'ouvrir une fenetre X11 pour l'editeur\n");
      }

      view->release();
    }
  }

  if (controller)
    controller->setComponentHandler(nullptr);

  component->terminate();
  component->release();

  if (auto deinit = reinterpret_cast<void (*)()>(dlsym(lib, "_Z12DeinitModulev")))
    deinit();

  dlclose(lib);

  printf("=== probe termine (code %d) ===\n", exitCode);
  return exitCode;
}
