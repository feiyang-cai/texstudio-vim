// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VIM_REGISTERS_H
#define VIM_REGISTERS_H

#include <QHash>
#include <QStringList>

enum class VimRegisterType { CharacterWise, LineWise, BlockWise };

struct VimRegister {
    VimRegisterType type = VimRegisterType::CharacterWise;
    QString text;
    QStringList blocks;
};

// Shared by editor views, matching Vim's session-wide register lifetime.
// No document cursors or editor ownership belong in this store.
class VimRegisters
{
public:
    static bool isValidName(QChar name);
    VimRegister read(QChar name) const;
    void write(QChar name, const VimRegister &value, bool yank);

private:
    QHash<QChar, VimRegister> m_values;
};

VimRegisters &vimRegisters();
#endif
