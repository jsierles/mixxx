#include "controllers/midi/portmidienumerator.h"

#include <portmidi.h>

#include <QRegularExpression>

#ifdef __LINUX__
#include <alsa/asoundlib.h>
#include "controllers/midi/denonusbidentity.h"
#include "controllers/midi/denonmidihotplug.h"
#endif

#include "controllers/midi/portmidicontroller.h"
#include "moc_portmidienumerator.cpp"
#include "util/cmdlineargs.h"

namespace {

const auto kMidiThroughPortPrefix = QLatin1String("MIDI Through Port");

bool recognizeDevice(const PmDeviceInfo& deviceInfo) {
#ifdef __LINUX__
    // The opt-in serial controllers retain their ALSA input across hotplug.
    // Never open the same physical input through PortMidi as well.
    if (!quietvinyl::denonHotplugSerials().isEmpty() &&
            QString::fromLatin1(deviceInfo.interf) == QStringLiteral("ALSA") &&
            QString::fromLocal8Bit(deviceInfo.name) == QStringLiteral("SC6000 Prime Controller")) return false;
#endif
    // In developer mode we show the MIDI Through Port, otherwise ignore it
    // since it routinely causes trouble.
    return CmdlineArgs::Instance().getDeveloper() ||
            !QLatin1String(deviceInfo.name)
                     .startsWith(kMidiThroughPortPrefix, Qt::CaseInsensitive);
}

// Some platforms format MIDI device names as "deviceName MIDI ###" where
// ### is the instance # of the device. Therefore we want to link two
// devices that have an equivalent "deviceName" and ### section.
const QRegularExpression kMidiDeviceNameRegex(QStringLiteral("^(.*) MIDI (\\d+)( .*)?$"));

const QRegularExpression kInputRegex(QStringLiteral("^(.*) in( \\d+)?( .*)?$"),
        QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kOutputRegex(QStringLiteral("^(.*) out( \\d+)?( .*)?$"),
        QRegularExpression::CaseInsensitiveOption);

// This is a broad pattern that matches a text blob followed by a numeral
// potentially followed by non-numeric text. The non-numeric requirement is
// meant to avoid corner cases around devices with names like "Hercules RMX
// 2" where we would potentially confuse the number in the device name as
// the ordinal index of the device.
const QRegularExpression kDeviceNameRegex(QStringLiteral("^(.*) (\\d+)( [^0-9]+)?$"));

bool namesMatchRegexes(const QRegularExpression& kInputRegex,
        const QString& input_name,
        const QRegularExpression& kOutputRegex,
        const QString& output_name) {
    QRegularExpressionMatch inputMatch = kInputRegex.match(input_name);
    if (inputMatch.hasMatch()) {
        QString inputDeviceName = inputMatch.captured(1);
        QString inputDeviceIndex = inputMatch.captured(2);
        QRegularExpressionMatch outputMatch = kOutputRegex.match(output_name);
        if (outputMatch.hasMatch()) {
            QString outputDeviceName = outputMatch.captured(1);
            QString outputDeviceIndex = outputMatch.captured(2);
            if (outputDeviceName.compare(inputDeviceName, Qt::CaseInsensitive) == 0 &&
                outputDeviceIndex == inputDeviceIndex) {
                return true;
            }
        }
    }
    return false;
}

bool namesMatchMidiPattern(const QString& input_name,
        const QString& output_name) {
    return namesMatchRegexes(kMidiDeviceNameRegex, input_name, kMidiDeviceNameRegex, output_name);
}

bool namesMatchInOutPattern(const QString& input_name,
        const QString& output_name) {
    return namesMatchRegexes(kInputRegex, input_name, kOutputRegex, output_name);
}

bool namesMatchPattern(const QString& input_name,
        const QString& output_name) {
    return namesMatchRegexes(kDeviceNameRegex, input_name, kDeviceNameRegex, output_name);
}

bool namesMatchAllowableEdgeCases(const QString& input_name,
        const QString& output_name) {
    // Mac OS 10.12 & Korg Kaoss DJ 1.6:
    // Korg Kaoss DJ has input 'KAOSS DJ CONTROL' and output 'KAOSS DJ SOUND'.
    // This means it doesn't pass the shouldLinkInputToOutput test. Without an
    // output linked, the MIDI output for the device fails, as the device is
    // NULL in PortMidiController
    if (input_name == "KAOSS DJ CONTROL" && output_name == "KAOSS DJ SOUND") {
        return true;
    }
    // Ableton Push on Windows
    // Shows 2 different devices for MIDI input and output.
    if (input_name == "MIDIIN2 (Ableton Push)" && output_name == "MIDIOUT2 (Ableton Push)") {
        return true;
    }

    // Novation Launchpad X (macOS)
    if (input_name == "Launchpad X LPX DAW Out" && output_name == "Launchpad X LPX DAW In") {
        return true;
    }

    return false;
}

// PortMidi's ALSA backend enumerates clients and ports in ascending order.
// Match the complete list of identically named inputs before attaching serials;
// never guess from a sound-card number or USB enumeration order.
QMap<int, QString> denonIdentityNames() {
    QMap<int, QString> result;
#ifdef __LINUX__
    const QString name = QStringLiteral("SC6000 Prime Controller");
    QList<int> indices;
    for (int i = 0; i < Pm_CountDevices(); ++i) {
        const auto* info = Pm_GetDeviceInfo(i);
        if (info && info->input && QString::fromLatin1(info->interf) == QStringLiteral("ALSA") &&
                QString::fromLocal8Bit(info->name) == name) indices.append(i);
    }
    if (indices.isEmpty()) return result;
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) return result;
    snd_seq_client_info_t* client;
    snd_seq_port_info_t* port;
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);
    QList<QString> serials;
    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(seq, client) == 0) {
        snd_seq_port_info_set_client(port, snd_seq_client_info_get_client(client));
        snd_seq_port_info_set_port(port, -1);
        while (snd_seq_query_next_port(seq, port) == 0) {
            if (!(snd_seq_port_info_get_capability(port) & SND_SEQ_PORT_CAP_SUBS_READ) ||
                    QString::fromLocal8Bit(snd_seq_port_info_get_name(port)) != name) continue;
            serials.append(quietvinyl::denonSerialForCard(snd_seq_client_info_get_card(client)));
        }
    }
    snd_seq_close(seq);
    if (indices.size() != serials.size()) {
        qWarning() << "Denon enumeration changed; serial assignment withheld until restart";
        return result;
    }
    for (int i = 0; i < indices.size(); ++i) {
        if (!serials[i].isEmpty() && serials.count(serials[i]) == 1) {
            result.insert(indices[i], name + QStringLiteral(" ") + serials[i]);
            qInfo() << "Denon identity" << indices[i] << result.value(indices[i]);
        }
    }
