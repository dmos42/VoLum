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

# ---------------------------------------------------------------------------
# volum-vst3-probe: a very small host (dlopen, factory, one instance, a test tone,
# optionally the editor inside a plain X11 window). It answers "does the port load
# and make sound?" without needing a DAW installed.
# ---------------------------------------------------------------------------

option(VOLUM_BUILD_PROBE "Build the small headless VST3 host used to check the port" ON)

if(VOLUM_BUILD_PROBE)
  add_executable(volum-vst3-probe
    "${CMAKE_CURRENT_LIST_DIR}/tools/vst3-probe.cpp"
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

  target_compile_definitions(volum-vst3-probe PRIVATE RELEASE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
  target_include_directories(volum-vst3-probe PRIVATE "${VST3_SDK}")
  target_link_libraries(volum-vst3-probe PRIVATE X11::X11 ${CMAKE_DL_LIBS} ${VOLUM_LIBATOMIC})
  target_compile_features(volum-vst3-probe PRIVATE cxx_std_17)

  set_target_properties(volum-vst3-probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
