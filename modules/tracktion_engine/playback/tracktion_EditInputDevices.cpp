/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion { inline namespace engine
{

EditInputDevices::EditInputDevices (Edit& e, const juce::ValueTree& v)
    : edit (e), state (v)
{
    editState.addListener (this);

    if (edit.shouldPlay())
        callBlocking ([this] { edit.engine.getDeviceManager().addChangeListener (this); });
}

EditInputDevices::~EditInputDevices()
{
    cancelPendingUpdate();
    edit.engine.getDeviceManager().removeChangeListener (this);
    removeNonExistantInputDeviceStates();
}

int EditInputDevices::getMaxNumInputs() const
{
    return 4;
}

static bool isForDevice (const juce::ValueTree& v, const InputDevice& d, bool fallbackToNameCheck)
{
    auto typeProp = v.getPropertyPointer (IDs::type);

    if (d.getDeviceType() == InputDevice::DeviceType::trackWaveDevice)
        return typeProp != nullptr && *typeProp == "audio" && v[IDs::sourceTrack] == d.getName();

    if (d.getDeviceType() == InputDevice::DeviceType::trackMidiDevice)
        return typeProp != nullptr && *typeProp == "MIDI" && v[IDs::sourceTrack] == d.getName();

    if (v[IDs::deviceID] == d.getDeviceID())
        return true;

    return fallbackToNameCheck && v[IDs::name] == d.getName();
}

static juce::ValueTree findDeviceState (const juce::ValueTree& parent, const InputDevice& d)
{
    for (const auto& v : parent)
        if (isForDevice (v, d, false))
            return v;

    for (const auto& v : parent)
        if (isForDevice (v, d, true))
            return v;

    return {};
}

static bool isTrackInputDeviceMIDI (const juce::ValueTree& v)
{
    return v[IDs::type].toString().trim() == "MIDI";
}

bool EditInputDevices::isInputDeviceAssigned (const InputDevice& d)
{
    return findDeviceState (state, d).isValid();
}

void EditInputDevices::clearAllInputs (AudioTrack& at, juce::UndoManager* um)
{
    for (auto idi : getDevicesForTargetTrack (at))
        [[ maybe_unused]] auto res = idi->removeTarget (at.itemID, um);
}

static bool isInstanceRecording (InputDeviceInstance* idi)
{
    if (idi->isRecording())
    {
        idi->edit.engine.getUIBehaviour().showWarningMessage (TRANS("Can't change tracks whilst recording is active"));
        return false;
    }

    return true;
}

void EditInputDevices::clearInputsOfDevice (AudioTrack& at, const InputDevice& d, juce::UndoManager* um)
{
    for (auto idi : getDevicesForTargetTrack (at))
        if (&idi->owner == &d)
            if (! isInstanceRecording (idi))
                [[ maybe_unused ]] auto res = idi->removeTarget (at.itemID, um);
}

InputDeviceInstance* EditInputDevices::getInputInstance (const AudioTrack& at, int index) const
{
    for (auto idi : getDevicesForTargetTrack (at))
        if (isOnTargetTrack (*idi, at, index))
            return idi;

    return {};
}

juce::Array<InputDeviceInstance*> EditInputDevices::getDevicesForTargetTrack (const AudioTrack& at) const
{
    juce::Array<InputDeviceInstance*> devices;

    for (auto idi : edit.getAllInputDevices())
        if (idi->getTargets().contains (at.itemID))
            devices.add (idi);

    return devices;
}

juce::ValueTree EditInputDevices::getInstanceStateForInputDevice (const InputDevice& d)
{
    if (auto v = findDeviceState (state, d); v.isValid())
    {
        // Refresh the ID to update from legacy edits
        v.setProperty (IDs::deviceID, d.getDeviceID(), nullptr);
        v.setProperty (IDs::name, d.getName(), nullptr);
        return v;
    }

    juce::ValueTree v (IDs::INPUTDEVICE);

    if (d.getDeviceType() == InputDevice::DeviceType::trackWaveDevice)
    {
        v.setProperty (IDs::sourceTrack, EditItemID::fromString (d.getName()), nullptr);
        v.setProperty (IDs::type, "audio", nullptr);
    }
    else if (d.getDeviceType() == InputDevice::DeviceType::trackMidiDevice)
    {
        v.setProperty (IDs::sourceTrack, EditItemID::fromString (d.getName()), nullptr);
        v.setProperty (IDs::type, "MIDI", nullptr);
    }
    else
    {
        v.setProperty (IDs::deviceID, d.getDeviceID(), nullptr);
        v.setProperty (IDs::name, d.getName(), nullptr);
    }

    state.addChild (v, -1, nullptr);
    return v;
}

//==============================================================================
void EditInputDevices::removeNonExistantInputDeviceStates()
{
    auto& dm = edit.engine.getDeviceManager();
    juce::Array<InputDevice*> devices;

    for (auto& d : dm.midiInputs)
        devices.add (d.get());

    devices.addArray (dm.waveInputs);

    if (! edit.isLoading())
    {
        for (auto* at : getAudioTracks (edit))
        {
            if (auto* wid = &at->getWaveInputDevice())
                if (wid->isEnabled())
                    devices.add (wid);

            if (auto* mid = &at->getMidiInputDevice())
                if (mid->isEnabled())
                    devices.add (mid);
        }
    }

    auto isDevicePresent = [devices] (const juce::ValueTree& v)
    {
        for (auto* d : devices)
            if (isForDevice (v, *d, true))
                return true;

        return false;
    };

    for (int i = state.getNumChildren(); --i >= 0;)
        if (! isDevicePresent (state.getChild(i)))
            state.removeChild (i, nullptr);
}

void EditInputDevices::addTrackDeviceInstanceToContext (const juce::ValueTree& v) const
{
    if (auto* id = getTrackDeviceForState (v))
    {
        if (auto* epc = edit.getCurrentPlaybackContext())
        {
            if (isTrackInputDeviceMIDI (v))
                epc->addMidiInputDeviceInstance (*id);
            else
                epc->addWaveInputDeviceInstance (*id);
        }
    }
}

void EditInputDevices::removeTrackDeviceInstanceToContext (const juce::ValueTree& v) const
{
    if (auto* id = getTrackDeviceForState (v))
        if (auto* epc = edit.getCurrentPlaybackContext())
            epc->removeInstanceForDevice (*id);
}

InputDevice* EditInputDevices::getTrackDeviceForState (const juce::ValueTree& v) const
{
    auto trackID = EditItemID::fromProperty (v, IDs::sourceTrack);

    if (trackID.isValid())
    {
        if (auto at = findAudioTrackForID (edit, trackID))
        {
            if (isTrackInputDeviceMIDI (v))
                return &at->getMidiInputDevice();

            return &at->getWaveInputDevice();
        }
    }

    return {};
}

void EditInputDevices::changeListenerCallback (juce::ChangeBroadcaster*)
{
    removeNonExistantInputDeviceStates();
}

void EditInputDevices::handleAsyncUpdate()
{
    removeNonExistantInputDeviceStates();
}

static bool isTrackDevice (const juce::ValueTree& v)
{
    jassert (v.hasType (IDs::INPUTDEVICE));
    return v.hasProperty (IDs::sourceTrack);
}

void EditInputDevices::valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree& c)
{
    if (p == state)
        if (c.hasType (IDs::INPUTDEVICE) && isTrackDevice (c))
            addTrackDeviceInstanceToContext (c);
}

void EditInputDevices::valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree& c, int)
{
    if (p == state)
        if (c.hasType (IDs::INPUTDEVICE) && isTrackDevice (c))
            removeTrackDeviceInstanceToContext (c);

    if (TrackList::isTrack (c))
        triggerAsyncUpdate();
}

}} // namespace tracktion { inline namespace engine
