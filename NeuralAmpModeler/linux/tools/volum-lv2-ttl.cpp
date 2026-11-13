/*
 ==============================================================================

 The LV2 bundle's data files (issue #9). A .lv2 bundle is a folder: the wrapper
 module, a manifest.ttl that lists the plug-in, and a VoLum.ttl that describes
 it - name, and one port block per port in the same order the wrapper connects
 them: two audio in, two audio out, one control input per VST3 parameter, one
 atom port for MIDI in.

 The parameter list comes from the module itself (the same enumeration the
 wrapper and the check tool use), so the TTL cannot drift from the binary: a
 parameter added upstream shows up here on the next build, with the same
 symbols the wrapper answers to. Symbols come from wrapperParamSymbol, names
 are escaped the small way TTL needs.

 Usage
   volum-lv2-ttl <output-dir>

 The VST3 module is found the way the wrappers find it (VOLUM_VST3_MODULE and
 the standard folders), because the TTL is generated after the plug-in build.

 ==============================================================================
*/

#include "wrapper-support.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using namespace volumlinux;

/** The binary name inside the bundle, agreed with the CMake copy step. */
constexpr const char* kLv2BinaryName = "volum-lv2.so";

/** TTL string literal: the backslash and the quote are the two characters a
 *  parameter name can carry that would end the string early. */
std::string ttlEscape(const std::string& s)
{
  std::string out;

  for (const char c : s)
  {
    if (c == '\\' || c == '"')
      out += '\\';

    out += c;
  }

  return out;
}

void writeIfChanged(const std::filesystem::path& path, const std::string& content)
{
  std::error_code ec;
  std::string old;

  if (std::ifstream in(path, std::ios::binary); in)
  {
    old.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    in.close();
  }

  if (old == content)
    return;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);

  if (!out)
  {
    fprintf(stderr, "volum-lv2-ttl : %s inaccessible en ecriture\n", path.string().c_str());
    exit(2);
  }

  out << content;
}

std::string manifestTtl()
{
  std::string ttl;
  ttl += "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n";
  ttl += "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n";
  ttl += "\n";
  ttl += std::string("<") + kPluginUri + ">\n";
  ttl += "    a lv2:Plugin ;\n";
  ttl += "    rdfs:seeAlso <VoLum.ttl> .\n";
  return ttl;
}

std::string pluginTtl(const std::vector<WrapperParam>& params)
{
  std::string ttl;
  ttl += "@prefix atom: <http://lv2plug.in/ns/ext/atom#> .\n";
  ttl += "@prefix doap: <http://usefulinc.com/ns/doap#> .\n";
  ttl += "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n";
  ttl += "@prefix midi: <http://lv2plug.in/ns/ext/midi#> .\n";
  ttl += "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n";
  ttl += "@prefix urid: <http://lv2plug.in/ns/ext/urid#> .\n";
  ttl += "\n";
  ttl += std::string("<") + kPluginUri + ">\n";
  ttl += "    a lv2:Plugin, lv2:AudioPlugin ;\n";
  ttl += "    doap:name \"" + std::string(kPluginName) + "\" ;\n";
  ttl += "    doap:homepage <" + std::string(kPluginUri) + "> ;\n";
  ttl += "    doap:maintainer [\n";
  ttl += "        doap:name \"Lum\" ;\n";
  ttl += "        doap:homepage <" + std::string(kPluginUri) + ">\n";
  ttl += "    ] ;\n";
  ttl += "    lv2:binary <" + std::string(kLv2BinaryName) + "> ;\n";
  ttl += "    lv2:optionalFeature urid:map ;\n";
  ttl += "    lv2:minorVersion " + std::to_string(3) + " ;\n";
  ttl += "    lv2:microVersion " + std::to_string(1) + " ;\n";

  unsigned int index = 0;

  const auto port = [&](const char* types, const char* symbol, const char* name, double def) {
    ttl += std::string("    lv2:port [\n");
    ttl += "        a " + std::string(types) + " ;\n";
    ttl += "        lv2:index " + std::to_string(index) + " ;\n";
    ttl += "        lv2:symbol \"" + std::string(symbol) + "\" ;\n";
    ttl += "        lv2:name \"" + ttlEscape(name) + "\" ;\n";

    if (def >= 0.0)
      ttl += "        lv2:default " + std::to_string(def) + " ;\n"
             "        lv2:minimum 0.0 ;\n"
             "        lv2:maximum 1.0 ;\n";

    ttl += "    ] ;\n";
    ++index;
  };

  port("lv2:AudioPort, lv2:InputPort", "in_0", "Audio In 0", -1.0);
  port("lv2:AudioPort, lv2:InputPort", "in_1", "Audio In 1", -1.0);
  port("lv2:AudioPort, lv2:OutputPort", "out_0", "Audio Out 0", -1.0);
  port("lv2:AudioPort, lv2:OutputPort", "out_1", "Audio Out 1", -1.0);

  for (const WrapperParam& p : params)
    port("lv2:ControlPort, lv2:InputPort", p.symbol.c_str(), p.name.c_str(), p.defaultValue);

  ttl += "    lv2:port [\n";
  ttl += "        a atom:AtomPort, lv2:InputPort ;\n";
  ttl += "        lv2:index " + std::to_string(index) + " ;\n";
  ttl += "        lv2:symbol \"midi_in\" ;\n";
  ttl += "        lv2:name \"MIDI In\" ;\n";
  ttl += "        atom:bufferType atom:Sequence ;\n";
  ttl += "        atom:supports midi:MidiEvent ;\n";
  ttl += "    ] .\n";
  return ttl;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage : %s <dossier du paquet .lv2>\n", argv[0]);
    return 2;
  }

  const std::filesystem::path dir = argv[1];
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  std::string tried;
  const std::string module = locateVst3Module(tried);

  if (module.empty())
  {
    fprintf(stderr, "volum-lv2-ttl : module VST3 introuvable (cherche :%s)\n", tried.c_str());
    return 2;
  }

  Loaded loaded;
  LoadOptions opt;
  opt.listClasses = false;
  opt.quiet = true;

  const int status = loadPlugin(module, loaded, opt);

  if (status != kLoadOk)
  {
    fprintf(stderr, "volum-lv2-ttl : chargement du module impossible (code %d)\n", status);
    return status;
  }

  const std::vector<WrapperParam> params = enumerateWrapperParams(loaded);
  unloadPlugin(loaded);

  if (params.empty())
  {
    fprintf(stderr, "volum-lv2-ttl : le module n'expose aucun parametre\n");
    return 3;
  }

  writeIfChanged(dir / "manifest.ttl", manifestTtl());
  writeIfChanged(dir / "VoLum.ttl", pluginTtl(params));

  printf("volum-lv2-ttl : %s (manifest.ttl, VoLum.ttl, %zu parametres)\n", dir.string().c_str(), params.size());
  return 0;
}
