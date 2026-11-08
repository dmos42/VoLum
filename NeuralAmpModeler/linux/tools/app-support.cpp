#include "app-support.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace volumlinux
{
namespace fs = std::filesystem;

namespace
{

/** The folder this executable sits in. Empty when /proc is not there to tell us, in
 * which case the search starts at the per-user folders instead. */
fs::path executableDir()
{
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);

  if (ec || exe.empty())
    return {};

  return exe.parent_path();
}

/** $HOME, empty when it is not set (a service started without a user, say). */
std::string homeDir()
{
  if (const char* home = std::getenv("HOME"); home && *home)
    return home;

  return {};
}

/** Inside a bundle the module lives in Contents/<arch>/, and <arch> is written by the
 * build that produced it - x86_64-linux here, aarch64-linux on a Pi. It is looked for
 * rather than assumed, so a bundle copied from another machine is still found and a
 * multilib bundle gets the host's own folder by the order directory_iterator returns
 * them, which is why the module is matched by name below. */
std::string moduleInBundle(const fs::path& bundle)
{
  const std::string bundleName = bundle.filename().string();

  if (bundleName.empty())
    return {};

  // Steinberg's own rule is <bundle>.so next to <bundle>.vst3; hosts from the JUCE
  // family rebuild it without the .vst3 part, and our bundle carries both names.
  std::vector<std::string> names{bundleName + ".so"};

  if (bundleName.size() > 5 && bundleName.compare(bundleName.size() - 5, 5, ".vst3") == 0)
    names.push_back(bundleName.substr(0, bundleName.size() - 5) + ".so");

  std::error_code ec;
  const fs::path contents = bundle / "Contents";

  if (!fs::is_directory(contents, ec))
    return {};

  for (const fs::directory_entry& entry : fs::directory_iterator(contents, ec))
  {
    if (!entry.is_directory(ec))
      continue;

    for (const std::string& name : names)
    {
      const fs::path candidate = entry.path() / name;

      if (fs::is_regular_file(candidate, ec))
        return candidate.string();
    }
  }

  return {};
}

} // namespace

std::vector<std::string> appBundleSearchPath()
{
  std::vector<std::string> path;
  const fs::path exeDir = executableDir();

  // A build tree (build/linux/volum-app next to build/linux/VoLum.vst3) and a portable
  // folder that keeps the two side by side are the same answer seen from bin/.
  if (!exeDir.empty())
  {
    path.push_back((exeDir / "VoLum.vst3").string());
    path.push_back((exeDir.parent_path() / "VoLum.vst3").string());
  }

  const std::string home = homeDir();

  if (!home.empty())
  {
    // What `cmake --install build/linux` writes, and what a user who unzipped a bundle
    // into their home would produce.
    path.push_back((fs::path(home) / ".vst3" / "VoLum.vst3").string());

    // The XDG base spec says a relative $XDG_DATA_HOME is not valid, so only an
    // absolute one counts; the spec's own default is the rest of this line.
    fs::path data;

    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0] == '/')
      data = fs::path(xdg);
    else
      data = fs::path(home) / ".local" / "share";

    path.push_back((data / "vst3" / "VoLum.vst3").string());
  }

  // Steinberg's documented Linux locations, with the 64-bit spellings Fedora and friends
  // use alongside them.
  for (const char* prefix :
       {"/usr/local/lib", "/usr/lib", "/usr/local/lib64", "/usr/lib64", "/usr/local/share", "/usr/share"})
    path.push_back((fs::path(prefix) / "vst3" / "VoLum.vst3").string());

  return path;
}

std::string findAppBundleModule()
{
  for (const std::string& bundle : appBundleSearchPath())
  {
    const std::string module = moduleInBundle(fs::path(bundle));

    if (!module.empty())
      return module;
  }

  return {};
}

} // namespace volumlinux
