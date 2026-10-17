#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
using APF = juce::AudioParameterFloat;
using Attr = juce::AudioParameterFloatAttributes;

// Log-scaled range: normalised n maps to min * (max/min)^n. Must match ui/index.html.
juce::NormalisableRange<float> logRange (float min, float max)
{
    return { min, max,
             [] (float a, float b, float n) { return a * std::pow (b / a, n); },
             [] (float a, float b, float v) { return std::log (v / a) / std::log (b / a); },
             [] (float, float, float v) { return v; } };
}

// Cubic range starting at 0 ("off"): fine control at the low end. Must match ui/index.html.
juce::NormalisableRange<float> cubeRange (float min, float max)
{
    return { min, max,
             [] (float a, float b, float n) { return a + n * n * n * (b - a); },
             [] (float a, float b, float v) { return std::cbrt (juce::jlimit (0.0f, 1.0f, (v - a) / (b - a))); },
             [] (float, float, float v) { return v; } };
}

juce::String pct (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }
juce::String dbStr (float v, int) { return juce::String (v, 1) + " dB"; }
juce::String msOrOff (float v, int) { return v < 0.5f ? juce::String ("off") : juce::String (juce::roundToInt (v)) + " ms"; }
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout BandwidthBrawlProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    auto id = [] (const char* s) { return juce::ParameterID { s, 1 }; };
    const juce::StringArray loopNotes { "1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 bar", "2 bars", "4 bars" };
    const juce::StringArray holdNotes { "off", "1/64", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4", "1/2", "1 bar" };

    // Fight
    l.add (std::make_unique<APF> (id ("budget"), "Budget", logRange (0.5f, 1000.0f), 24.0f,
        Attr().withLabel ("kB/s").withStringFromValueFunction ([] (float v, int) {
            return juce::String (v, v < 10.0f ? 1 : 0) + " kB/s"; })));
    l.add (std::make_unique<APF> (id ("bias"), "Bias", juce::NormalisableRange<float> (-1.0f, 1.0f), 0.0f,
        Attr().withStringFromValueFunction ([] (float v, int) {
            const int a = juce::roundToInt ((1.0f - v) * 50.0f);
            return a == 50 ? juce::String ("even") : "A " + juce::String (a) + " : " + juce::String (100 - a) + " B"; })));
    l.add (std::make_unique<APF> (id ("sharpness"), "Sharpness", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f,
        Attr().withStringFromValueFunction (pct)));
    l.add (std::make_unique<APF> (id ("hold"), "Hold", cubeRange (0.0f, 1000.0f), 0.0f,
        Attr().withLabel ("ms").withStringFromValueFunction (msOrOff)));
    l.add (std::make_unique<juce::AudioParameterChoice> (id ("holdDiv"), "Synced hold", holdNotes, 4));
    l.add (std::make_unique<juce::AudioParameterBool> (id ("holdSync"), "Sync hold to tempo", false));
    l.add (std::make_unique<APF> (id ("savings"), "Savings", cubeRange (0.0f, 2000.0f), 0.0f,
        Attr().withLabel ("ms").withStringFromValueFunction (msOrOff)));

    // Buffer
    l.add (std::make_unique<APF> (id ("loop"), "Loop length", logRange (1.0f, 4000.0f), 250.0f,
        Attr().withLabel ("ms").withStringFromValueFunction ([] (float v, int) {
            return v < 1000.0f ? juce::String (v, v < 10.0f ? 1 : 0) + " ms" : juce::String (v / 1000.0f, 2) + " s"; })));
    l.add (std::make_unique<juce::AudioParameterChoice> (id ("loopDiv"), "Synced loop length", loopNotes, 6));
    l.add (std::make_unique<juce::AudioParameterBool> (id ("loopSync"), "Sync loop to tempo", false));
    l.add (std::make_unique<APF> (id ("chunk"), "Chunk size", logRange (1.0f, 4096.0f), 64.0f,
        Attr().withLabel ("smp").withStringFromValueFunction ([] (float v, int) {
            return juce::String (juce::roundToInt (v)) + " smp"; })));
    l.add (std::make_unique<APF> (id ("smooth"), "Smoothing", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f,
        Attr().withStringFromValueFunction (pct)));
    l.add (std::make_unique<APF> (id ("memory"), "Memory", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f,
        Attr().withStringFromValueFunction (pct)));
    l.add (std::make_unique<juce::AudioParameterInt> (id ("bits"), "Bits", 2, 16, 16,
        juce::AudioParameterIntAttributes().withLabel ("bit")));
    l.add (std::make_unique<APF> (id ("randomness"), "Random", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f,
        Attr().withStringFromValueFunction (pct)));

    // In / out
    l.add (std::make_unique<APF> (id ("bTrim"), "B Trim", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f,
        Attr().withStringFromValueFunction (dbStr)));
    l.add (std::make_unique<APF> (id ("mix"), "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f,
        Attr().withStringFromValueFunction (pct)));
    l.add (std::make_unique<APF> (id ("outGain"), "Output", juce::NormalisableRange<float> (-24.0f, 12.0f), 0.0f,
        Attr().withStringFromValueFunction (dbStr)));
    l.add (std::make_unique<juce::AudioParameterBool> (id ("resetOnPlay"), "Reset when playback starts", true));
    l.add (std::make_unique<juce::AudioParameterInt> (id ("seed"), "Seed", 0, 9999, 1));
    return l;
}

