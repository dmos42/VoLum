#include "options-support.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace volumlinux
{

/** Only these keys take a value, so a bare flag never swallows the next argument. */
bool keyWantsValue(const std::string& key)
{
  static const char* const valued[] = {"module", "out",     "in",   "in-file", "out-file", "config", "rate",
                                       "period", "periods", "gain", "seconds", "meter",    "param"};

  for (const char* candidate : valued)
    if (key == candidate)
      return true;

  return false;
}

bool applyOption(const std::string& key, const char* value, Options& opt, std::string& err)
{
  auto wantNumber = [&](double& out) {
    if (!value)
    {
      err = "--" + key + " attend une valeur";
      return false;
    }
    out = std::atof(value);
    return true;
  };
  auto wantText = [&](std::string& out) {
    if (!value)
    {
      err = "--" + key + " attend une valeur";
      return false;
    }
    out = value;
    return true;
  };

  if (key == "module")
    return wantText(opt.module);

  // --out is what tells us the user wants the sound live: without it, a WAV in and a
  // WAV out is a plain offline conversion.
  if (key == "out")
  {
    if (!wantText(opt.outDevice))
      return false;
    opt.outRequested = true;
    return true;
  }
  if (key == "in")
    return wantText(opt.inDevice);
  if (key == "in-file")
    return wantText(opt.inFile);
  if (key == "out-file")
    return wantText(opt.outFile);
  if (key == "config")
    return wantText(opt.configFile);
  if (key == "rate")
  {
    double v;
    if (!wantNumber(v))
      return false;
    opt.rate = v;
    return true;
  }
  if (key == "period")
  {
    double v;
    if (!wantNumber(v))
      return false;
    opt.period = unsigned(v);
    return true;
  }
  if (key == "periods")
  {
    double v;
    if (!wantNumber(v))
      return false;
    opt.periods = unsigned(v);
    return true;
  }
  if (key == "gain")
    return wantNumber(opt.gain);
  if (key == "seconds")
    return wantNumber(opt.seconds);
  if (key == "meter")
    return wantNumber(opt.meterEvery);

  if (key == "param")
  {
    if (!value)
    {
      err = "--param attend IDX:VAL";
      return false;
    }
    const char* colon = std::strchr(value, ':');
    if (!colon)
    {
      err = "--param attend IDX:VAL";
      return false;
    }
    opt.params.emplace_back(std::atoi(value), std::atof(colon + 1));
    return true;
  }

  if (key == "no-ui" || key == "ui")
  {
    opt.ui = key == "ui";
    return true;
  }
  if (key == "no-in")
  {
    opt.noIn = true;
    return true;
  }
  if (key == "list")
  {
    opt.listDevices = true;
    return true;
  }
  if (key == "list-params")
  {
    opt.listParams = true;
    return true;
  }
  if (key == "check")
  {
    opt.check = true;
    return true;
  }

  err = "option inconnue : " + key;
  return false;
}

bool loadConfigFile(const std::string& path, Options& opt, std::string& err)
{
  FILE* f = std::fopen(path.c_str(), "r");

  if (!f)
    return true; // no config file is not an error

  char line[512];
  int lineNo = 0;

  while (std::fgets(line, sizeof(line), f))
  {
    ++lineNo;
    std::string s(line);

    const size_t hash = s.find('#');
    if (hash != std::string::npos)
      s.erase(hash);

    const size_t eq = s.find('=');
    if (eq == std::string::npos)
      continue;

    auto trim = [](std::string& t) {
      while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front())))
        t.erase(t.begin());
      while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
        t.pop_back();
    };

    std::string key = s.substr(0, eq);
    std::string value = s.substr(eq + 1);
    trim(key);
    trim(value);

    if (key.empty())
      continue;

    // A flag key with no value means "on"; everything else needs its value.
    std::string where = path + " ligne " + std::to_string(lineNo) + " : ";
    if (!applyOption(key, value.empty() ? nullptr : value.c_str(), opt, err))
    {
      err = where + err;
      std::fclose(f);
      return false;
    }
  }

  std::fclose(f);
  printf("reglages lus dans %s\n", path.c_str());
  return true;
}

bool parseArgs(int argc, char** argv, Options& opt, void (*printUsage)(const char*), const char* defaultConfigName)
{
  // The config file is read first so the command line always wins.
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h")
    {
      printUsage(argv[0]);
      return false;
    }

    if (arg == "--config")
    {
      if (i + 1 >= argc)
      {
        fprintf(stderr, "--config attend un chemin\n");
        return false;
      }
      opt.configFile = argv[++i];
    }
  }

  if (opt.configFile.empty())
  {
    if (const char* home = getenv("HOME"))
      opt.configFile = std::string(home) + "/.config/VoLum/" + defaultConfigName;
  }

  std::string err;

  if (!opt.configFile.empty() && !loadConfigFile(opt.configFile, opt, err))
  {
    fprintf(stderr, "%s\n", err.c_str());
    return false;
  }

  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];

    if (arg.rfind("--", 0) != 0)
    {
      opt.module = arg; // the module may still be given as a bare argument
      continue;
    }

    std::string key = arg.substr(2);
    const char* value = nullptr;
    const size_t eq = key.find('=');

    if (eq != std::string::npos)
    {
      value = argv[i] + 2 + eq + 1; // --opt=value
      key.erase(eq);
    }
    else if (keyWantsValue(key) && i + 1 < argc && argv[i + 1][0] != '-')
    {
      value = argv[++i]; // --opt value
    }

    if (!applyOption(key, value, opt, err))
    {
      fprintf(stderr, "%s\n", err.c_str());
      return false;
    }
  }

  if (opt.module.empty())
  {
    if (const char* home = getenv("HOME"))
      opt.module = std::string(home) + "/.vst3/VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so";
    else
      opt.module = "~/.vst3/VoLum.vst3/Contents/x86_64-linux/VoLum.vst3.so";
  }

  // Capture comes from the same device as playback unless --in said otherwise; --no-in
  // asks for no capture at all, and the plug-in is then fed silence or a WAV.
  if (opt.noIn)
    opt.inDevice.clear();
  else if (opt.inDevice.empty())
    opt.inDevice = opt.outDevice;

  if (opt.period == 0)
    opt.period = 64;
  if (opt.periods == 0)
    opt.periods = 2;

  return true;
}
} // namespace volumlinux
