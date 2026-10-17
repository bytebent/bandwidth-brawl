#include "PluginEditor.h"
#include "BinaryData.h"

BandwidthBrawlEditor::BandwidthBrawlEditor (BandwidthBrawlProcessor& p)
    : AudioProcessorEditor (p),
      proc (p),
      browser (juce::WebBrowserComponent::Options {}
                   .withNativeIntegrationEnabled()
                   .withInitialisationData ("version", juce::String (JucePlugin_VersionString))
                   .withResourceProvider ([] (const juce::String& url) { return getResource (url); })
                   .withEventListener ("setParam", [this] (const juce::var& v) { onSetParam (v); })
                   .withEventListener ("gesture", [this] (const juce::var& v) { onGesture (v); })
                   .withEventListener ("uiReady", [this] (const juce::var&) { needFullSync = true; }))
{
    addAndMakeVisible (browser);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    setSize (width, height);
    proc.editorOpen = true;
    startTimerHz (30);
}

BandwidthBrawlEditor::~BandwidthBrawlEditor()
{
    stopTimer();
    proc.editorOpen = false;
    for (auto& id : openGestures)
        if (auto* param = proc.apvts.getParameter (id))
            param->endChangeGesture();
}

void BandwidthBrawlEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xffdcdce0));   // platinum, shown for a moment while the page loads
}

void BandwidthBrawlEditor::resized()
{
    browser.setBounds (getLocalBounds());
}

std::optional<juce::WebBrowserComponent::Resource> BandwidthBrawlEditor::getResource (const juce::String& url)
{
    const auto path = url.fromFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("?", false, false);
    if (path.isEmpty() || path == "index.html")
    {
        const auto* bytes = reinterpret_cast<const std::byte*> (BinaryData::index_html);
        return juce::WebBrowserComponent::Resource {
            std::vector<std::byte> (bytes, bytes + BinaryData::index_htmlSize), "text/html; charset=utf-8" };
    }
    return std::nullopt;
}

void BandwidthBrawlEditor::onSetParam (const juce::var& v)
{
    const auto id = v.getProperty ("id", {}).toString();
    auto* param = proc.apvts.getParameter (id);
    if (param == nullptr) return;
    const float value = juce::jlimit (0.0f, 1.0f, (float) (double) v.getProperty ("value", 0.0));

    const bool inGesture = openGestures.count (id) > 0;
    if (! inGesture) param->beginChangeGesture();
    param->setValueNotifyingHost (value);
    if (! inGesture) param->endChangeGesture();
    lastSent[id] = param->getValue();   // don't echo our own change back
}

void BandwidthBrawlEditor::onGesture (const juce::var& v)
{
    const auto id = v.getProperty ("id", {}).toString();
    auto* param = proc.apvts.getParameter (id);
    if (param == nullptr) return;
    const bool begin = (bool) v.getProperty ("begin", false);
    if (begin && openGestures.insert (id).second) param->beginChangeGesture();
    if (! begin && openGestures.erase (id) > 0) param->endChangeGesture();
}

void BandwidthBrawlEditor::timerCallback()
{
    // 1) Parameter values the UI doesn't know yet (automation, presets, undo, first load).
    for (auto* p : proc.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (rp == nullptr) continue;
        const auto id = rp->getParameterID();
        const float value = rp->getValue();
        auto it = lastSent.find (id);
        if (needFullSync || it == lastSent.end() || std::abs (it->second - value) > 1.0e-6f)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id", id);
            obj->setProperty ("value", value);
            browser.emitEventIfBrowserIsVisible ("param", juce::var (obj));
            lastSent[id] = value;
        }
    }
    needFullSync = false;

    // 2) Loop view frame, if the audio thread has produced a new one.
    auto& lv = proc.loopView;
    const auto counter = lv.counter.load (std::memory_order_acquire);
    if (counter != lastViewCounter)
    {
        lastViewCounter = counter;
        juce::Array<juce::var> amp, own, age;
        amp.ensureStorageAllocated (bb::LoopView::N);
        own.ensureStorageAllocated (bb::LoopView::N);
        age.ensureStorageAllocated (bb::LoopView::N);
        for (int i = 0; i < bb::LoopView::N; ++i)
        {
            amp.add (lv.amp[(size_t) i].load (std::memory_order_relaxed));
            own.add (lv.own[(size_t) i].load (std::memory_order_relaxed));
            age.add (lv.age[(size_t) i].load (std::memory_order_relaxed));
        }
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("amp", amp);
        obj->setProperty ("own", own);
        obj->setProperty ("age", age);
        obj->setProperty ("shareA", lv.shareA.load());
        obj->setProperty ("holdA", lv.holdA.load());
        obj->setProperty ("holdB", lv.holdB.load());
        obj->setProperty ("sideLevel", lv.sideLevel.load());
        obj->setProperty ("loopMs", lv.loopMs.load());
        obj->setProperty ("playhead", lv.playhead.load());
        obj->setProperty ("sampleRate", proc.currentSampleRate.load());
        browser.emitEventIfBrowserIsVisible ("loopview", juce::var (obj));
    }
}
