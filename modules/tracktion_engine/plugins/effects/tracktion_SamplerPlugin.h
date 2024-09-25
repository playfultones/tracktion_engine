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

class SamplerPlugin  : public Plugin,
                       private juce::AsyncUpdater
{
public:
    SamplerPlugin (PluginCreationInfo);
    ~SamplerPlugin() override;

    //==============================================================================
    int getNumSounds() const;
    juce::String getSoundName (int index) const;
    void setSoundName (int index, const juce::String& name);
    AudioFile getSoundFile (int index) const;
    juce::String getSoundMedia (int index) const;
    int getKeyNote (int index) const;
    int getMinKey (int index) const;
    int getMaxKey (int index) const;
    float getSoundGainDb (int index) const;
    float getSoundPan (int index) const;
    bool isSoundOpenEnded (int index) const;
    double getSoundStartTime (int index) const;
    double getSoundLength (int index) const;
    void setSoundExcerpt (int index, double start, double length);

    // returns an error
    juce::String addSound (const juce::String& sourcePathOrProjectID, const juce::String& name,
                           double startTime, double length, float gainDb);
    virtual void removeSound (int index);
    void setSoundParams (int index, int keyNote, int minNote, int maxNote);
    void setSoundGains (int index, float gainDb, float pan);
    void setSoundOpenEnded (int index, bool isOpenEnded);
    void setSoundMedia (int index, const juce::String& sourcePathOrProjectID);

    void playNotes (const juce::BigInteger& keysDown);
    void allNotesOff();

    void setReleaseTimeSeconds (float seconds) noexcept;
    float getReleaseTimeSeconds() const noexcept;

    //==============================================================================
    static const char* getPluginName()                  { return NEEDS_TRANS("Sampler"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return TRANS("Sampler"); }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Smplr"; }
    juce::String getSelectableDescription() override    { return TRANS("Sampler"); }
    bool isSynth() override                             { return true; }
    bool needsConstantBufferSize() override             { return false; }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override { return juce::jmin (numInputChannels, 2); }
    void initialise (const PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const PluginRenderContext&) override;

    //==============================================================================
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool hasNameForMidiNoteNumber (int note, int midiChannel, juce::String& name) override;

    juce::Array<ReferencedItem> getReferencedItems() override;
    void reassignReferencedItem (const ReferencedItem&, ProjectItemID newID, double newStartTime) override;
    void sourceMediaChanged() override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    //==============================================================================
    struct SamplerSound
    {
        SamplerSound (SamplerPlugin&, const juce::String& sourcePathOrProjectID, const juce::String& name,
                      double startTime, double length, float gainDb);

        void setExcerpt (double startTime, double length);
        void refreshFile();

        SamplerPlugin& owner;
        juce::String source;
        juce::String name;
        int keyNote = -1, minNote = 0, maxNote = 0;
        int fileStartSample = 0, fileLengthSamples = 0;
        bool openEnded = false;
        float gainDb = 0, pan = 0;
        double startTime = 0, length = 0;
        AudioFile audioFile;
        juce::AudioBuffer<float> audioData { 2, 64 };

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerSound)
    };

protected:
    //==============================================================================
    struct SampledNote   : public ReferenceCountedObject
    {
    public:
        SampledNote (int midiNote, int keyNote,
            float velocity,
            const AudioFile& file,
            double sampleRate,
            int sampleDelayFromBufferStart,
            const juce::AudioBuffer<float>& data,
            int lengthInSamples,
            float gainDb,
            float pan,
            bool openEnded_)
            : note (midiNote),
              offset (-sampleDelayFromBufferStart),
              audioData (data),
              openEnded (openEnded_)
        {
            resampler[0].reset();
            resampler[1].reset();

            const float volumeSliderPos = decibelsToVolumeFaderPosition (gainDb - (20.0f * (1.0f - velocity)));
            getGainsFromVolumeFaderPositionAndPan (volumeSliderPos, pan, getDefaultPanLaw(), gains[0], gains[1]);

            const double hz = juce::MidiMessage::getMidiNoteInHertz (midiNote);
            playbackRatio = hz / juce::MidiMessage::getMidiNoteInHertz (keyNote);
            playbackRatio *= file.getSampleRate() / sampleRate;
            samplesLeftToPlay = playbackRatio > 0 ? (1 + (int) (lengthInSamples / playbackRatio)) : 0;
            sR = sampleRate;
            adsr.setSampleRate(sampleRate);
            juce::ADSR::Parameters params;
            params.attack = 0.f;
            params.decay = 0.f;
            params.sustain = 1.f;
            params.release = (float)samplesLeftToPlay / (float)sR;
            adsr.setParameters(params);
            adsr.noteOn();
        }

        virtual void addNextBlock (juce::AudioBuffer<float>& outBuffer, int startSamp, int numSamples)
        {
            jassert (! isFinished);

            if (offset < 0)
            {
                const int num = std::min (-offset, numSamples);
                startSamp += num;
                numSamples -= num;
                offset += num;
            }

            numSamps = std::min (numSamples, samplesLeftToPlay);

            if (numSamps > 0)
            {
                int numUsed = 0;

                for (int i = std::min (2, outBuffer.getNumChannels()); --i >= 0;)
                {
                    numUsed = resampler[i]
                                  .processAdding (playbackRatio,
                                      audioData.getReadPointer (std::min (i, audioData.getNumChannels() - 1), offset),
                                      outBuffer.getWritePointer (i, startSamp),
                                      numSamps,
                                      gains[i]);
                }

                if(releaseStageTriggered)
                    adsr.applyEnvelopeToBuffer(outBuffer, startSamp, numSamps);

                offset += numUsed;
                samplesLeftToPlay -= numSamps;

                jassert (offset <= audioData.getNumSamples());
            }

            if (numSamples > numSamps && startFade > 0.0f)
            {
                startSamp += numSamps;
                numSamps = numSamples - numSamps;
                float endFade;

                if (numSamps > 100)
                {
                    endFade = 0.0f;
                    numSamps = 100;
                }
                else
                {
                    endFade = std::max (0.0f, startFade - numSamps * 0.01f);
                }

                const int numSampsNeeded = 2 + juce::roundToInt ((numSamps + 2) * playbackRatio);
                AudioScratchBuffer scratch (audioData.getNumChannels(), numSampsNeeded + 8);

                if (offset + numSampsNeeded < audioData.getNumSamples())
                {
                    for (int i = scratch.buffer.getNumChannels(); --i >= 0;)
                        scratch.buffer.copyFrom (i, 0, audioData, i, offset, numSampsNeeded);
                }
                else
                {
                    scratch.buffer.clear();
                }

                if (numSampsNeeded > 2)
                    AudioFadeCurve::applyCrossfadeSection (scratch.buffer, 0, numSampsNeeded - 2,
                        AudioFadeCurve::linear, startFade, endFade);

                startFade = endFade;

                int numUsed = 0;

                for (int i = std::min (2, outBuffer.getNumChannels()); --i >= 0;)
                    numUsed = resampler[i].processAdding (playbackRatio,
                        scratch.buffer.getReadPointer (std::min (i, scratch.buffer.getNumChannels() - 1)),
                        outBuffer.getWritePointer (i, startSamp),
                        numSamps, gains[i]);

                if(releaseStageTriggered)
                    adsr.applyEnvelopeToBuffer(outBuffer, startSamp, numSamps);

                offset += numUsed;

                if (startFade <= 0.0f)
                    isFinished = true;
            }
        }

        void triggerRelease()
        {
            if (! releaseStageTriggered)
            {
                juce::ADSR::Parameters params;
                params.attack = 0.f;
                params.decay = 0.f;
                params.sustain = 1.f;
                params.release = (float)samplesLeftToPlay / (float)sR;
                adsr.setParameters(params);
                adsr.noteOff();
                releaseStageTriggered = true;
            }
        }

        juce::LagrangeInterpolator resampler[2];
        int note;
        int offset, samplesLeftToPlay = 0;
        float gains[2];
        double playbackRatio = 1.0;
        const juce::AudioBuffer<float>& audioData;
        float lastVals[4] = { 0, 0, 0, 0 };
        float startFade = 1.0f;
        bool openEnded, isFinished = false;
        int numSamps = 0;
        bool releaseStageTriggered = false;
        double sR = 0.0;
        juce::ADSR adsr;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampledNote)
    };

    juce::Colour colour;
    juce::CriticalSection lock;
    juce::ReferenceCountedArray<SampledNote> playingNotes;
    juce::OwnedArray<SamplerSound> soundList;
    juce::BigInteger highlightedNotes;

    juce::ValueTree getSound (int index) const;

    void valueTreeChanged() override;
    void handleAsyncUpdate() override;

    virtual SampledNote* createNote (int midiNote, int keyNote, float velocity, const AudioFile& file,
                                     double sampleRate, int sampleDelayFromBufferStart,
                                     const juce::AudioBuffer<float>& data, int lengthInSamples,
                                     float gainDb, float pan, bool openEnded);

    virtual void handleMessageBuffer(MidiMessageArray&);
    virtual void handleNoteOnMessage(MidiMessageArray::MidiMessageWithSource&);

    void handleOngoingNote (MidiMessageArray::MidiMessageWithSource&);
    void handleNoteOffMessage(MidiMessageArray::MidiMessageWithSource&);
    void handleMiscMessages(MidiMessageArray::MidiMessageWithSource&);

    // this must be high enough for low freq sounds not to click
    static inline constexpr int minimumSamplesToPlayWhenStopping = 8;
    static inline constexpr int maximumSimultaneousNotes = 32;

    std::atomic<float> releaseTimeSeconds = 0.f;
    int getMinimumSamplesToPlay() const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerPlugin)
};

}} // namespace tracktion { inline namespace engine
