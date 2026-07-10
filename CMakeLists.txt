# VST3-haiku — native VST3 for Haiku OS.
#
# Builds the vendored, Haiku-patched Steinberg VST 3 SDK in a headless
# configuration (no VSTGUI, no X11): core libraries, the validator, the
# moduleinfotool, the JACK-based audiohost sample, and the SDK example
# plug-ins (adelay, mda-vst3, ... — the VSTGUI-based examples exclude
# themselves). Our own hosts/ and plugins/ subdirectories are added on top.

cmake_minimum_required(VERSION 3.25.0)

project(VST3-haiku
    VERSION 0.1.0
    DESCRIPTION "Native VST3 for Haiku: SDK port, JACK host, plugins"
)

# Headless SDK configuration. VSTGUI (and with it every X11/cairo/pango
# dependency) stays off until a Haiku backend exists; plugin GUIs are not
# usable on Haiku anyway until plug-ins are built with a Haiku view type.
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "VSTGUI has no Haiku backend yet" FORCE)
set(SMTG_ENABLE_WAYLAND_SUPPORT OFF CACHE BOOL "no Wayland on Haiku" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "validator + audiohost" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES ON CACHE BOOL "adelay, mda-vst3, ... (VSTGUI-based examples exclude themselves)" FORCE)

# The validator is run manually per phase gate (see the Makefile 'validate'
# target); running it as a post-build step of every plug-in would abort
# builds on expected-fail baselines while the port is being brought up.
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "run manually instead" FORCE)

# Plug-ins are installed with the Makefile 'install' target instead of
# per-target symlinks into the user's add-ons directory.
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "install via Makefile instead" FORCE)

add_subdirectory(vst3sdk)

# Our own targets
add_subdirectory(hosts/vst3jackhost)
# add_subdirectory(plugins/namku)   # phase 4
