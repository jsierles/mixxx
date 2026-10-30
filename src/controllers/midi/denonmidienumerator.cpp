#include "controllers/midi/denonmidienumerator.h"
#include "controllers/midi/denonmidihotplug.h"
#include "controllers/midi/denonusbidentity.h"
#include "controllers/midi/midicontroller.h"
#include "util/time.h"
#include <QElapsedTimer>

namespace {
class DenonMidiController final : public MidiController {
  public:
    explicit DenonMidiController(const QString& serial)
            : MidiController(QStringLiteral("SC6000 Prime Controller ") + serial), m_serial(serial) {
        setInputDevice(true);
        setOutputDevice(false);
    }
    ~DenonMidiController() override { if (isOpen()) close(); }
    bool isPolling() const override { return true; }
    int open() override {
        if (isOpen()) return -1;
        startEngine();
        applyMapping();
        setOpen(true);
        m_scan.invalidate();
        return 0;
    }
    int close() override {
        stopEngine();
        MidiController::close();
        closeInput();
        setOpen(false);
        return 0;
    }
    bool poll() override {
        if (!m_scan.isValid() || m_scan.elapsed() >= 1000) {
            m_scan.start();
            reconnect();
        }
        if (!m_seq) return false;
        bool received = false;
        for (int count = 0; count < 256; ++count) {
            snd_seq_event_t* event = nullptr;
            const int result = snd_seq_event_input(m_seq, &event);
            if (result == -EAGAIN) break;
            if (result < 0) { closeInput(); break; }
            if (event->source.client == SND_SEQ_CLIENT_SYSTEM) {
                if (m_source && ((event->type == SND_SEQ_EVENT_CLIENT_EXIT && event->data.addr.client == m_source->client) ||
                        (event->type == SND_SEQ_EVENT_PORT_EXIT && event->data.addr.client == m_source->client && event->data.addr.port == m_source->port))) {
                    qCInfo(m_logBase) << "Denon MIDI disconnected:" << m_serial;
                    m_source.reset();
                }
                continue;
            }
            if (!m_source || event->source.client != m_source->client || event->source.port != m_source->port) continue;
            std::array<unsigned char, 3> bytes;
            if (quietvinyl::denonShortMessage(*event, &bytes)) {
                receivedShortMessage(bytes[0], bytes[1], bytes[2], mixxx::Time::elapsed());
                received = true;
            }
        }
        return received;
    }
  protected:
    void sendShortMsg(unsigned char, unsigned char, unsigned char) override {}
    void sendBytes(const QByteArray&) override {}
  private:
    void closeInput() {
        m_source.reset();
        if (m_seq) snd_seq_close(m_seq);
        m_seq = nullptr;
        m_port = -1;
    }
    void reconnect() {
        if (!m_seq) {
            if (snd_seq_open(&m_seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) { m_seq = nullptr; return; }
            snd_seq_set_client_name(m_seq, qPrintable(QStringLiteral("QuietVinyl Denon ") + m_serial));
            m_port = snd_seq_create_simple_port(m_seq, "Controls", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_APPLICATION);
            if (m_port < 0 || snd_seq_connect_from(m_seq, m_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE) < 0) { closeInput(); return; }
        }
        QList<quietvinyl::DenonMidiPort> ports;
        snd_seq_client_info_t* client; snd_seq_client_info_alloca(&client);
        snd_seq_port_info_t* port; snd_seq_port_info_alloca(&port);
        snd_seq_client_info_set_client(client, -1);
        while (snd_seq_query_next_client(m_seq, client) >= 0) {
            if (snd_seq_client_info_get_type(client) != SND_SEQ_KERNEL_CLIENT) continue;
            const int id = snd_seq_client_info_get_client(client);
            snd_seq_port_info_set_client(port, id); snd_seq_port_info_set_port(port, -1);
            while (snd_seq_query_next_port(m_seq, port) >= 0) {
                const int caps = snd_seq_port_info_get_capability(port);
                if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ) ||
                        QString::fromLocal8Bit(snd_seq_port_info_get_name(port)) != QStringLiteral("SC6000 Prime Controller")) continue;
                ports.append({id, snd_seq_port_info_get_port(port), quietvinyl::denonSerialForCard(snd_seq_client_info_get_card(client))});
            }
        }
        const auto next = quietvinyl::uniqueDenonPort(m_serial, ports);
        if (m_source && next && *m_source == *next) {
            snd_seq_port_subscribe_t* subscription; snd_seq_port_subscribe_alloca(&subscription);
            const snd_seq_addr_t sender{static_cast<unsigned char>(next->client), static_cast<unsigned char>(next->port)};
            const snd_seq_addr_t destination{static_cast<unsigned char>(snd_seq_client_id(m_seq)), static_cast<unsigned char>(m_port)};
            snd_seq_port_subscribe_set_sender(subscription, &sender);
            snd_seq_port_subscribe_set_dest(subscription, &destination);
            if (snd_seq_get_port_subscription(m_seq, subscription) >= 0) return;
        }
        if (m_source) snd_seq_disconnect_from(m_seq, m_port, m_source->client, m_source->port);
        m_source.reset();
        // An ALSA client number may be reused after unplugging. Discard the old
        // connection's queued input before subscribing to its replacement.
        snd_seq_drop_input(m_seq);
        if (next && snd_seq_connect_from(m_seq, m_port, next->client, next->port) >= 0) {
            m_source = next;
            qCInfo(m_logBase) << "Denon MIDI connected:" << m_serial << next->client << next->port;
        }
    }
    QString m_serial;
    QElapsedTimer m_scan;
    snd_seq_t* m_seq = nullptr;
    int m_port = -1;
    std::optional<quietvinyl::DenonMidiPort> m_source;
};
} // namespace

DenonMidiEnumerator::DenonMidiEnumerator(const QStringList& serials) {
    for (const auto& serial : serials) m_devices.append(new DenonMidiController(serial));
}
DenonMidiEnumerator::~DenonMidiEnumerator() { qDeleteAll(m_devices); }
