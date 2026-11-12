# Public source build notes

This repository contains Project Tandem source and build metadata only. It does
not contain a game image, generated game C files, build output, EOS credentials,
or the proprietary EOS SDK.

## Common requirements

Install CMake 3.20 or newer, Ninja, Python 3, a C/C++17 toolchain, and the
platform graphics/audio development headers. Provide your own legally obtained
GM4E01 ISO when configuring the build.

## Android

Install the Android SDK/NDK and build-tools. Obtain the EOS SDK from Epic under
its developer terms and place its headers and ARM64 runtime in a private local
dependency directory. Build the Android curl helper for arm64 as well. Pass
that directory with:

```sh
cmake -S . -B build-android-arm64 -G Ninja \
  -DTANDEM_EXTERNAL_ROOT=/path/to/private-dependencies \
  -DANDROID_ABI=arm64-v8a
ninja -C build-android-arm64 mkdd
```

The app loads `eos.json` from private application storage. Never commit that
file, a client secret, tokens, keystores, or generated APKs.

## Linux

Install SDL2, libcurl, OpenGL/OpenXR dependencies as appropriate for the host,
then configure with CMake and Ninja. Supply the ISO through the documented
`GAME_ISO` option or the local `rom/` directory. Linux and Android builds share
the source tree but use different platform dependencies.

## Distribution boundary

Only publish source, notices, and documentation. Do not publish the ISO,
anything extracted from it, generated `gen/` output, compiled binaries, EOS
runtime files, private service configuration, or signing keys.
