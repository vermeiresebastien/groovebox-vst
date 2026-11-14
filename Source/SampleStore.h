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

    The audio thread takes no lock here. It reads a channel's atomic sample id
    and resolves it through `retainSampleForChannel`, which returns a
    reference-counted pointer that stays valid for as long as it is held.

    That works without a lock because the registry is append-only: a sample, once
    published, is never removed, never moved and never has its id reused. So the
    store holds a reference to every sample for its own lifetime, which means the
    audio thread can increment a refcount on a pointer it just read and cannot
    race with the object being freed. Publication is a release store and the read
    an acquire load, so a slot is either empty or holds a fully decoded sample.

    The price is a ceiling on how many samples one session can load. It reports
    that ceiling as an error rather than growing without bound.
*/
class SampleStore
{
public:
    /** Distinct samples one session can hold. Ids start at 1 and map to slot id-1. */
    static constexpr int maxSamples = 512;

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

    /** Ids of every loaded sample, oldest first (for the channel menus). */
    juce::Array<int> getSampleIds() const;

    void clearChannel (int channel);

    //==========================================================================
    // Audio thread access
    //==========================================================================

    /** Resolves a channel's sample without allocation or locking; may be null. */
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

    /** Publishes the sample and returns its id, or 0 when the registry is full. */
    int addSample (Sample::Ptr sample);

    /** Raw slot for an id, or nullptr when the id is out of range. */
    Sample* slotFor (int sampleId) const;

    std::array<ChannelData, Layout::numChannels> channels;

    /**
        Registered once, in the constructor, rather than per load.

        `registerBasicFormats` builds a reader for every format JUCE ships with, so
        doing it on each drag-and-drop was repeating that work for nothing. Message
        thread only, like every other decode here.
    */
    juce::AudioFormatManager formats;

    // Written only by the message thread, read by both. The `{}` zero-initialises
    // every atomic, which in C++17 is what makes an untouched slot a null pointer
    // rather than an indeterminate one.
    std::array<std::atomic<Sample*>, (size_t) maxSamples> slots {};

    /** The references that keep every published sample alive. Message thread only. */
    std::vector<Sample::Ptr> owned;

    int nextSampleId = 1;
    int sampleCounter = 0; // used to name imported files

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleStore)
};

} // namespace gb
