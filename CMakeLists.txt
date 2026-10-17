cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(BandwidthBrawl VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is downloaded at configure time. To use a local copy instead:
#   cmake -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE ...
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(BB_FORMATS AU VST3 Standalone)
else()
    set(BB_FORMATS VST3 Standalone)
endif()

juce_add_plugin(BandwidthBrawl
    PRODUCT_NAME                "Bandwidth Brawl"
    COMPANY_NAME                "Bytebent Audio"
    BUNDLE_ID                   "com.bytebent.bandwidthbrawl"
    PLUGIN_MANUFACTURER_CODE    Bytb
    PLUGIN_CODE                 Bwbr
    FORMATS                     ${BB_FORMATS}
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    NEEDS_WEB_BROWSER           TRUE
    NEEDS_CURL                  FALSE
    AU_MAIN_TYPE                kAudioUnitType_Effect
    VST3_CATEGORIES             Fx Distortion
    COPY_PLUGIN_AFTER_BUILD     FALSE)

juce_add_binary_data(BandwidthBrawlUI SOURCES ui/index.html)

target_sources(BandwidthBrawl PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp)

target_compile_definitions(BandwidthBrawl PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(BandwidthBrawl
    PRIVATE
        BandwidthBrawlUI
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Command-line test runner for the DSP core (see tests/compare.py).
add_executable(dsp_cli tests/dsp_cli.cpp)
