# Project Tandem Linux source

The complete public source tree is under [`source/`](source/). It contains the
shared recompilation runtime, Linux-facing build configuration, renderer,
audio, controls, and online code.

The repository intentionally excludes game images, generated `gen/` output,
compiled binaries, EOS credentials, and proprietary runtime components. Supply
your own legally obtained GM4E01 image and install the host dependencies before
configuring with CMake and Ninja. See [`source/BUILDING-PUBLIC.md`](source/BUILDING-PUBLIC.md).

The MIT notices and upstream attribution files are retained in `source/`.
