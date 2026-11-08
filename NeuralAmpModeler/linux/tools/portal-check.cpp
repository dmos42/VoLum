/**
 * @file portal-check.cpp
 * @brief Dev tool: drives the same portal calls the plug-in makes.
 *
 * It is not shipped and no host needs it: it exists because a file chooser needs
 * fingers to answer, and the protocol half of this feature has to be testable
 * without them. Two modes:
 *
 *   --probe   reach the real portal and read a property. No window, no dialog:
 *             proves the session bus, the connection and the bus name.
 *   --open / --save / --dir / --color
 *             the real request, against whatever portal the session has. Point the
 *             tool at test/fake-portal.py on a throwaway bus and the "dialog" is a
 *             scripted answer, which is what CI can check.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gio/gio.h>

#include "IGraphicsLinuxPortal.h"

namespace
{

constexpr char kPortalName[] = "org.freedesktop.portal.Desktop";
constexpr char kObjectPath[] = "/org/freedesktop/portal/desktop";

/** The portal answers a method it does not have, which means the bus, the name and
 *  the marshalling all worked without anyone having to click anything. */
int probe()
{
  GError* error = nullptr;
  GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!conn)
  {
    std::printf("bus: %s\n", error && error->message ? error->message : "unreachable");
    return 2;
  }

  GVariant* reply =
    g_dbus_connection_call_sync(conn, kPortalName, kObjectPath, "org.freedesktop.DBus.Properties", "Get",
                                g_variant_new("(ss)", "org.freedesktop.portal.FileChooser", "version"),
                                G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  if (!reply)
  {
    std::printf("portal: %s\n", error && error->message ? error->message : "no reply");
    g_object_unref(conn);
    return 2;
  }

  GVariant* value = nullptr;
  g_variant_get(reply, "(v)", &value);
  guint32 version = 0;
  g_variant_get(value, "u", &version);
  g_variant_unref(value);
  g_variant_unref(reply);
  g_object_unref(conn);

  std::printf("portal: file chooser version %u\n", version);
  return version > 0 ? 0 : 2;
}

// What the plug-in's file browser passes for a capture library: iPlug spells the
// types as extensions, and the portal client is what turns them into patterns.
constexpr char kDefaultExtensions[] = "nam wav";

void printPaths(const std::vector<std::string>& paths)
{
  for (const std::string& path : paths)
    std::printf("picked: %s\n", path.c_str());
}

} // namespace

int main(int argc, char** argv)
{
  const char* mode = argc > 1 ? argv[1] : "--probe";
  const char* parent = nullptr;
  const char* initialPath = nullptr;
  const char* initialName = nullptr;
  const char* extensions = kDefaultExtensions;

  for (int i = 2; i < argc; ++i)
  {
    if (!std::strcmp(argv[i], "--parent") && i + 1 < argc)
      parent = argv[++i];
    else if (!std::strcmp(argv[i], "--path") && i + 1 < argc)
      initialPath = argv[++i];
    else if (!std::strcmp(argv[i], "--name") && i + 1 < argc)
      initialName = argv[++i];
    else if (!std::strcmp(argv[i], "--ext") && i + 1 < argc)
      extensions = argv[++i];
  }

  if (!std::strcmp(mode, "--probe"))
    return probe();

  std::vector<std::string> paths;
  std::string error;
  iplug::EPortalResult result = iplug::EPortalResult::Failed;

  if (!std::strcmp(mode, "--color"))
  {
    double rgb[3] = {0.0, 0.0, 0.0};
    result = iplug::PortalPickColor(rgb, parent, "Color", error);
    if (result == iplug::EPortalResult::Chosen)
      std::printf("color: %.3f %.3f %.3f\n", rgb[0], rgb[1], rgb[2]);
  }
  else if (!std::strcmp(mode, "--open") || !std::strcmp(mode, "--dir") || !std::strcmp(mode, "--save"))
  {
    const bool directory = !std::strcmp(mode, "--dir");
    const bool save = !std::strcmp(mode, "--save");

    iplug::PortalFileRequest request;
    request.mMethod = save ? "SaveFile" : "OpenFile";
    request.mTitle = directory ? "Choose a folder" : (save ? "Save pack" : "Open captures");
    request.mParentWindow = parent ? parent : "";
    request.mInitialPath = initialPath ? initialPath : "";
    request.mInitialName = initialName ? initialName : "";
    // A folder chooser has no file types to offer, so it gets no filters.
    if (!directory)
      request.mFilters = iplug::PortalFiltersFromExtensions(extensions);
    request.mMultiple = !directory && !save;
    request.mDirectory = directory;

    result = iplug::PortalChooseFiles(request, paths, error);
    printPaths(paths);
  }
  else
  {
    std::printf(
      "usage: volum-portal-check [--probe|--open|--save|--dir|--color] [--parent x11:0x...] [--path DOSSIER] [--name "
      "NOM] [--ext \"nam wav\"]\n");
    return 2;
  }

  if (result == iplug::EPortalResult::Chosen)
    return 0;
  if (result == iplug::EPortalResult::Cancelled)
  {
    std::printf("cancelled\n");
    return 3;
  }

  std::printf("failed: %s\n", error.c_str());
  return 2;
}
