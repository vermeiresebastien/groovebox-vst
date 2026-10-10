#pragma once

#include "Common.h"

namespace gb
{

/**
    A decoded audio sample, shared between the message thread (which loads and
    replaces samples) and the audio thread (which plays them).

    Reference counting is what makes replacing a sample safe while it is still
    sounding: the engine holds a reference for the lifetime of the voice that is
    playing it, so the buffer cannot be freed underneath it.
*/
class Sample : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<Sample>;

    Sample() = default;

    juce::AudioBuffer<float> buffer;
    juce::String name;
    juce::String sourcePath;
    double sourceSampleRate = 44100.0;

    /** Sample rate ratio applied on top of the pitch ratio when resampling. */
    double rateRatioFor (double hostSampleRate) const
    {
        return sourceSampleRate > 0.0 ? sourceSampleRate / hostSampleRate : 1.0;
    }

    double lengthSeconds() const
    {
        return sourceSampleRate > 0.0 ? (double) buffer.getNumSamples() / sourceSampleRate : 0.0;
    }
};

/**
    Owns every loaded sample and the per-channel assignment of samples to the
    twelve drum lanes.

    The audio thread never touches juce::String or the map: it reads a channel's
    atomic sample id and resolves it through `retainSample`, which returns a
    reference-counted pointer that stays valid for as long as it is held.
*/
class SampleStore
{
public:
    SampleStore();

    //==========================================================================
    // Loading (message thread)
    //==========================================================================

    /** Decodes an audio file and returns the new sample id, or 0 on failure. */
    int addFile (const juce::File& file, juce::String& errorOut);

    /** Decodes raw bytes (used by drag-and-drop from other applications). */
    int addFromMemory (const void* data, size_t numBytes, const juce::String& nameHint, juce::String& errorOut);

    /** Assigns a sample to a channel. Pass 0 to clear the channel. */
    void setChannelSample (int channel, int sampleId);

    /** Looks up a sample by id; returns nullptr when it is not loaded. */
    Sample::Ptr getSample (int sampleId) const;

    juce::String getSampleName (int sampleId) const;

    /** Path the sample was loaded from; empty for dropped or generated audio. */
    juce::String getSamplePath (int sampleId) const;

    /** Ids of every loaded sample, newest last (for the channel menus). */
    juce::Array<int> getSampleIds() const;

    void clearChannel (int channel);

    //==========================================================================
    // Audio thread access
    //==========================================================================

    /** Resolves a channel's sample without allocation; may return nullptr. */
    Sample::Ptr retainSampleForChannel (int channel) const;

    int getChannelSampleId (int channel) const;

    //==========================================================================
    // Channel metadata (message thread only)
    //==========================================================================

    const juce::String& getChannelName (int channel) const;
    void setChannelName (int channel, const juce::String& name);

    juce::Colour getChannelColour (int channel) const;
    void setChannelColour (int channel, juce::Colour colour);

    bool channelHasSample (int channel) const;

private:
    struct ChannelData
    {
        std::atomic<int> sampleId { 0 };
        juce::String name;
        juce::Colour colour { defaultLaneColour };
    };

    int addSample (Sample::Ptr sample);

    std::array<ChannelData, Layout::numChannels> channels;

    // The registry is only mutated on the message thread. The audio thread only
    // ever walks it while holding the spin lock, and only for a few cycles.
    mutable juce::SpinLock registryLock;
    std::map<int, Sample::Ptr> registry;
    int nextSampleId = 1;
    int sampleCounter = 0; // used to name imported files

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleStore)
};

} // namespace gb
