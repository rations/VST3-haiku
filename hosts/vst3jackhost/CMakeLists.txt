# vst3jackhost — headless VST3 host on JACK with a stdin parameter REPL.
# Reuses the SDK audiohost sample's JACK backend and AudioClient glue.

set(SDK_DIR ${CMAKE_SOURCE_DIR}/vst3sdk)
set(AUDIOHOST_DIR ${SDK_DIR}/public.sdk/samples/vst-hosting/audiohost)

list(APPEND CMAKE_MODULE_PATH ${SDK_DIR}/cmake/modules)
include(SMTG_FindJack)

if(NOT ${LIBJACK_FOUND})
    message(STATUS "[VST3-haiku] JACK not found - skipping vst3jackhost.")
    return()
endif()

add_executable(vst3jackhost
    source/main.cpp
    ${AUDIOHOST_DIR}/source/media/audioclient.cpp
    ${AUDIOHOST_DIR}/source/media/jack/jackclient.cpp
    ${SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${SDK_DIR}/public.sdk/source/vst/hosting/module_haiku.cpp
)

target_compile_features(vst3jackhost
    PUBLIC
        cxx_std_17
)
target_include_directories(vst3jackhost
    PRIVATE
        ${SDK_DIR}
        ${LIBJACK_INCLUDE_DIR}
)
target_link_libraries(vst3jackhost
    PRIVATE
        sdk_hosting
        ${LIBJACK_LIBRARY}
)
set_target_properties(vst3jackhost
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>
)