#endif
    return result;
}

} // namespace

PortMidiEnumerator::PortMidiEnumerator() {
    PmError err = Pm_Initialize();
    // Based on reading the source, it's not possible for this to fail.
    if (err != pmNoError) {
        qWarning() << "PortMidi error:" << Pm_GetErrorText(err);
    }
}

PortMidiEnumerator::~PortMidiEnumerator() {
    qDebug() << "Deleting PortMIDI devices...";
    QListIterator<Controller*> dev_it(m_devices);
    while (dev_it.hasNext()) {
        delete dev_it.next();
    }
    PmError err = Pm_Terminate();
    // Based on reading the source, it's not possible for this to fail.
    if (err != pmNoError) {
        qWarning() << "PortMidi error:" << Pm_GetErrorText(err);
    }
}

bool shouldLinkInputToOutput(const QString& input_name,
        const QString& output_name) {
    // Early exit.
    if (input_name == output_name || namesMatchAllowableEdgeCases(input_name, output_name)) {
        return true;
    }

    // Some device drivers prepend "To" and "From" to the names of their MIDI
    // ports. If the output and input device names don't match, let's try
    // trimming those words from the start, and seeing if they then match.

    // Ignore "From" text in the beginning of device input name.
    QString input_name_stripped = input_name;
    if (input_name.indexOf("from", 0, Qt::CaseInsensitive) == 0) {
        input_name_stripped = input_name.right(input_name.length() - 4);
    }

    // Ignore "To" text in the beginning of device output name.
    QString output_name_stripped = output_name;
    if (output_name.indexOf("to", 0, Qt::CaseInsensitive) == 0) {
        output_name_stripped = output_name.right(output_name.length() - 2);
    }

    if (output_name_stripped != input_name_stripped) {
        // Ignore " input " text in the device names
        int offset = input_name_stripped.indexOf(" input ", 0,
                                                 Qt::CaseInsensitive);
        if (offset != -1) {
            input_name_stripped = input_name_stripped.replace(offset, 7, " ");
        }

        // Ignore " output " text in the device names
        offset = output_name_stripped.indexOf(" output ", 0,
                                              Qt::CaseInsensitive);
        if (offset != -1) {
            output_name_stripped = output_name_stripped.replace(offset, 8, " ");
        }
    }

    if (input_name_stripped == output_name_stripped ||
        namesMatchMidiPattern(input_name_stripped, output_name_stripped) ||
        namesMatchMidiPattern(input_name, output_name) ||
        namesMatchInOutPattern(input_name_stripped, output_name_stripped) ||
        namesMatchInOutPattern(input_name, output_name) ||
        namesMatchPattern(input_name_stripped, output_name_stripped) ||
        namesMatchPattern(input_name, output_name)) {
        return true;
    }

    return false;
}

