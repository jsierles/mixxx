#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

struct EngineDjHotCue {
    int slot; // zero-based; empty slots are never renumbered
    double milliseconds;
    QString label;
    quint32 rgb;
};
struct EngineDjCueData {
    bool valid = false;
    QList<EngineDjHotCue> hotCues;
};

// Engine's length-prefixed zlib records contain big-endian frame offsets.
// Return no partial result if a record is malformed or its timing is unknown.
EngineDjCueData decodeEngineDjCues(const QByteArray& quickCues, const QByteArray& trackData);
