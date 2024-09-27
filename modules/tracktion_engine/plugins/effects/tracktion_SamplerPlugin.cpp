/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2018
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion { inline namespace engine
{

//==============================================================================
SamplerPlugin::SamplerPlugin (PluginCreationInfo info)  : Plugin (info)
{
    triggerAsyncUpdate();
}

SamplerPlugin::~SamplerPlugin()
{
    notifyListenersOfDeletion();
}

const char* SamplerPlugin::xmlTypeName = "sampler";

void SamplerPlugin::valueTreeChanged()
{
    triggerAsyncUpdate();
    Plugin::valueTreeChanged();
}

void SamplerPlugin::handleAsyncUpdate()
{
    juce::OwnedArray<SamplerSound> newSounds;

    auto numSounds = state.getNumChildren();

    for (int i = 0; i < numSounds; ++i)
    {
        auto v = getSound (i);

        if (v.hasType (IDs::SOUND))
        {
            auto s = new SamplerSound (*this,
                                       v[IDs::source].toString(),
                                       v[IDs::name],
                                       v[IDs::startTime],
                                       v[IDs::length],
                                       v[IDs::gainDb]);

            s->keyNote      = juce::jlimit (0, 127, static_cast<int> (v[IDs::keyNote]));
            s->minNote      = juce::jlimit (0, 127, static_cast<int> (v[IDs::minNote]));
            s->maxNote      = juce::jlimit (0, 127, static_cast<int> (v[IDs::maxNote]));
            s->pan          = juce::jlimit (-1.0f, 1.0f, static_cast<float> (v[IDs::pan]));
            s->openEnded    = v[IDs::openEnded];

            newSounds.add (s);
        }
    }

    for (auto newSound : newSounds)
    {
        for (auto s : soundList)
        {
            if (s->source == newSound->source
                && s->startTime == newSound->startTime
                && s->length == newSound->length)
            {
                newSound->audioFile = s->audioFile;
                newSound->fileStartSample = s->fileStartSample;
                newSound->fileLengthSamples = s->fileLengthSamples;
                newSound->audioData = s->audioData;
            }
        }
    }

    {
        const juce::ScopedLock sl (lock);
        allNotesOff();
        soundList.swapWith (newSounds);

        sourceMediaChanged();
    }

    newSounds.clear();
    changed();
}

void SamplerPlugin::initialise (const PluginInitialisationInfo& pII)
{
    const juce::ScopedLock sl (lock);
    allNotesOff();
}

void SamplerPlugin::deinitialise()
{
    allNotesOff();
}

//==============================================================================
void SamplerPlugin::playNotes (const juce::BigInteger& keysDown)
{
    const juce::ScopedLock sl (lock);

    if (highlightedNotes != keysDown)
    {
        for (int i = playingNotes.size(); --i >= 0;)
            if ((! keysDown [playingNotes.getUnchecked(i)->note])
                 && highlightedNotes [playingNotes.getUnchecked(i)->note]
                 && ! playingNotes.getUnchecked(i)->openEnded)
                playingNotes.getUnchecked(i)->samplesLeftToPlay = minimumSamplesToPlayWhenStopping;

        for (int note = 128; --note >= 0;)
        {
            if (keysDown [note] && ! highlightedNotes [note])
            {
                for (auto ss : soundList)
                {
                    if (ss->minNote <= note
                         && ss->maxNote >= note
                         && ss->audioData.getNumSamples() > 0
                         && (! ss->audioFile.isNull())
                         && playingNotes.size() < maximumSimultaneousNotes)
                    {
                        playingNotes.add (createNote (note,
                                                           ss->keyNote,
                                                           0.75f,
                                                           ss->audioFile,
                                                           sampleRate,
                                                           0,
                                                           ss->audioData,
                                                           ss->fileLengthSamples,
                                                           ss->gainDb,
                                                           ss->pan,
                                                           ss->openEnded));
                    }
                }
            }
        }

        highlightedNotes = keysDown;
    }
}

void SamplerPlugin::allNotesOff()
{
    const juce::ScopedLock sl (lock);
    playingNotes.clear();
    highlightedNotes.clear();
}

void SamplerPlugin::applyToBuffer (const PluginRenderContext& fc)
{
    if (fc.destBuffer != nullptr)
    {
        SCOPED_REALTIME_CHECK

        const juce::ScopedLock sl (lock);

        clearChannels (*fc.destBuffer, 2, -1, fc.bufferStartSample, fc.bufferNumSamples);

        if (fc.bufferForMidiMessages != nullptr)
        {
            if (fc.bufferForMidiMessages->isAllNotesOff)
            {
                playingNotes.clear();
                highlightedNotes.clear();
            }

            handleMessageBuffer(*fc.bufferForMidiMessages);
        }

        for (int i = playingNotes.size(); --i >= 0;)
        {
            auto sn = playingNotes.getUnchecked (i);

            sn->addNextBlock (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples);

            if (sn->isFinished)
                playingNotes.remove (i);
        }
    }
}

//==============================================================================
int SamplerPlugin::getNumSounds() const
{
    return std::accumulate (state.begin(), state.end(), 0,
                            [] (int total, auto v) { return total + (v.hasType (IDs::SOUND) ? 1 : 0); });
}

juce::String SamplerPlugin::getSoundName (int index) const
{
    return getSound (index)[IDs::name];
}

void SamplerPlugin::setSoundName (int index, const juce::String& n)
{
    getSound (index).setProperty (IDs::name, n, getUndoManager());
}

bool SamplerPlugin::hasNameForMidiNoteNumber (int note, int, juce::String& noteName)
{
    juce::String s;

    {
        const juce::ScopedLock sl (lock);

        for (auto ss : soundList)
        {
            if (ss->minNote <= note && ss->maxNote >= note)
            {
                if (s.isNotEmpty())
                    s << " + " << ss->name;
                else
                    s = ss->name;
            }
        }
    }

    noteName = s;
    return true;
}

AudioFile SamplerPlugin::getSoundFile (int index) const
{
    const juce::ScopedLock sl (lock);

    if (auto s = soundList[index])
        return s->audioFile;

    return AudioFile (edit.engine);
}

juce::String SamplerPlugin::getSoundMedia (int index) const
{
    const juce::ScopedLock sl (lock);

    if (auto s = soundList[index])
        return s->source;

    return {};
}

int SamplerPlugin::getKeyNote (int index) const             { return getSound (index)[IDs::keyNote]; }
int SamplerPlugin::getMinKey (int index) const              { return getSound (index)[IDs::minNote]; }
int SamplerPlugin::getMaxKey (int index) const              { return getSound (index)[IDs::maxNote]; }
float SamplerPlugin::getSoundGainDb (int index) const       { return getSound (index)[IDs::gainDb]; }
float SamplerPlugin::getSoundPan (int index) const          { return getSound (index)[IDs::pan]; }
double SamplerPlugin::getSoundStartTime (int index) const   { return getSound (index)[IDs::startTime]; }
bool SamplerPlugin::isSoundOpenEnded (int index) const      { return getSound (index)[IDs::openEnded]; }

double SamplerPlugin::getSoundLength (int index) const
{
    const double l = getSound (index)[IDs::length];

    if (l == 0.0)
    {
        const juce::ScopedLock sl (lock);

        if (auto s = soundList[index])
            return s->length;
    }

    return l;
}

juce::String SamplerPlugin::addSound (const juce::String& source, const juce::String& name,
                                      double startTime, double length, float gainDb)
{
    const int maxNumSamples = 64;

    if (getNumSounds() >= maxNumSamples)
        return TRANS("Can't load any more samples");

    auto v = createValueTree (IDs::SOUND,
                              IDs::source, source,
                              IDs::name, name,
                              IDs::startTime, startTime,
                              IDs::length, length,
                              IDs::keyNote, 72,
                              IDs::minNote, 72 - 24,
                              IDs::maxNote, 72 + 24,
                              IDs::gainDb, gainDb,
                              IDs::pan, (double) 0);

    state.addChild (v, -1, getUndoManager());
    return {};
}

void SamplerPlugin::removeSound (int index)
{
    state.removeChild (index, getUndoManager());

    const juce::ScopedLock sl (lock);
    playingNotes.clear();
    highlightedNotes.clear();
}

void SamplerPlugin::setSoundParams (int index, int keyNote, int minNote, int maxNote)
{
    auto um = getUndoManager();

    auto v = getSound (index);
    v.setProperty (IDs::keyNote, juce::jlimit (0, 127, keyNote), um);
    v.setProperty (IDs::minNote, juce::jlimit (0, 127, std::min (minNote, maxNote)), um);
    v.setProperty (IDs::maxNote, juce::jlimit (0, 127, std::max (minNote, maxNote)), um);
}

void SamplerPlugin::setSoundGains (int index, float gainDb, float pan)
{
    auto um = getUndoManager();

    auto v = getSound (index);
    v.setProperty (IDs::gainDb, juce::jlimit (-48.0f, 48.0f, gainDb), um);
    v.setProperty (IDs::pan,    juce::jlimit (-1.0f,  1.0f,  pan), um);
}

void SamplerPlugin::setSoundExcerpt (int index, double start, double length)
{
    auto um = getUndoManager();

    auto v = getSound (index);
    v.setProperty (IDs::startTime, start, um);
    v.setProperty (IDs::length, length, um);
}

void SamplerPlugin::setSoundOpenEnded (int index, bool b)
{
    auto um = getUndoManager();

    auto v = getSound (index);
    v.setProperty (IDs::openEnded, b, um);
}

void SamplerPlugin::setSoundMedia (int index, const juce::String& source)
{
    auto v = getSound (index);
    v.setProperty (IDs::source, source, getUndoManager());
    triggerAsyncUpdate();
}

juce::ValueTree SamplerPlugin::getSound (int soundIndex) const
{
    int index = 0;

    for (auto v : state)
        if (v.hasType (IDs::SOUND))
            if (index++ == soundIndex)
                return v;

    return {};
}

//==============================================================================
juce::Array<Exportable::ReferencedItem> SamplerPlugin::getReferencedItems()
{
    juce::Array<ReferencedItem> results;

    // must be careful to generate this list in the right order..
    for (int i = 0; i < getNumSounds(); ++i)
    {
        auto v = getSound (i);

        Exportable::ReferencedItem ref;
        ref.itemID = ProjectItemID::fromProperty (v, IDs::source);
        ref.firstTimeUsed = v[IDs::startTime];
        ref.lengthUsed = v[IDs::length];
        results.add (ref);
    }

    return results;
}

void SamplerPlugin::reassignReferencedItem (const ReferencedItem& item, ProjectItemID newID, double newStartTime)
{
    auto index = getReferencedItems().indexOf (item);

    if (index >= 0)
    {
        auto um = getUndoManager();

        auto v = getSound (index);
        v.setProperty (IDs::source, newID.toString(), um);
        v.setProperty (IDs::startTime, static_cast<double> (v[IDs::startTime]) - newStartTime, um);
    }
    else
    {
        jassertfalse;
    }
}

void SamplerPlugin::sourceMediaChanged()
{
    const juce::ScopedLock sl (lock);

    for (auto s : soundList)
        s->refreshFile();
}

void SamplerPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    copyValueTree (state, v, getUndoManager());
}

//==============================================================================
SamplerPlugin::SamplerSound::SamplerSound (SamplerPlugin& sf,
                                           const juce::String& source_,
                                           const juce::String& name_,
                                           const double startTime_,
                                           const double length_,
                                           const float gainDb_)
    : owner (sf),
      source (source_),
      name (name_),
      gainDb (juce::jlimit (-48.0f, 48.0f, gainDb_)),
      startTime (startTime_),
      length (length_),
      audioFile (owner.edit.engine, SourceFileReference::findFileFromString (owner.edit, source))
{
    setExcerpt (startTime_, length_);

    keyNote = audioFile.getInfo().loopInfo.getRootNote();

    if (keyNote < 0)
        keyNote = 72;

    maxNote = keyNote + 24;
    minNote = keyNote - 24;
}

void SamplerPlugin::SamplerSound::setExcerpt (double startTime_, double length_)
{
    CRASH_TRACER

    if (! audioFile.isValid())
    {
        audioFile = AudioFile (owner.edit.engine, SourceFileReference::findFileFromString (owner.edit, source));

       #if JUCE_DEBUG
        if (! audioFile.isValid() && ProjectItemID (source).isValid())
            DBG ("Failed to find media: " << source);
       #endif
    }

    if (audioFile.isValid())
    {
        const double minLength = 32.0 / audioFile.getSampleRate();

        startTime = juce::jlimit (0.0, audioFile.getLength() - minLength, startTime_);

        if (length_ > 0)
            length = juce::jlimit (minLength, audioFile.getLength() - startTime, length_);
        else
            length = audioFile.getLength();

        fileStartSample   = juce::roundToInt (startTime * audioFile.getSampleRate());
        fileLengthSamples = juce::roundToInt (length * audioFile.getSampleRate());

        if (auto reader = owner.engine.getAudioFileManager().cache.createReader (audioFile))
        {
            audioData.setSize (audioFile.getNumChannels(), fileLengthSamples + 32);
            audioData.clear();

            auto audioDataChannelSet = juce::AudioChannelSet::canonicalChannelSet (audioFile.getNumChannels());
            auto channelsToUse = juce::AudioChannelSet::stereo();

            int total = fileLengthSamples;
            int offset = 0;

            while (total > 0)
            {
                const int numThisTime = std::min (8192, total);
                reader->setReadPosition (fileStartSample + offset);

                if (! reader->readSamples (numThisTime, audioData, audioDataChannelSet, offset, channelsToUse, 2000))
                {
                    jassertfalse;
                    break;
                }

                offset += numThisTime;
                total -= numThisTime;
            }
        }
        else
        {
            audioData.clear();
        }

        // add a quick fade-in if needed..
        int fadeLen = 0;
        for (int i = audioData.getNumChannels(); --i >= 0;)
        {
            const float* d = audioData.getReadPointer (i);

            if (std::abs (*d) > 0.01f)
                fadeLen = 30;
        }

        if (fadeLen > 0)
            AudioFadeCurve::applyCrossfadeSection (audioData, 0, fadeLen, AudioFadeCurve::concave, 0.0f, 1.0f);
    }
    else
    {
        audioFile = AudioFile (owner.edit.engine);
    }
}

void SamplerPlugin::SamplerSound::refreshFile()
{
    audioFile = AudioFile (owner.edit.engine);
    setExcerpt (startTime, length);
}

SamplerPlugin::SampledNote* SamplerPlugin::createNote (int midiNote, int keyNote, float velocity, const tracktion::AudioFile& file, double sR, int sampleDelayFromBufferStart, const juce::AudioBuffer<float>& data, int lengthInSamples, float gainDb, float pan, bool openEnded) {
    return new SampledNote(midiNote, keyNote, velocity, file, sR, sampleDelayFromBufferStart, data, lengthInSamples, gainDb, pan, openEnded, getADSRParams());
}

void SamplerPlugin::handleNoteOnMessage (tracktion::MidiMessageArray::MidiMessageWithSource& m) {
    const int note = m.getNoteNumber();
    const int noteTimeSample = juce::roundToInt (m.getTimeStamp() * sampleRate);

    handleOngoingNote(m);

    for (auto ss : soundList)
    {
        if (ss->minNote <= note
            && ss->maxNote >= note
            && ss->audioData.getNumSamples() > 0
            && playingNotes.size() < maximumSimultaneousNotes)
        {
            highlightedNotes.setBit (note);

            playingNotes.add (createNote (note,
                ss->keyNote,
                m.getVelocity() / 127.0f,
                ss->audioFile,
                sampleRate,
                noteTimeSample,
                ss->audioData,
                ss->fileLengthSamples,
                ss->gainDb,
                ss->pan,
                ss->openEnded));
        }
    }
}

void SamplerPlugin::handleOngoingNote (MidiMessageArray::MidiMessageWithSource& m) {
    const int note = m.getNoteNumber();
    const int noteTimeSample = juce::roundToInt (m.getTimeStamp() * sampleRate);

    for (auto playingNote : playingNotes)
    {
        if (playingNote->note == note && ! playingNote->openEnded)
        {
            playingNote->samplesLeftToPlay = std::min (playingNote->samplesLeftToPlay,
                std::max (getMinimumSamplesToPlay(),
                    noteTimeSample));
            playingNote->adsr.noteOff();
            highlightedNotes.clearBit (note);
        }
    }
}

void SamplerPlugin::handleNoteOffMessage (MidiMessageArray::MidiMessageWithSource& m) {
    const int note = m.getNoteNumber();
    const int noteTimeSample = juce::roundToInt (m.getTimeStamp() * sampleRate);

    for (auto playingNote : playingNotes)
    {
        if (playingNote->note == note && ! playingNote->openEnded)
        {
            playingNote->samplesLeftToPlay = std::min (playingNote->samplesLeftToPlay,
                std::max (getMinimumSamplesToPlay(),
                    noteTimeSample));
            playingNote->adsr.noteOff();

            highlightedNotes.clearBit (note);
        }
    }
}

void SamplerPlugin::handleMiscMessages (MidiMessageArray::MidiMessageWithSource& m) {
    if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        playingNotes.clear();
        highlightedNotes.clear();
    }
}