/** Enumerate the MIDI devices
  * This method needs a bit of intelligence because PortMidi (and the underlying MIDI APIs) like to split
  * output and input into separate devices. Eg. PortMidi would tell us the Hercules is two half-duplex devices.
  * To help simplify a lot of code, we're going to aggregate these two streams into a single full-duplex device.
  */
QList<Controller*> PortMidiEnumerator::queryDevices() {
    qDebug() << "Scanning PortMIDI devices:";

    int iNumDevices = Pm_CountDevices();
    const auto identityNames = denonIdentityNames();

    QListIterator<Controller*> dev_it(m_devices);
    while (dev_it.hasNext()) {
        delete dev_it.next();
    }

    m_devices.clear();

    const PmDeviceInfo* inputDeviceInfo = nullptr;
    const PmDeviceInfo* outputDeviceInfo = nullptr;
    int inputDevIndex = -1;
    int outputDevIndex = -1;
    QMap<int, QString> unassignedOutputDevices;

    // Build a complete list of output devices for later pairing
    for (int i = 0; i < iNumDevices; i++) {
        const PmDeviceInfo* pDeviceInfo = Pm_GetDeviceInfo(i);
        VERIFY_OR_DEBUG_ASSERT(pDeviceInfo) {
            continue;
        }
        if (!recognizeDevice(*pDeviceInfo) || !pDeviceInfo->output) {
            continue;
        }
        qDebug() << " Found output device"
                 << "#" << i << pDeviceInfo->name;
        QString deviceName = pDeviceInfo->name;
        unassignedOutputDevices[i] = deviceName;
    }

    // Search for input devices and pair them with output devices if applicable
    for (int i = 0; i < iNumDevices; i++) {
        const PmDeviceInfo* pDeviceInfo = Pm_GetDeviceInfo(i);
        VERIFY_OR_DEBUG_ASSERT(pDeviceInfo) {
            continue;
        }
        if (!recognizeDevice(*pDeviceInfo) || !pDeviceInfo->input) {
            // Is there a use case for output-only devices such as message
            // displays? Then this condition has to be split and
            // deviceInfo->output also needs to be checked and handled.
            continue;
        }

        qDebug() << " Found input device"
                 << "#" << i << pDeviceInfo->name;
        inputDeviceInfo = pDeviceInfo;
        inputDevIndex = i;

        //Reset our output device variables before we look for one in case we find none.
        outputDeviceInfo = nullptr;
        outputDevIndex = -1;

        //Search for a corresponding output device
        QMapIterator<int, QString> j(unassignedOutputDevices);
        while (j.hasNext()) {
            j.next();

            QString deviceName = inputDeviceInfo->name;
            QString outputName = QString(j.value());

            if (shouldLinkInputToOutput(deviceName, outputName)) {
                outputDevIndex = j.key();
                outputDeviceInfo = Pm_GetDeviceInfo(outputDevIndex);

                unassignedOutputDevices.remove(outputDevIndex);

                qDebug() << "    Linking to output device #" << outputDevIndex << outputName;
                break;
            }
        }

        // So at this point, we either have an input-only MIDI device
        // (outputDeviceInfo == NULL) or we've found a matching output MIDI
        // device (outputDeviceInfo != NULL).

        //.... so create our (aggregate) MIDI device!
        PortMidiController* currentDevice =
                new PortMidiController(inputDeviceInfo,
                        outputDeviceInfo,
                        inputDevIndex,
                        outputDevIndex,
                        identityNames.value(inputDevIndex));
        m_devices.push_back(currentDevice);
    }
    return m_devices;
}
