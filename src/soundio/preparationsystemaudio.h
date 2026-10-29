#pragma once

#ifdef Q_OS_MACOS
#include <CoreAudio/CoreAudio.h>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

inline bool preparationFollowsSystemOutput(const UserSettingsPointer& config) {
    QFile file(QDir(config->getSettingsPath()).filePath("quietvinyl-sync.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return false;
    const auto value = QJsonDocument::fromJson(file.readAll()).object();
    return value.value("mode") == "collective" && value.value("follow_system_output").toBool();
}

inline AudioDeviceID preparationSystemOutputId() {
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) != noErr) {
        return kAudioObjectUnknown;
    }
    return device;
}

// Re-enumerating PortAudio after a CoreAudio default-device change gives the
// correct default index even for identically named USB devices.
inline bool preparationSystemOutputConfig(SoundManager* manager, SoundManagerConfig* config) {
    const auto index = Pa_GetDefaultOutputDevice();
    for (const auto& device : manager->getDeviceList(MIXXX_PORTAUDIO_COREAUDIO_STRING, true, false)) {
        if (device->getDeviceId().portAudioIndex != index || device->getNumOutputChannels() < 1) continue;
        config->setAPI(MIXXX_PORTAUDIO_COREAUDIO_STRING);
        config->clearInputs();
        config->clearOutputs();
        const auto channels = device->getNumOutputChannels() >= 2
                ? mixxx::audio::ChannelCount::stereo() : mixxx::audio::ChannelCount::mono();
        config->addOutput(device->getDeviceId(), AudioOutput(AudioPathType::Main, 0, channels, 0));
        config->setSampleRate(device->getDefaultSampleRate());
        config->setForceNetworkClock(false);
        return true;
    }
    return false;
}
#endif
