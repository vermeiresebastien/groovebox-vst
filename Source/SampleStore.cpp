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
    formats.registerBasicFormats();

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

SampleStore::FileKey SampleStore::fileKeyFor (const juce::File& file)
{
    return { file.getFullPathName(), file.getSize(), file.getLastModificationTime().toMilliseconds() };
}

int SampleStore::findSampleForFile (const juce::File& file) const
{
    if (! file.existsAsFile())
        return 0;

    const auto key = fileKeyFor (file);

    // Oldest registration first. If a file was loaded, edited and loaded again, only
    // the second entry still describes what is on disk, so the order here decides
    // nothing about which bytes come back — the two entries for one path are the
    // same decoded audio whenever both match at all.
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i].matches (key))
            return (int) i + 1;

    return 0;
}

int SampleStore::addFile (const juce::File& file, juce::String& errorOut)
{
    if (! file.existsAsFile())
    {
        errorOut = "File not found";
        return 0;
    }

    // The key is read before the decode, because it has to describe the file as it
    // was when the bytes were read. Taking it afterwards would record a modification
    // time that a later recall could not tell apart from this one.
    const auto key = fileKeyFor (file);

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

    const auto id = addSample (sample, key);

    if (id == 0)
        errorOut = "This session has already loaded " + juce::String (maxSamples) + " samples";

    return id;
}

int SampleStore::addFromMemory (const void* data, size_t numBytes, const juce::String& nameHint, juce::String& errorOut)
{
    if (data == nullptr || numBytes == 0)
    {
        errorOut = "Dropped item contained no audio data";
        return 0;
    }

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

    const auto id = addSample (sample);

    if (id == 0)
        errorOut = "This session has already loaded " + juce::String (maxSamples) + " samples";

    return id;
}

int SampleStore::addSample (Sample::Ptr sample, FileKey origin)
{
    if (sample == nullptr)
        return 0;

    if (nextSampleId > maxSamples)
        return 0; // the registry is full; the caller turns this into an error

    const auto id = nextSampleId;

    // The owning reference goes in first. From the moment the slot is published
    // the audio thread may retain the sample, and it can only do that safely
    // because something here holds it for the rest of the session.
    owned.push_back (sample);

    // Alongside it, in the same order, so an index into either is an index into the
    // other and into the ids. Both go in here rather than at the call sites, which
    // is what keeps them from drifting apart.
    keys.push_back (std::move (origin));

    // Release, so a reader that acquires this pointer also sees the decoded
    // buffer, the name and the sample rate that were written before it.
    slots[(size_t) (id - 1)].store (sample.get(), std::memory_order_release);

    ++nextSampleId;
    return id;
}

Sample* SampleStore::slotFor (int sampleId) const
{
    if (! juce::isPositiveAndBelow (sampleId - 1, maxSamples))
        return nullptr;

    return slots[(size_t) (sampleId - 1)].load (std::memory_order_acquire);
}

//==============================================================================
// Assignment

void SampleStore::setChannelSample (int channel, int sampleId)
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return;

    // Release, paired with the acquire on the id in retainSampleForChannel: a
    // thread that sees the new id is guaranteed to see the slot it names.
    channels[(size_t) channel].sampleId.store (sampleId, std::memory_order_release);
}

Sample::Ptr SampleStore::getSample (int sampleId) const
{
    // Wrapping the raw pointer bumps the refcount, which cannot race with a free:
    // `owned` holds a reference to every published sample for the life of the
    // store, so the count can never reach zero underneath this call.
    return Sample::Ptr (slotFor (sampleId));
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
    juce::Array<int> ids;

    // Ids are handed out in order and never reused, so the slots up to the next id
    // are exactly the loaded samples, oldest first.
    for (int id = 1; id < nextSampleId; ++id)
        ids.add (id);

    return ids;
}

//==============================================================================
// Audio thread access

Sample::Ptr SampleStore::retainSampleForChannel (int channel) const
{
    if (! juce::isPositiveAndBelow (channel, Layout::numChannels))
        return nullptr;

    return getSample (channels[(size_t) channel].sampleId.load (std::memory_order_acquire));
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
