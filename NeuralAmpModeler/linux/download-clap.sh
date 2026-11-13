#!/usr/bin/env bash

# The CLAP headers the volum-clap wrapper compiles against (issue #9). Headers
# only: the wrapper speaks the format's C API, the SDK is the VST3 module it
# drives. Same pattern as iPlug2/Dependencies/IPlug/download-vst3-sdk.sh, and
# the headers land next to it.

TAG="1.2.3"
if [ "$1" != "" ]; then
  TAG=$1
fi

DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../iPlug2/Dependencies/IPlug/clap"

rm -f -r "$DEST"
git clone https://github.com/free-audio/clap.git --branch $TAG --single-branch --depth=1 "$DEST"
rm -f -r "$DEST/.git"
