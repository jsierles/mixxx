#include "library/enginedjcueparser.h"
#include <QDataStream>
#include <QtEndian>
#include "util/fpclassify.h"
#include <zlib.h>

namespace {
QByteArray unpack(const QByteArray& blob) {
    if (blob.size() < 5 || blob.size() > 8192) return {};
    const auto size = qFromBigEndian<quint32>(blob.constData());
    if (size == 0 || size > 4096) return {};
    QByteArray data(static_cast<int>(size), '\0');
    uLongf actual = size;
    if (uncompress(reinterpret_cast<Bytef*>(data.data()), &actual,
                reinterpret_cast<const Bytef*>(blob.constData() + 4), blob.size() - 4) != Z_OK ||
            actual != size) return {};
    return data;
}
}

EngineDjCueData decodeEngineDjCues(const QByteArray& quickCues, const QByteArray& trackData) {
    const QByteArray audio = unpack(trackData), cues = unpack(quickCues);
    if (audio.size() < 44 || cues.size() < 25) return {};
    QDataStream timing(audio);
    double sampleRate;
    qint64 frames;
    timing >> sampleRate >> frames;
    if (!util_isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 384000 || frames <= 0) return {};
    QDataStream stream(cues);
    qint64 count;
    stream >> count;
    if (count < 0 || count > 8) return {};
    EngineDjCueData result;
    for (int slot = 0; slot < count; ++slot) {
        quint8 length;
        stream >> length;
        QByteArray label(length, '\0');
        if (stream.readRawData(label.data(), length) != length) return {};
        double offset;
        quint8 alpha, red, green, blue;
        stream >> offset >> alpha >> red >> green >> blue;
        if (stream.status() != QDataStream::Ok || !util_isfinite(offset) ||
                offset < -1 || (offset < 0 && offset != -1) || offset > frames) return {};
        if (offset == -1) continue;
        result.hotCues.append({slot, offset * 1000.0 / sampleRate,
                QString::fromUtf8(label), quint32(red) << 16 | quint32(green) << 8 | blue});
    }
    // The trailing adjusted/default main cue fields are required by the
    // record format, but are not imported: loading still follows the needle.
    double adjusted, original;
    quint8 changed;
    stream >> adjusted >> changed >> original;
    if (stream.status() != QDataStream::Ok || changed > 1) return {};
    result.valid = true;
    return result;
}
