#include "SampleStore.h"

namespace gb
{

namespace
{
    /** Channels start out with the three names the web app ships with. */
    const char* defaultChannelName (int channel)
    {
        switch (channel)
        {
            case 0:  return "Kick";
            case 1:  return "Snare";
            case 2:  return "Hat";
            default: return "Drum";
        }
    }
}

SampleStore::SampleStore()
{
    for (int i = 0; i < Layout::numChannels; ++i)
    {
        channels[(size_t) i].name = defaultChannelName (i);
        channels[(size_t) i].colour = juce::Colour (lanePalette[(size_t) (i % (int) lanePalette.size())]);
    }

    // Keep the three default lanes on the same colour the web app uses.
    for (int i = 0; i < 3; ++i)
        channels[(size_t) i].colour = defaultLaneColour;
}

//==============================================================================
// Decoding

int SampleStore::addFile (const juce::File& file, juce::String& errorOut)
{
    if (! file.existsAsFile())
    {
        errorOut = "File not found";
        return 0;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        errorOut = "Unsupported or unreadable audio file";
        return 0;
    }

    auto sample = new Sample();
    sample->name = file.getFileNameWithoutExtension();
    sample->sourcePath = file.getFullPathName();
    sample->sourceSampleRate = reader->sampleRate;

    const auto numChannels = juce::jmax (1, (int) reader->numChannels);
    const auto numSamples  = (int) juce::jmax ((juce::int64) 0, reader->lengthInSamples);

    if (numSamples <= 0)
    {
        errorOut = "File contains no audio";
        return 0;
    }

    sample->buffer.setSize (numChannels, numSamples);
    reader->read (&sample->buffer, 0, numSamples, 0, true, numChannels > 1);

    return addSample (sample);
}

int SampleStore::addFromMemory (const void* data, size_t numBytes, const juce::String& nameHint, juce::String& errorOut)
{
    if (data == nullptr || numBytes == 0)
    {
        errorOut = "Dropped item contained no audio data";
        return 0;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    // The manager's stream overload takes a unique_ptr and hands ownership to the
    // reader, so the stream lives exactly as long as the reader does.
    auto stream = std::make_unique<juce::MemoryInputStream> (data, numBytes, false);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (std::move (stream)));

    if (reader == nullptr)
    {
        errorOut = "Unsupported or unreadable audio data";
        return 0;
    }

    auto sample = new Sample();
    sample->name = nameHint.isNotEmpty() ? nameHint : ("Sample " + juce::String (++sampleCounter));
    sample->sourceSampleRate = reader->sampleRate;

    const auto numChannels = juce::jmax (1, (int) reader->numChannels);
    const auto numSamples  = (int) juce::jmax ((juce::int64) 0, reader->lengthInSamples);

    if (numSamples <= 0)
    {
        errorOut = "Dropped audio contained no samples";
        return 0;
    }

    sample->buffer.setSize (numChannels, numSamples);
    reader->read (&sample->buffer, 0, numSamples, 0, true, numChannels > 1);

    return addSample (sample);
}

int SampleStore::addSample (Sample::Ptr sample)
{
    const juce::SpinLock::ScopedLockType lock (registryLock);

    const auto id = nextSampleId++;
    registry[id] = sample;
    return id;
}

//==============================================================================
// Assignment

void SampleStore::setChannelSample (int channel, int sampleId)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    channels[(size_t) channel].sampleId.store (sampleId);
}

void SampleStore::clearChannel (int channel)
{
    setChannelSample (channel, 0);
}

Sample::Ptr SampleStore::getSample (int sampleId) const
{
    if (sampleId <= 0)
        return nullptr;

    const juce::SpinLock::ScopedLockType lock (registryLock);

    const auto it = registry.find (sampleId);
    return it != registry.end() ? it->second : nullptr;
}

juce::String SampleStore::getSampleName (int sampleId) const
{
    auto sample = getSample (sampleId);
    return sample != nullptr ? sample->name : juce::String();
}

juce::String SampleStore::getSamplePath (int sampleId) const
{
    auto sample = getSample (sampleId);
    return sample != nullptr ? sample->sourcePath : juce::String();
}

juce::Array<int> SampleStore::getSampleIds() const
{
    const juce::SpinLock::ScopedLockType lock (registryLock);

    juce::Array<int> ids;

    for (const auto& entry : registry)
        ids.add (entry.first);

    return ids;
}

//==============================================================================
// Audio thread access

Sample::Ptr SampleStore::retainSampleForChannel (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return nullptr;

    return getSample (channels[(size_t) channel].sampleId.load());
}

int SampleStore::getChannelSampleId (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return 0;

    return channels[(size_t) channel].sampleId.load();
}

//==============================================================================
// Metadata

const juce::String& SampleStore::getChannelName (int channel) const
{
    jassert (juce::isPositiveAndBelow (channel, Layout::numChannels));
    return channels[(size_t) channel].name;
}

void SampleStore::setChannelName (int channel, const juce::String& name)
{
    if (juce::isPositiveAndBelow (channel, Layout::numChannels))
        channels[(size_t) channel].name = name.substring (0, 14);
}

juce::Colour SampleStore::getChannelColour (int channel) const
{
    jassert (juce::isPositiveAndBelow (channel, Layout::numChannels));
    return channels[(size_t) channel].colour;
}

void SampleStore::setChannelColour (int channel, juce::Colour colour)
{
    if (juce::isPositiveAndBelow (channel, Layout::numChannels))
        channels[(size_t) channel].colour = colour;
}

bool SampleStore::channelHasSample (int channel) const
{
    return getChannelSampleId (channel) > 0;
}

} // namespace gb
