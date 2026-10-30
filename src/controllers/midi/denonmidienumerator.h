#pragma once
#include "controllers/midi/midienumerator.h"
#include <QStringList>

// Stable logical controllers retain their mapping while physical ALSA ports
// disappear/reappear. No PortMidi-wide reset or controller-list replacement.
class DenonMidiEnumerator final : public MidiEnumerator {
  public:
    explicit DenonMidiEnumerator(const QStringList& serials);
    ~DenonMidiEnumerator() override;
    QList<Controller*> queryDevices() override { return m_devices; }
  private:
    QList<Controller*> m_devices;
};
