/*
 ==============================================================================

 Where a standalone VoLum finds its plug-in.

 A DAW is told the path, or looks the bundle up through Steinberg's own rules. A host
 that starts from the desktop has to work that out for itself, and the answer depends
 on how VoLum got there: a build tree, a folder somebody copied onto a USB stick, an
 install under ~/.vst3, a system install. So the places are listed from the one that
 explains this executable to the one that explains the machine - the executable's own
 folder first, because that is the single answer that is true both for a build tree and
 for a portable folder - and the list is printed when nothing matched instead of saying
 "not found" and leaving the user guessing where to put the bundle.

 ==============================================================================
*/

#pragma once

#include <string>
#include <vector>

namespace volumlinux
{

/** The folders that could be holding a VoLum.vst3 bundle, the ones closest to this
 * executable first. Found through /proc/self/exe, which is how "the folder I was
 * installed into" is answerable without asking the user. */
std::vector<std::string> appBundleSearchPath();

/** The module to load: the first bundle in that list that really holds one. Empty when
 * none does, and the caller shows appBundleSearchPath as the reason. */
std::string findAppBundleModule();

} // namespace volumlinux
