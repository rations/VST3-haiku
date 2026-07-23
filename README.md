# VST3-haiku

A native port of the [Steinberg VST 3 SDK](https://github.com/steinbergmedia/vst3sdk)
(3.8.0, MIT-licensed) to [Haiku OS](https://www.haiku-os.org/), with Haiku added as a
first-class platform (`SMTG_OS_HAIKU`) rather than a Linux masquerade.

This is part of a native Haiku pro-audio stack built on a full
[JACK2 port](https://github.com/rations) (jackd with native `hmulti`/Media Kit backends,
standard libjack API): a graphical patchbay, a multitrack DAW (jackDAW), and — with this
repository — VST3 plugin hosting and plugins.

## What's here

- `vst3sdk/` — the vendored SDK with Haiku patches applied in place at the canonical
  upstream file locations, so the whole Haiku port is one reviewable diff against the
  pristine 3.8.0 import commit. Key additions:
  - `pluginterfaces/base/fplatform.h`: `__HAIKU__` platform branch (`SMTG_OS_HAIKU`)
  - `public.sdk/source/vst/hosting/module_haiku.cpp`: module loading with Haiku-native
    application-path lookup and plugin search via `find_paths()` add-on directories
  - CMake: `SMTG_HAIKU` platform detection and `Contents/x86_64-haiku/` bundle layout
- `hosts/vst3jackhost/` — a headless VST3 host on JACK with an interactive parameter REPL
  (list parameters, set values live — no GUI required).
Plug-ins built on this port live in their own repositories —
[NAMku](https://github.com/rations/NAMku), a Neural Amp Modeler plugin written directly
against the SDK (no framework), is built automatically when checked out as a sibling
directory of this one (see the `NAMKU_DIR` cache variable).

Plugin bundles use `<name>.vst3/Contents/x86_64-haiku/<name>.so` and are searched in the
canonical Haiku add-on directory `add-ons/media/VST3` across every install root
(`~/config/non-packaged/add-ons/media/VST3` and the packaged/system equivalents). No
dot-folders — Haiku organises per-user files under the `~/config` hierarchy.

## Status

Work in progress. Milestones: SDK core libraries + `validator` running on Haiku → SDK
sample plugins (adelay, mda suite) passing validation → live JACK audio through
vst3jackhost → NAMku → FX chains in jackDAW-haiku with a native parameter window.
Plugin GUIs (IPlugView) are out of scope for now: no plugin ships a Haiku-compatible view,
so hosting is parameter-based ("just the sliders").

## Building (on Haiku)

Requires Haiku x86_64 (tested on a recent nightly), gcc 13, cmake ≥ 3.25, ninja.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

or use the convenience `Makefile` wrapper: `make configure build`.

## Packaging (Haiku)

Build a distributable package with `packaging/make-hpkg.sh` (produces
`vst3_haiku-0.1.0-1-x86_64.hpkg`: the `vst3jackhost` binary plus the SDK example
plug-ins), or install from source with `build-from-source.sh`. A HaikuPorts recipe is in
`packaging/vst3_haiku-0.1.0.recipe`. See the stack overview in `jackDAW-haiku/STACK.md`.

## License

MIT — see [LICENSE](LICENSE). The vendored VST 3 SDK is MIT-licensed by Steinberg Media
Technologies GmbH (see `vst3sdk/LICENSE.txt`). "VST" is a trademark of Steinberg Media
Technologies GmbH; this project is an unofficial community port and is not affiliated with
or endorsed by Steinberg.