void SamplerPlugin::handleMessageBuffer (MidiMessageArray& bufferForMidiMessages) {
    for (auto& m : bufferForMidiMessages)
    {
        if (m.isNoteOn())
        {
            handleNoteOnMessage(m);
        }
        else if (m.isNoteOff())
        {
            handleNoteOffMessage(m);
        }
        else
        {
            handleMiscMessages(m);
        }
    }
}
void SamplerPlugin::setAttackTime (float seconds) noexcept {
    attackTimeSeconds.store(seconds);
}

void SamplerPlugin::setDecayTime (float seconds) noexcept {
    decayTimeSeconds.store(seconds);
}

void SamplerPlugin::setSustainLevel (float level) noexcept {
    sustainLevel.store(level);
}

void SamplerPlugin::setReleaseTime (float seconds) noexcept {
    releaseTimeSeconds.store(seconds);
}

juce::ADSR::Parameters SamplerPlugin::getADSRParams() const noexcept {
    juce::ADSR::Parameters params;
    params.attack = attackTimeSeconds.load();
    params.decay = decayTimeSeconds.load();
    params.sustain = sustainLevel.load();
    params.release = releaseTimeSeconds.load();
    return params;
}

int SamplerPlugin::getMinimumSamplesToPlay() const noexcept {
    return std::max(minimumSamplesToPlayWhenStopping, static_cast<int>(releaseTimeSeconds * sampleRate));
}

}} // namespace tracktion { inline namespace engine
