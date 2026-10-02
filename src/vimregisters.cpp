// SPDX-License-Identifier: GPL-2.0-or-later
#include "vimregisters.h"
#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>

namespace {
const char *registerMimeType = "application/x-texstudio-vim-register";

QClipboard::Mode clipboardMode(QChar name)
{
    // Platforms without primary selection use the ordinary clipboard for '*'.
    return name == QLatin1Char('*') && QApplication::clipboard()->supportsSelection()
            ? QClipboard::Selection : QClipboard::Clipboard;
}

VimRegister appendRegister(VimRegister previous, const VimRegister &value)
{
    if (previous.text.isEmpty() && previous.blocks.isEmpty())
        return value;
    if (previous.type == VimRegisterType::BlockWise && value.type == VimRegisterType::BlockWise) {
        const int rows = qMax(previous.blocks.size(), value.blocks.size());
        while (previous.blocks.size() < rows)
            previous.blocks.append(QString());
        for (int i = 0; i < value.blocks.size(); ++i)
            previous.blocks[i] += value.blocks.at(i);
        previous.text = previous.blocks.join(QLatin1Char('\n'));
    } else {
        previous.text += value.text;
        previous.type = previous.type == VimRegisterType::LineWise || value.type == VimRegisterType::LineWise
                ? VimRegisterType::LineWise : VimRegisterType::CharacterWise;
        if (previous.type == VimRegisterType::LineWise && !previous.text.endsWith(QLatin1Char('\n')))
            previous.text += QLatin1Char('\n');
        previous.blocks.clear();
    }
    return previous;
}
}

bool VimRegisters::isValidName(QChar name)
{
    return (name >= QLatin1Char('a') && name <= QLatin1Char('z'))
            || (name >= QLatin1Char('A') && name <= QLatin1Char('Z'))
            || (name >= QLatin1Char('0') && name <= QLatin1Char('9'))
            || QStringLiteral("\"-_+*").contains(name);
}

VimRegister VimRegisters::read(QChar name) const
{
    if (name == QLatin1Char('_'))
        return {};
    if (name == QLatin1Char('+') || name == QLatin1Char('*')) {
        const QMimeData *mime = QApplication::clipboard()->mimeData(clipboardMode(name));
        if (!mime)
            return {};
        VimRegister value;
        // QDocument stores LF internally. External clipboard producers, notably
        // Windows applications, may provide CRLF or legacy CR line endings.
        value.text = mime->text();
        value.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        value.text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        value.type = value.text.endsWith(QLatin1Char('\n')) ? VimRegisterType::LineWise : VimRegisterType::CharacterWise;
        if (mime->hasFormat(registerMimeType)) {
            const QJsonObject object = QJsonDocument::fromJson(mime->data(registerMimeType)).object();
            const int type = object.value(QStringLiteral("type")).toInt(-1);
            if (type >= int(VimRegisterType::CharacterWise) && type <= int(VimRegisterType::BlockWise)) {
                value.type = static_cast<VimRegisterType>(type);
                for (const auto &row : object.value(QStringLiteral("blocks")).toArray())
                    value.blocks.append(row.toString());
            }
        }
        return value;
    }
    return m_values.value(name.toLower());
}

void VimRegisters::write(QChar name, const VimRegister &value, bool yank)
{
    if (!isValidName(name) || name == QLatin1Char('_') || (value.text.isEmpty() && value.blocks.isEmpty()))
        return;
    VimRegister stored = value;
    if (name >= QLatin1Char('A') && name <= QLatin1Char('Z'))
        stored = appendRegister(read(name), value);
    if (name == QLatin1Char('+') || name == QLatin1Char('*')) {
        auto *mime = new QMimeData;
        mime->setText(stored.text);
        QJsonObject object;
        object.insert(QStringLiteral("type"), int(stored.type));
        object.insert(QStringLiteral("blocks"), QJsonArray::fromStringList(stored.blocks));
        mime->setData(registerMimeType, QJsonDocument(object).toJson(QJsonDocument::Compact));
        QApplication::clipboard()->setMimeData(mime, clipboardMode(name));
    } else if (name != QLatin1Char('"')) {
        m_values.insert(name.toLower(), stored);
    }
    m_values.insert(QLatin1Char('"'), stored);

    // Explicit destinations leave the automatic history alone.
    if (name != QLatin1Char('"'))
        return;
    if (yank) {
        m_values.insert(QLatin1Char('0'), value);
    } else if (value.type == VimRegisterType::CharacterWise && !value.text.contains(QLatin1Char('\n'))) {
        m_values.insert(QLatin1Char('-'), value);
    } else {
        for (char number = '9'; number > '1'; --number)
            m_values.insert(QLatin1Char(number), m_values.value(QLatin1Char(number - 1)));
        m_values.insert(QLatin1Char('1'), value);
    }
}

VimRegisters &vimRegisters()
{
    static VimRegisters registers;
    return registers;
}
