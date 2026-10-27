#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace quietvinyl {
inline QString readUsbAttribute(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.readAll()).trimmed() : QString{};
}

// The SC6000 MIDI function has no serial. Its sibling main-player USB
// function carries the serial inside the same built-in Denon hub.
inline QString denonSerialForCard(int card, const QString& soundRoot = QStringLiteral("/sys/class/sound")) {
    if (card < 0) return {};
    QString path = QFileInfo(soundRoot + QStringLiteral("/card%1/device").arg(card)).canonicalFilePath();
    while (!path.isEmpty() && path != QStringLiteral("/")) {
        if (readUsbAttribute(path + "/idVendor") == QStringLiteral("15e4") &&
                readUsbAttribute(path + "/idProduct") == QStringLiteral("800d")) {
            QDir hub(QFileInfo(path).absolutePath());
            QString found;
            for (const auto& child : hub.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString sibling = child.absoluteFilePath();
                if (readUsbAttribute(sibling + "/idVendor") != QStringLiteral("15e4") ||
                        readUsbAttribute(sibling + "/idProduct") != QStringLiteral("a00d")) continue;
                const QString serial = readUsbAttribute(sibling + "/serial");
                if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9]+$")).match(serial).hasMatch() ||
                        !found.isEmpty()) return {};
                found = serial;
            }
            return found;
        }
        path = QFileInfo(path).absolutePath();
    }
    return {};
}
} // namespace quietvinyl
