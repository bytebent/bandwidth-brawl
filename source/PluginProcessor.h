#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "BrawlDSP.h"

class BandwidthBrawlProcessor : public juce::AudioProcessor
{
public:
    BandwidthBrawlProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Bandwidth Brawl"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    bb::LoopView loopView;
    std::atomic<bool> editorOpen { false };
    std::atomic<double> currentSampleRate { 48000.0 };

    // Tempo-synced values, in beats (quarter notes). Must match ui/index.html.
    static constexpr std::array<double, 13> loopBeats { 1.0/16, 1.0/12, 1.0/8, 1.0/6, 1.0/4, 1.0/3, 1.0/2, 2.0/3, 1.0, 2.0, 4.0, 8.0, 16.0 };
    static constexpr std::array<double, 10> holdBeats { 0.0, 1.0/16, 1.0/8, 1.0/6, 1.0/4, 1.0/3, 1.0/2, 1.0, 2.0, 4.0 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    bb::Brawl dsp;
    bool wasPlaying = false;
    int viewCountdown = 0;

    std::atomic<float>* pBudget; std::atomic<float>* pBias; std::atomic<float>* pSharp; std::atomic<float>* pHold;
    std::atomic<float>* pHoldDiv; std::atomic<float>* pHoldSync; std::atomic<float>* pSavings;
    std::atomic<float>* pLoop; std::atomic<float>* pLoopDiv; std::atomic<float>* pLoopSync;
    std::atomic<float>* pChunk; std::atomic<float>* pSmooth; std::atomic<float>* pMemory; std::atomic<float>* pBits;
    std::atomic<float>* pRandom; std::atomic<float>* pBTrim; std::atomic<float>* pMix; std::atomic<float>* pOutGain;
    std::atomic<float>* pResetOnPlay; std::atomic<float>* pSeed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandwidthBrawlProcessor)
};