BandwidthBrawlProcessor::BandwidthBrawlProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    auto get = [this] (const char* s) { return apvts.getRawParameterValue (s); };
    pBudget = get ("budget"); pBias = get ("bias"); pSharp = get ("sharpness"); pHold = get ("hold");
    pHoldDiv = get ("holdDiv"); pHoldSync = get ("holdSync"); pSavings = get ("savings");
    pLoop = get ("loop"); pLoopDiv = get ("loopDiv"); pLoopSync = get ("loopSync");
    pChunk = get ("chunk"); pSmooth = get ("smooth"); pMemory = get ("memory"); pBits = get ("bits");
    pRandom = get ("randomness"); pBTrim = get ("bTrim"); pMix = get ("mix"); pOutGain = get ("outGain");
    pResetOnPlay = get ("resetOnPlay"); pSeed = get ("seed");
}

bool BandwidthBrawlProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    if (out != layouts.getMainInputChannelSet()) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto side = layouts.getChannelSet (true, 1);
        if (! side.isDisabled() && side != juce::AudioChannelSet::mono() && side != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void BandwidthBrawlProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    dsp.prepare (sampleRate, getMainBusNumOutputChannels(), 8.0);
    dsp.reset ((uint32_t) pSeed->load());
    setLatencySamples (dsp.latencySamples());
    wasPlaying = false;
    viewCountdown = 0;
}

void BandwidthBrawlProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);   // the track; processed in place
    const int numMain = main.getNumChannels();
    if (numMain <= 0) return;

    // The sidechain (input B). A view into the host's buffer, built in place: no copy, no allocation.
    const bool hasSide = getBusCount (true) > 1 && getBus (true, 1) != nullptr && getBus (true, 1)->isEnabled();
    const auto side = hasSide ? getBusBuffer (buffer, true, 1) : juce::AudioBuffer<float>();
    const int numSide = side.getNumChannels();
    const float* const* sidePtrs = numSide > 0 ? side.getArrayOfReadPointers() : nullptr;

    bb::Params p;
    p.budgetKbs = pBudget->load(); p.bias = pBias->load(); p.sharpness = pSharp->load();
    p.holdMs = pHold->load(); p.savingsMs = pSavings->load(); p.loopMs = pLoop->load();
    p.chunk = pChunk->load(); p.smooth = pSmooth->load(); p.memory = pMemory->load(); p.bits = pBits->load();
    p.randomness = pRandom->load(); p.bTrimDb = pBTrim->load(); p.mix = pMix->load(); p.outGainDb = pOutGain->load();

    bool playing = false;
    double bpm = 120.0;
    if (auto* head = getPlayHead())
        if (auto pos = head->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto b = pos->getBpm()) bpm = juce::jlimit (20.0, 999.0, *b);
        }

    if (pLoopSync->load() > 0.5f)
    {
        const int d = juce::jlimit (0, (int) loopBeats.size() - 1, (int) pLoopDiv->load());
        p.loopMs = (float) juce::jlimit (1.0, 8000.0, loopBeats[(size_t) d] * 60000.0 / bpm);
    }
    if (pHoldSync->load() > 0.5f)
    {
        const int d = juce::jlimit (0, (int) holdBeats.size() - 1, (int) pHoldDiv->load());
        p.holdMs = (float) (holdBeats[(size_t) d] * 60000.0 / bpm);
    }

    if (playing && ! wasPlaying && pResetOnPlay->load() > 0.5f)
        dsp.reset ((uint32_t) pSeed->load());
    wasPlaying = playing;

    dsp.process (main.getArrayOfWritePointers(), numMain, sidePtrs, numSide, main.getNumSamples(), p);

    if (editorOpen.load (std::memory_order_relaxed))
    {
        viewCountdown -= buffer.getNumSamples();
        if (viewCountdown <= 0)
        {
            dsp.writeView (loopView);
            viewCountdown = (int) (currentSampleRate.load() / 30.0);
        }
    }
}

juce::AudioProcessorEditor* BandwidthBrawlProcessor::createEditor()
{
    return new BandwidthBrawlEditor (*this);
}

void BandwidthBrawlProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void BandwidthBrawlProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BandwidthBrawlProcessor();
}
