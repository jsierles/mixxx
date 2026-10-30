#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>
#include <alsa/asoundlib.h>
#include <array>
#include <optional>

namespace quietvinyl {
inline QStringList denonHotplugSerials() {
    const auto document = QJsonDocument::fromJson(qgetenv("QUIETVINYL_DENON_PLAYERS"));
    if (!document.isArray() || document.array().isEmpty() || document.array().size() > 2) return {};
    QStringList result;
    for (const auto& value : document.array()) {
        const auto serial = value.toString();
        if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9]{1,64}$")).match(serial).hasMatch() || result.contains(serial)) return {};
        result.append(serial);
    }
    return result;
}
struct DenonMidiPort {
    int client, port;
    QString serial;
    bool operator==(const DenonMidiPort& other) const {
        return client == other.client && port == other.port && serial == other.serial;
    }
};
inline std::optional<DenonMidiPort> uniqueDenonPort(const QString& serial, const QList<DenonMidiPort>& ports) {
    std::optional<DenonMidiPort> result;
    for (const auto& port : ports) {
        if (port.serial != serial) continue;
        if (result) return {};
        result = port;
    }
    return result;
}
inline bool denonShortMessage(const snd_seq_event_t& event, std::array<unsigned char, 3>* bytes) {
    const auto channel = event.data.control.channel;
    if (channel > 15) return false;
    switch (event.type) {
    case SND_SEQ_EVENT_NOTEON:
    case SND_SEQ_EVENT_NOTEOFF:
    case SND_SEQ_EVENT_KEYPRESS:
        if (event.data.note.note > 127 || event.data.note.velocity > 127) return false;
        *bytes = {static_cast<unsigned char>((event.type == SND_SEQ_EVENT_NOTEON ? 0x90 : event.type == SND_SEQ_EVENT_NOTEOFF ? 0x80 : 0xA0) | channel), event.data.note.note, event.data.note.velocity};
        return true;
    case SND_SEQ_EVENT_CONTROLLER:
        if (event.data.control.param > 127 || event.data.control.value < 0 || event.data.control.value > 127) return false;
        *bytes = {static_cast<unsigned char>(0xB0 | channel), static_cast<unsigned char>(event.data.control.param), static_cast<unsigned char>(event.data.control.value)};
        return true;
    case SND_SEQ_EVENT_PGMCHANGE:
    case SND_SEQ_EVENT_CHANPRESS:
        if (event.data.control.value < 0 || event.data.control.value > 127) return false;
        *bytes = {static_cast<unsigned char>((event.type == SND_SEQ_EVENT_PGMCHANGE ? 0xC0 : 0xD0) | channel), static_cast<unsigned char>(event.data.control.value), 0};
        return true;
    case SND_SEQ_EVENT_PITCHBEND: {
        const int value = event.data.control.value + 8192;
        if (value < 0 || value > 16383) return false;
        *bytes = {static_cast<unsigned char>(0xE0 | channel), static_cast<unsigned char>(value & 127), static_cast<unsigned char>(value >> 7)};
        return true;
    }
    default: return false;
    }
}
} // namespace quietvinyl
