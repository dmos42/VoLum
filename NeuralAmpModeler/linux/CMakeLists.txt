# Linux build of the VoLum VST3 (X11 + GLX, NanoVG/GL2).
#
# Upstream VoLum ships Windows and macOS binaries only; this file is the Linux
# equivalent of NeuralAmpModeler/projects/NeuralAmpModeler-vst3.vcxproj. It lives in
# its own folder so it never interferes with the Windows/macOS build scripts.
#
#   cmake -S NeuralAmpModeler/linux -B build/linux -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/linux
#   -> build/linux/VoLum.vst3/Contents/x86_64-linux/volum-vst3.so
#
# Requires the VST3 SDK submodule (iPlug2/Dependencies/IPlug/VST3_SDK) and the
# Fedora packages listed in README.md next to this file.

cmake_minimum_required(VERSION 3.20)

project(VoLumLinux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Ubuntu turns on -D_FORTIFY_SOURCE=3 in the compiler for every -O2 build; Fedora does not.
# That difference hides port bugs on the dev machine: iPlug 2 passed 32 as the snprintf size
# where 32-i bytes were left, and a fortified build aborts the process inside the plug-in
# constructor (glibc: "*** buffer overflow detected ***"), which killed the CI probe while
# every local Fedora build was green. Turn this on locally to compile the way CI does.
option(VOLUM_FORTIFY "Compile with -D_FORTIFY_SOURCE=3, matching Ubuntu CI" OFF)
if(VOLUM_FORTIFY)
  add_compile_options(-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
endif()

# gcc 11 and 12 reject the member alias that renames the LanczosResampler template
# (AudioDSPTools' ResamplingContainer.h: "declaration changes meaning"), which newer
# gcc accepts; the AppImage job builds with gcc 12, so let it through there.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  add_compile_options(-fpermissive)
endif()

set(PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(REPO_ROOT "${PROJECT_DIR}/.." ABSOLUTE)
set(IPLUG2_ROOT "${REPO_ROOT}/iPlug2")
set(DEPS_PATH "${IPLUG2_ROOT}/Dependencies")
set(VST3_SDK "${DEPS_PATH}/IPlug/VST3_SDK")

if(NOT EXISTS "${VST3_SDK}/public.sdk/source/main/linuxmain.cpp")
  message(FATAL_ERROR
    "The VST3 SDK is not in place at ${VST3_SDK}. Run "
    "iPlug2/Dependencies/IPlug/download-vst3-sdk.sh first.")
endif()

find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
# Optional: volum-live is the only tool that talks to ALSA; the plug-in itself does not.
find_package(ALSA)

# Optional: GLib's D-Bus client, which is how the plug-in reaches the XDG desktop
# portal - the file, folder and colour dialogs of the desktop itself. A session that has
# a portal already runs GLib; a headless build box might not, and there the plug-in is
# built with no dialogs (as it was until now) rather than not built at all.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(VOLUM_GIO IMPORTED_TARGET gio-2.0)
endif()
if(NOT VOLUM_GIO_FOUND)
  message(WARNING
    "gio-2.0 introuvable : le plug-in sera construit sans boites de dialogue natives "
    "(fichier, dossier, couleur). Installe glib2-devel (Fedora) ou libglib2.0-dev "
    "(Ubuntu/Debian), puis relance le configure.")
endif()

# libatomic. Two 64-bit atomics in the code we link resolve to __atomic_store/__atomic_load
# libcalls rather than inline instructions. Fedora's GCC adds -latomic to the link line by
# itself, so the bundle links clean here; GCC on the Ubuntu runners does not, the .so is still
# produced (a shared library tolerates undefined symbols) and the failure only surfaces when a
# host dlopen()s it: "undefined symbol: __atomic_store".
#
# Link it explicitly whenever the toolchain provides the library. Probing for the atomic call
# itself is no good - the compiler resolves a known-size one inline and reports success.
# -latomic is resolved from the compiler's own directories, which is why this cannot use
# find_library: on Fedora only libatomic.so.1 is in the loader path, the .so the linker needs
# sits under /usr/lib/gcc, and the search would return NOTFOUND.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_QUIET TRUE)
set(CMAKE_REQUIRED_LIBRARIES atomic)
check_cxx_source_compiles("int main() { return 0; }" VOLUM_HAS_LIBATOMIC)
set(CMAKE_REQUIRED_LIBRARIES)
if(VOLUM_HAS_LIBATOMIC)
  set(VOLUM_LIBATOMIC atomic)
endif()

# Steinberg layout inside the bundle: <name>.vst3/Contents/<arch>/module.so
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(VOLUM_ARCH_DIR "aarch64-linux")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(VOLUM_ARCH_DIR "i386-linux")
else()
  set(VOLUM_ARCH_DIR "x86_64-linux")
endif()

set(VOLUM_BUNDLE_DIR "${CMAKE_BINARY_DIR}/VoLum.vst3")
set(VOLUM_MODULE_DIR "${VOLUM_BUNDLE_DIR}/Contents/${VOLUM_ARCH_DIR}")
set(VOLUM_RESOURCES_DIR "${VOLUM_BUNDLE_DIR}/Contents/Resources")

# ---------------------------------------------------------------------------
# Sources: the same list as the Windows VST3 project, with the platform layer
# swapped for IGraphicsLinux.cpp and the SDK module entry for Linux.
# ---------------------------------------------------------------------------

set(VOLUM_PLUGIN_SOURCES
  "${PROJECT_DIR}/NeuralAmpModeler.cpp"
  "${PROJECT_DIR}/ToneStack.cpp"
  "${PROJECT_DIR}/VoLumHttpGet.cpp"
)

set(VOLUM_DSP_SOURCES
  "${REPO_ROOT}/AudioDSPTools/dsp/Delay.cpp"
  "${REPO_ROOT}/AudioDSPTools/dsp/dsp.cpp"
  "${REPO_ROOT}/AudioDSPTools/dsp/ImpulseResponse.cpp"
  "${REPO_ROOT}/AudioDSPTools/dsp/NoiseGate.cpp"
  "${REPO_ROOT}/AudioDSPTools/dsp/RecursiveLinearFilter.cpp"
  "${REPO_ROOT}/AudioDSPTools/dsp/Reverb.cpp"
  "${REPO_ROOT}/AudioDSPTools/dsp/wav.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/activations.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/container.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/conv1d.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/convnet.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/dsp.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/get_dsp.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/lstm.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/ring_buffer.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/wavenet/a2_fast.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/wavenet/model.cpp"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM/wavenet/slimmable.cpp"
)

set(IPLUG_SOURCES
  "${IPLUG2_ROOT}/IPlug/IPlugAPIBase.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugParameter.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugPaths.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugPluginBase.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugProcessor.cpp"
  "${IPLUG2_ROOT}/IPlug/IPlugTimer.cpp"
  "${IPLUG2_ROOT}/IPlug/VST3/IPlugVST3.cpp"
  "${IPLUG2_ROOT}/IPlug/VST3/IPlugVST3_ProcessorBase.cpp"
)

set(IGRAPHICS_SOURCES
  "${IPLUG2_ROOT}/IGraphics/IControl.cpp"
  "${IPLUG2_ROOT}/IGraphics/IGraphics.cpp"
  "${IPLUG2_ROOT}/IGraphics/IGraphicsEditorDelegate.cpp"
  "${IPLUG2_ROOT}/IGraphics/Controls/IControls.cpp"
  "${IPLUG2_ROOT}/IGraphics/Controls/IPopupMenuControl.cpp"
  "${IPLUG2_ROOT}/IGraphics/Controls/ITextEntryControl.cpp"
  "${IPLUG2_ROOT}/IGraphics/Drawing/IGraphicsNanoVG.cpp"
  "${IPLUG2_ROOT}/IGraphics/Platforms/IGraphicsLinux.cpp"
  "${DEPS_PATH}/IGraphics/NanoVG/src/nanovg.c"
)

# The dialogs the platform layer shows are the desktop's own, reached over the session
# bus (see IGraphicsLinuxPortal.h). GLib is what speaks D-Bus here: every desktop session
# has its runtime, while a build box only has it if the developer package is installed.
if(VOLUM_GIO_FOUND)
  list(APPEND IGRAPHICS_SOURCES "${IPLUG2_ROOT}/IGraphics/Platforms/IGraphicsLinuxPortal.cpp")
endif()

set(VST3_SDK_SOURCES
  "${VST3_SDK}/base/source/baseiids.cpp"
  "${VST3_SDK}/base/source/fbuffer.cpp"
  "${VST3_SDK}/base/source/fdebug.cpp"
  "${VST3_SDK}/base/source/fdynlib.cpp"
  "${VST3_SDK}/base/source/fobject.cpp"
  "${VST3_SDK}/base/source/fstreamer.cpp"
  "${VST3_SDK}/base/source/fstring.cpp"
  "${VST3_SDK}/base/source/timer.cpp"
  "${VST3_SDK}/base/source/updatehandler.cpp"
  "${VST3_SDK}/base/thread/source/fcondition.cpp"
  "${VST3_SDK}/base/thread/source/flock.cpp"
  "${VST3_SDK}/pluginterfaces/base/conststringtable.cpp"
  "${VST3_SDK}/pluginterfaces/base/coreiids.cpp"
  "${VST3_SDK}/pluginterfaces/base/funknown.cpp"
  "${VST3_SDK}/pluginterfaces/base/ustring.cpp"
  "${VST3_SDK}/public.sdk/source/common/commoniids.cpp"
  "${VST3_SDK}/public.sdk/source/common/memorystream.cpp"
  "${VST3_SDK}/public.sdk/source/common/pluginview.cpp"
  "${VST3_SDK}/public.sdk/source/main/linuxmain.cpp"
  "${VST3_SDK}/public.sdk/source/main/pluginfactory.cpp"
  "${VST3_SDK}/public.sdk/source/vst/vstaudioeffect.cpp"
  "${VST3_SDK}/public.sdk/source/vst/vstbus.cpp"
  "${VST3_SDK}/public.sdk/source/vst/vstcomponentbase.cpp"
  "${VST3_SDK}/public.sdk/source/vst/vstcomponent.cpp"
  "${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp"
  "${VST3_SDK}/public.sdk/source/vst/vstparameters.cpp"
  "${VST3_SDK}/public.sdk/source/vst/vstsinglecomponenteffect.cpp"
)

add_library(volum-vst3 SHARED
  ${VOLUM_PLUGIN_SOURCES}
  ${VOLUM_DSP_SOURCES}
  ${IPLUG_SOURCES}
  ${IGRAPHICS_SOURCES}
  ${VST3_SDK_SOURCES}
)

# Un paquet VST3 contient Contents/<arch>/<nom>.vst3.so, comme les paquets Windows et macOS
# amont : le module garde le nom du paquet, pas celui de la cible CMake.
set_target_properties(volum-vst3 PROPERTIES
  PREFIX ""
  OUTPUT_NAME "VoLum.vst3"
  SUFFIX ".so"
  LIBRARY_OUTPUT_DIRECTORY "${VOLUM_MODULE_DIR}"
  LINK_FLAGS "-Wl,-rpath,$ORIGIN"
)

# Deux noms pour le module, parce que les hôtes ne cherchent pas pareil. Steinberg demande
# que le module porte le nom du paquet (VoLum.vst3.so), mais un hôte JUCE (Carla, Qtractor,
# et leurs dérivés) reconstruit le chemin « Contents/<arch>/<paquet sans .vst3>.so » et ne
# trouve que VoLum.so. Sans ce lien, Carla répond « No plugins found » alors que le paquet
# est valide. Le lien est relatif, donc le paquet reste déplacable.
add_custom_command(TARGET volum-vst3 POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E create_symlink "VoLum.vst3.so" "VoLum.so"
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:volum-vst3>"
  COMMENT "lien VoLum.so pour les hôtes de famille JUCE")

target_include_directories(volum-vst3 PRIVATE
  "${PROJECT_DIR}"
  "${PROJECT_DIR}/third_party/signalsmith"
  "${REPO_ROOT}/eigen"
  "${REPO_ROOT}/AudioDSPTools"
  "${REPO_ROOT}/NeuralAmpModelerCore"
  "${REPO_ROOT}/NeuralAmpModelerCore/NAM"
  "${IPLUG2_ROOT}/IPlug"
  "${IPLUG2_ROOT}/IPlug/VST3"
  "${IPLUG2_ROOT}/IPlug/Extras"
  "${IPLUG2_ROOT}/IGraphics"
  "${IPLUG2_ROOT}/IGraphics/Controls"
  "${IPLUG2_ROOT}/IGraphics/Drawing"
  "${IPLUG2_ROOT}/IGraphics/Platforms"
  "${IPLUG2_ROOT}/IGraphics/Extras"
  "${IPLUG2_ROOT}/WDL"
  "${DEPS_PATH}/Extras"
  "${DEPS_PATH}/Extras/nlohmann"
  "${DEPS_PATH}/IGraphics/NanoVG/src"
  "${DEPS_PATH}/IGraphics/NanoSVG/src"
  "${DEPS_PATH}/IGraphics/STB"
  "${VST3_SDK}"
)

target_compile_definitions(volum-vst3 PRIVATE
  VST3_API
  IPLUG_EDITOR=1
  IPLUG_DSP=1
  IGRAPHICS_NANOVG
  IGRAPHICS_GL2
  GRAYED_ALPHA=0.5f
  NAM_ENABLE_A2_FAST
)

# The VST3 SDK refuses to compile unless RELEASE, NDEBUG, DEVELOPMENT or _DEBUG is defined, and
# its DEVELOPMENT code path - the one _DEBUG selects - does not build on Linux. Tell the SDK it
# is release code in every configuration; iPlug2 switches its own logging off NDEBUG, which is
# left undefined for a Debug build, so DBGMSG still prints there.
target_compile_definitions(volum-vst3 PRIVATE RELEASE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

# WDL's headers use free/malloc/realloc without including <cstdlib>; the test build
# works the same way (see NeuralAmpModeler/tests/CMakeLists.txt).
target_compile_options(volum-vst3 PRIVATE
  # -include cstdlib is a C++ header: keep it off the C translation units (nanovg.c)
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang>>:-include;cstdlib;-Wno-deprecated-declarations>
)

target_link_libraries(volum-vst3 PRIVATE
  OpenGL::GL
  X11::X11
  Threads::Threads
  ${CMAKE_DL_LIBS}
  ${VOLUM_LIBATOMIC}
)

# VOLUM_HAVE_PORTAL is what IGraphicsLinux.cpp looks at: with it the pickers ask the
# desktop, without it they say so in the log and change nothing.
if(VOLUM_GIO_FOUND)
  target_compile_definitions(volum-vst3 PRIVATE VOLUM_HAVE_PORTAL=1)
  target_link_libraries(volum-vst3 PRIVATE PkgConfig::VOLUM_GIO)
endif()

# ---------------------------------------------------------------------------
# Resources: fonts and images next to the module, so IGraphicsLinux finds them
# through the bundle's Resources folder. The shipped rig collection is copied
# too; the plug-in looks for it as VoLumRigs inside that folder.
# ---------------------------------------------------------------------------

option(VOLUM_COPY_RIGS "Copy the bundled VoLumRigs collection into the bundle" ON)

add_custom_command(TARGET volum-vst3 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${PROJECT_DIR}/resources/fonts" "${VOLUM_RESOURCES_DIR}/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${PROJECT_DIR}/resources/img" "${VOLUM_RESOURCES_DIR}/img"
  COMMENT "Copying fonts and images into ${VOLUM_RESOURCES_DIR}"
)

if(VOLUM_COPY_RIGS)
  add_custom_command(TARGET volum-vst3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${REPO_ROOT}/rigs" "${VOLUM_RESOURCES_DIR}/VoLumRigs"
    COMMENT "Copying the bundled rigs into the VST3 bundle"
  )
endif()

# Install into the per-user VST3 folder: cmake --install build/linux
include(GNUInstallDirs)
# Le hote VST3 cherche des paquets nommes <nom>.vst3 : sans barre oblique a la fin, CMake
# installe le dossier lui-meme (~/.vst3/VoLum.vst3/Contents/...) et non son contenu.
set(VOLUM_INSTALL_DIR "$ENV{HOME}/.vst3" CACHE PATH "Folder the VoLum.vst3 bundle is installed into")
install(DIRECTORY "${VOLUM_BUNDLE_DIR}"
  DESTINATION "${VOLUM_INSTALL_DIR}"
  USE_SOURCE_PERMISSIONS)

# Le standalone s'installe lui la ou un utilisateur regarde sans rien regler :
# ~/.local/bin (deja au PATH sur Fedora comme sur Ubuntu) pour le programme,
# ~/.local/share pour l'entree de menu et l'icone. Un packager qui veut un /usr/bin
# passe -DCMAKE_INSTALL_PREFIX=/usr, et -DVOLUM_INSTALL_DIR=/usr/lib/vst3 pour le
# paquet : rien d'autre ne bouge ici.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH
    "Prefix the standalone, its menu entry and its icon are installed under" FORCE)
endif()


# ---------------------------------------------------------------------------
# The small hosts in tools/. They need the same host-side plumbing
# (tools/host-support.h) and the same handful of SDK sources, so the list and the
# settings are shared and each tool adds only what is its own.
#
#   volum-vst3-probe : headless. dlopen, one instance, a test tone, the editor in
#                      a plain X11 window. Answers "does the port load and make
#                      sound?" with no DAW installed.
#   volum-live       : drives the plug-in itself - ALSA capture and playback plus
#                      the editor window - so a guitar can go through VoLum with no
#                      DAW in front of it, no JACK bridge, and one period of
#                      buffering instead of three.
#   volum-app        : the same engine as a stand-alone application, the Linux
#                      counterpart of the Windows and macOS VoLum app: it finds its
#                      own bundle, reads its own settings file, and stops when the
#                      window closes. Built from the files volum-live is built from,
#                      which is the point - two hosts, one block loop.
# ---------------------------------------------------------------------------

option(VOLUM_BUILD_PROBE "Build the small headless VST3 host used to check the port" ON)
option(VOLUM_BUILD_LIVE "Build volum-live, the ALSA host that runs the plug-in with no DAW" ON)
option(VOLUM_BUILD_APP "Build volum-app, the standalone VoLum window with no DAW" ON)

set(VOLUM_HOST_SDK_SOURCES
  "${VST3_SDK}/base/source/baseiids.cpp"
  "${VST3_SDK}/base/source/fbuffer.cpp"
  "${VST3_SDK}/base/source/fdebug.cpp"
  "${VST3_SDK}/base/source/fdynlib.cpp"
  "${VST3_SDK}/base/source/fobject.cpp"
  "${VST3_SDK}/base/source/fstreamer.cpp"
  "${VST3_SDK}/base/source/fstring.cpp"
  "${VST3_SDK}/base/source/timer.cpp"
  "${VST3_SDK}/base/source/updatehandler.cpp"
  "${VST3_SDK}/base/thread/source/fcondition.cpp"
  "${VST3_SDK}/base/thread/source/flock.cpp"
  "${VST3_SDK}/pluginterfaces/base/conststringtable.cpp"
  "${VST3_SDK}/pluginterfaces/base/coreiids.cpp"
  "${VST3_SDK}/pluginterfaces/base/funknown.cpp"
  "${VST3_SDK}/pluginterfaces/base/ustring.cpp"
  "${VST3_SDK}/public.sdk/source/common/commoniids.cpp"
  "${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp"
)

# What a host that puts sound through the plug-in is made of, once, instead of once
# per host: the ALSA streams, the WAV reader/writer the tests use, the option
# surface, and the block loop itself. volum-live already ran on this code; the
# standalone is supposed to hear the same guitar, so it gets the same code.
set(VOLUM_HOST_SUPPORT_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/tools/alsa-support.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tools/options-support.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tools/wav-support.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/tools/live-runner.cpp"
)

macro(volum_configure_host target source)
  add_executable(${target}
    "${CMAKE_CURRENT_LIST_DIR}/tools/${source}"
    ${VOLUM_HOST_SDK_SOURCES}
  )

  target_compile_definitions(${target} PRIVATE RELEASE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
  target_include_directories(${target} PRIVATE "${VST3_SDK}" "${CMAKE_CURRENT_LIST_DIR}/tools")
  target_link_libraries(${target} PRIVATE X11::X11 ${CMAKE_DL_LIBS} ${VOLUM_LIBATOMIC})
  target_compile_features(${target} PRIVATE cxx_std_17)

  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endmacro()

# A host that drives a sound card: the shared plumbing plus ALSA itself.
macro(volum_configure_alsa_host target source)
  volum_configure_host(${target} ${source})
  target_sources(${target} PRIVATE ${VOLUM_HOST_SUPPORT_SOURCES})
  target_include_directories(${target} PRIVATE ${ALSA_INCLUDE_DIRS})
  target_link_libraries(${target} PRIVATE ${ALSA_LIBRARIES})
endmacro()

if(VOLUM_BUILD_PROBE)
  volum_configure_host(volum-vst3-probe vst3-probe.cpp)
endif()

if(NOT ALSA_FOUND)
  if(VOLUM_BUILD_LIVE OR VOLUM_BUILD_APP)
    message(WARNING
      "alsa-lib introuvable : volum-live et volum-app ne seront pas construits. Installe alsa-lib-devel "
      "(Fedora) ou libasound2-dev (Ubuntu/Debian), puis relance le configure.")
  endif()
else()
  if(VOLUM_BUILD_LIVE)
    volum_configure_alsa_host(volum-live alsa-live.cpp)
  endif()

  # The standalone is volum-live's engine plus the one thing a host inside a DAW never has
  # to do: work out on its own which VoLum.vst3 it is running.
  if(VOLUM_BUILD_APP)
    volum_configure_alsa_host(volum-app volum-app.cpp)
    target_sources(volum-app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tools/app-support.cpp")
  endif()
endif()

# ---------------------------------------------------------------------------
# test/portal-test.bash drives volum-portal-check. A file chooser needs fingers to
# answer it, so the check answers it with test/fake-portal.py on a throwaway session
# bus instead: what has to be proven is that the request leaves this process in the
# form the portal expects, and a scripted portal is what notices a filter list or a
# folder hint that came out wrong.
# ---------------------------------------------------------------------------

option(VOLUM_BUILD_PORTAL_CHECK "Build volum-portal-check, driven by test/portal-test.bash" ON)

if(VOLUM_BUILD_PORTAL_CHECK AND VOLUM_GIO_FOUND)
  add_executable(volum-portal-check
    "${CMAKE_CURRENT_LIST_DIR}/tools/portal-check.cpp"
    "${IPLUG2_ROOT}/IGraphics/Platforms/IGraphicsLinuxPortal.cpp"
  )

  target_include_directories(volum-portal-check PRIVATE
    "${IPLUG2_ROOT}/IGraphics/Platforms"
    "${CMAKE_CURRENT_LIST_DIR}/tools"
  )
  target_link_libraries(volum-portal-check PRIVATE PkgConfig::VOLUM_GIO)
  target_compile_features(volum-portal-check PRIVATE cxx_std_17)

  set_target_properties(volum-portal-check PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# ---------------------------------------------------------------------------
# volum-msgbox-check drives the in-app message box the platform layer draws
# (issue #2): its own editor window, a ShowMessageBox, and synthetic button
# events answered the way the probe clicks a plug-in's window. The button rects
# come from the platform itself, so the test carries no font metrics of its own.
# ---------------------------------------------------------------------------

option(VOLUM_BUILD_MSGBOX_CHECK "Build volum-msgbox-check, the headless message-box check" ON)

if(VOLUM_BUILD_MSGBOX_CHECK)
  add_executable(volum-msgbox-check
    "${CMAKE_CURRENT_LIST_DIR}/tools/msgbox-check.cpp"
    # The graphics layer reaches into iPlug's parameter, path and timer code; the
    # VST3-specific sources of IPLUG_SOURCES stay out, this client hosts no plug-in.
    "${IPLUG2_ROOT}/IPlug/IPlugParameter.cpp"
    "${IPLUG2_ROOT}/IPlug/IPlugPaths.cpp"
    "${IPLUG2_ROOT}/IPlug/IPlugTimer.cpp"
    ${IGRAPHICS_SOURCES}
  )

  target_include_directories(volum-msgbox-check PRIVATE
    "${IPLUG2_ROOT}/IPlug"
    "${IPLUG2_ROOT}/IPlug/Extras"
    "${IPLUG2_ROOT}/IGraphics"
    "${IPLUG2_ROOT}/IGraphics/Controls"
    "${IPLUG2_ROOT}/IGraphics/Drawing"
    "${IPLUG2_ROOT}/IGraphics/Platforms"
    "${IPLUG2_ROOT}/WDL"
    "${DEPS_PATH}/IGraphics/NanoVG/src"
    "${DEPS_PATH}/IGraphics/NanoSVG/src"
    "${DEPS_PATH}/IGraphics/STB"
  )
  target_compile_definitions(volum-msgbox-check PRIVATE
    IGRAPHICS_NANOVG
    IGRAPHICS_GL2
    RELEASE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
  )
  target_compile_options(volum-msgbox-check PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang>>:-include;cstdlib;-Wno-deprecated-declarations>
  )
  target_link_libraries(volum-msgbox-check PRIVATE
    OpenGL::GL
    X11::X11
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${VOLUM_LIBATOMIC}
  )

  if(VOLUM_GIO_FOUND)
    target_compile_definitions(volum-msgbox-check PRIVATE VOLUM_HAVE_PORTAL=1)
    target_sources(volum-msgbox-check PRIVATE "${IPLUG2_ROOT}/IGraphics/Platforms/IGraphicsLinuxPortal.cpp")
    target_link_libraries(volum-msgbox-check PRIVATE PkgConfig::VOLUM_GIO)
  endif()

  set_target_properties(volum-msgbox-check PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# ---------------------------------------------------------------------------
# volum-panel-check drives the audio settings panel the standalone's plug-in
# draws (issue #10): its own editor window, a panel fed a made-up model (fake
# devices, a throwaway app.conf, an injected probe), and synthetic clicks on
# the real rows. The row rects come from the panel itself, so the test carries
# no font metrics of its own. The conf writer and the host's read-back of a
# rewritten file are checked without a window at all.
# ---------------------------------------------------------------------------

option(VOLUM_BUILD_PANEL_CHECK "Build volum-panel-check, the headless audio-panel check" ON)

if(VOLUM_BUILD_PANEL_CHECK)
  add_executable(volum-panel-check
    "${CMAKE_CURRENT_LIST_DIR}/tools/panel-check.cpp"
    # The conf read-back the host answers the panel with: plain C++, no ALSA,
    # no VST3 - the same file the hosts build from.
    "${CMAKE_CURRENT_LIST_DIR}/tools/options-support.cpp"
    "${IPLUG2_ROOT}/IPlug/IPlugParameter.cpp"
    "${IPLUG2_ROOT}/IPlug/IPlugPaths.cpp"
    "${IPLUG2_ROOT}/IPlug/IPlugTimer.cpp"
    ${IGRAPHICS_SOURCES}
  )

  target_include_directories(volum-panel-check PRIVATE
    "${PROJECT_DIR}"
    "${IPLUG2_ROOT}/IPlug"
    "${IPLUG2_ROOT}/IPlug/Extras"
    "${IPLUG2_ROOT}/IGraphics"
    "${IPLUG2_ROOT}/IGraphics/Controls"
    "${IPLUG2_ROOT}/IGraphics/Drawing"
    "${IPLUG2_ROOT}/IGraphics/Platforms"
    "${CMAKE_CURRENT_LIST_DIR}/tools"
    "${IPLUG2_ROOT}/WDL"
    "${DEPS_PATH}/IGraphics/NanoVG/src"
    "${DEPS_PATH}/IGraphics/NanoSVG/src"
    "${DEPS_PATH}/IGraphics/STB"
  )
  target_compile_definitions(volum-panel-check PRIVATE
    IGRAPHICS_NANOVG
    IGRAPHICS_GL2
    RELEASE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
  )
  target_compile_options(volum-panel-check PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang>>:-include;cstdlib;-Wno-deprecated-declarations>
  )
  target_link_libraries(volum-panel-check PRIVATE
    OpenGL::GL
    X11::X11
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${VOLUM_LIBATOMIC}
  )

  if(VOLUM_GIO_FOUND)
    target_compile_definitions(volum-panel-check PRIVATE VOLUM_HAVE_PORTAL=1)
    target_sources(volum-panel-check PRIVATE "${IPLUG2_ROOT}/IGraphics/Platforms/IGraphicsLinuxPortal.cpp")
    target_link_libraries(volum-panel-check PRIVATE PkgConfig::VOLUM_GIO)
  endif()

  set_target_properties(volum-panel-check PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# ---------------------------------------------------------------------------
# What a user gets from `cmake --install build/linux`: the VST3 bundle where the
# hosts look (VOLUM_INSTALL_DIR, ~/.vst3 by default), and the standalone where a
# desktop looks - bin/, share/applications/, share/icons/hicolor/. These rules sit
# after the host section because that is where volum-app becomes a target: asking
# CMake to install a target it has not created yet installs nothing, quietly.
# ---------------------------------------------------------------------------
if(TARGET volum-app)
  # Le nom d'application AppStream - com.github.guitarlum.VoLum - est a la fois le nom du
  # fichier .desktop, celui des icones hicolor et celui du .metainfo.xml. Les regles ci
  # dessous installent les trois cote a cote, et CI (comme le script d'AppImage) verifie
  # qu'ils concordent : un .desktop dont l'`Icon=` ne designe aucune icone installee, ou un
  # metainfo dont l'`<id>` differe du nom du .desktop, se voit refuser par le depot du
  # bureau, pas par VoLum - donc c'est ici que ca doit se voir.
  install(TARGETS volum-app RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(FILES "${CMAKE_CURRENT_LIST_DIR}/packaging/com.github.guitarlum.VoLum.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
  install(FILES "${CMAKE_CURRENT_LIST_DIR}/packaging/com.github.guitarlum.VoLum.metainfo.xml"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
  install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/packaging/hicolor/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor")

  # Ou ca atterrit, parce que deux endroits differents sont touches selon l'option, et
  # qu'un PATH oublie est la premiere cause d'une entree de menu qui ne lance rien.
  install(CODE "message(\"VoLum : paquet VST3 dans ${VOLUM_INSTALL_DIR} ; programme dans ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR} ; entree de menu dans ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications\")")
endif()
