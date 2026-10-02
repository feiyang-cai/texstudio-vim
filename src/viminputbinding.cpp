// SPDX-License-Identifier: GPL-2.0-or-later
#include "viminputbinding.h"
#include "vimregisters.h"
#include "latexeditorview.h"
#include "latexdocument.h"
#include "qeditor.h"
#include "qdocumentline.h"
#include "qstatuspanel.h"
#include "mostQtHeaders.h"
#include <functional>

namespace {
enum class VimMode {
    Normal,
    Insert,
    Replace,
    Visual,
    VisualLine,
    VisualBlock,
    OperatorPending,
    CommandPrompt,
    SearchForward,
    SearchBackward
};

enum class VimOperator {
    None,
    Delete,
    Change,
    Yank,
    Indent,
    Unindent
};

enum class VimFindKind {
    None,
    FindForward,
    FindBackward,
    TillForward,
    TillBackward
};

enum class VimPendingMarkAction {
    None,
    Set,
    JumpLine,
    JumpExact
};

struct VimInsertStep {
    enum Type {
        InsertText,
        Backspace,
        Delete,
        NewLine
    };

    Type type = InsertText;
    QString text;
};

struct VimMotion {
    enum Kind {
        None,
        Left,
        Right,
        Up,
        Down,
        WordForward,
        WordBackward,
        WordEnd,
        LineStart,
        LineStartText,
        LineEnd,
        FileStart,
        FileEnd,
        PrevBlock,
        NextBlock,
        MatchingPair,
        FindCharacter
    };

    Kind kind = None;
    int count = 1;
    VimFindKind findKind = VimFindKind::None;
    QChar findChar;
};

struct VimTextObject {
    enum Kind {
        None,
        InnerWord,
        AroundWord,
        InnerParen,
        AroundParen,
        InnerBracket,
        AroundBracket,
        InnerBrace,
        AroundBrace,
        InnerDoubleQuote,
        AroundDoubleQuote,
        InnerSingleQuote,
        AroundSingleQuote
    };

    Kind kind = None;
};

}

class VimPromptPanelImpl : public VimPromptPanel
{
public:
    Q_PANEL(VimPromptPanelImpl, "Vim Prompt Panel")

    explicit VimPromptPanelImpl(QWidget *parent = nullptr)
        : VimPromptPanel(parent), m_view(qobject_cast<LatexEditorView *>(parent)), m_promptLabel(new QLabel(this)), m_lineEdit(new QLineEdit(this)),
          m_messageLabel(new QLabel(this)), m_kind(NoPrompt), m_historyIndex(-1)
    {
        setDefaultVisibility(false);
        setObjectName("vimPromptPanel");

        auto *layout = new QGridLayout(this);
        layout->setContentsMargins(6, 2, 6, 2);
        layout->setHorizontalSpacing(6);
        layout->addWidget(m_promptLabel, 0, 0);
        layout->addWidget(m_lineEdit, 0, 1);
        layout->addWidget(m_messageLabel, 1, 0, 1, 2);

        m_promptLabel->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral(":")) + 4);
        m_messageLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
        m_messageLabel->hide();
        m_lineEdit->installEventFilter(this);

        connect(m_lineEdit, &QLineEdit::returnPressed, this, [this]() {
            submitPrompt();
        });
    }

    QString type() const override
    {
        return QStringLiteral("Vim Prompt");
    }

    bool forward(QMouseEvent *event) override
    {
        Q_UNUSED(event)
        return false;
    }

    void openPrompt(PromptKind kind)
    {
        m_kind = kind;
        resetHistoryNavigation();
        m_promptLabel->setText(kind == CommandPrompt ? QStringLiteral(":") : (kind == SearchBackwardPrompt ? QStringLiteral("?") : QStringLiteral("/")));
        m_lineEdit->clear();
        m_messageLabel->clear();
        m_messageLabel->hide();
        if (m_view)
            m_view->setVimPromptVisible(true);
        else
            show();
        raise();
        m_lineEdit->setFocus();
    }

    void closePrompt();

    PromptKind promptKind() const
    {
        return m_kind;
    }

    void showError(const QString &message)
    {
        m_messageLabel->setText(message);
        m_messageLabel->show();
        m_lineEdit->setFocus();
        m_lineEdit->selectAll();
        QApplication::beep();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QPanel::showEvent(event);
        m_lineEdit->setFocus();
        m_lineEdit->selectAll();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_lineEdit && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Up || (keyEvent->key() == Qt::Key_P && (keyEvent->modifiers() & Qt::ControlModifier))) {
                if (stepHistory(-1))
                    return true;
            }
            if (keyEvent->key() == Qt::Key_Down || (keyEvent->key() == Qt::Key_N && (keyEvent->modifiers() & Qt::ControlModifier))) {
                if (stepHistory(1))
                    return true;
            }
            if (keyEvent->key() == Qt::Key_Escape || (keyEvent->key() == Qt::Key_BracketLeft && (keyEvent->modifiers() & Qt::ControlModifier))) {
                closePrompt();
                return true;
            }
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                submitPrompt();
                return true;
            }
        }
        return QPanel::eventFilter(watched, event);
    }

private:
    bool submitPrompt()
    {
        if (!m_view)
            return false;

        const QPointer<VimPromptPanelImpl> guard(this);
        const QString text = m_lineEdit->text();
        bool handled = false;
        switch (m_kind) {
        case CommandPrompt:
            handled = m_view->executeVimExCommand(text);
            break;
        case SearchForwardPrompt:
            m_view->executeVimSearch(text, false);
            handled = true;
            break;
        case SearchBackwardPrompt:
            m_view->executeVimSearch(text, true);
            handled = true;
            break;
        case NoPrompt:
            break;
        }
        if (handled)
            rememberEntry(text);
        if (handled && guard)
            closePrompt();
        return handled;
    }

    static int historyBucket(PromptKind kind)
    {
        switch (kind) {
        case CommandPrompt:
            return 0;
        case SearchForwardPrompt:
        case SearchBackwardPrompt:
            return 1;
        case NoPrompt:
        default:
            return -1;
        }
    }

    void resetHistoryNavigation()
    {
        m_historyIndex = -1;
        m_pendingInput.clear();
    }

    bool stepHistory(int direction)
    {
        const int bucket = historyBucket(m_kind);
        if (bucket < 0)
            return false;

        const QStringList entries = m_history.value(bucket);
        if (entries.isEmpty())
            return false;

        if (direction < 0) {
            if (m_historyIndex < 0) {
                m_pendingInput = m_lineEdit->text();
                m_historyIndex = entries.size() - 1;
            } else if (m_historyIndex > 0) {
                --m_historyIndex;
            }
        } else {
            if (m_historyIndex < 0)
                return false;
            if (m_historyIndex + 1 < entries.size()) {
                ++m_historyIndex;
            } else {
                m_historyIndex = -1;
                m_lineEdit->setText(m_pendingInput);
                m_lineEdit->setCursorPosition(m_lineEdit->text().size());
                m_messageLabel->hide();
                return true;
            }
        }

        m_lineEdit->setText(entries.at(m_historyIndex));
        m_lineEdit->setCursorPosition(m_lineEdit->text().size());
        m_messageLabel->hide();
        return true;
    }

    void rememberEntry(const QString &text)
    {
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty())
            return;

        const int bucket = historyBucket(m_kind);
        if (bucket < 0)
            return;

        QStringList entries = m_history.value(bucket);
        entries.removeAll(trimmed);
        entries << trimmed;

        const int historyLimit = 50;
        while (entries.size() > historyLimit)
            entries.removeFirst();

        m_history.insert(bucket, entries);
        resetHistoryNavigation();
    }

    LatexEditorView *m_view;
    QLabel *m_promptLabel;
    QLineEdit *m_lineEdit;
    QLabel *m_messageLabel;
    PromptKind m_kind;
    QHash<int, QStringList> m_history;
    int m_historyIndex;
    QString m_pendingInput;
};

class VimInputBindingImpl : public VimInputBinding
{
public:
    explicit VimInputBindingImpl(LatexEditorView *view, QEditorInputBindingInterface *defaultBinding)
        : m_view(view), m_defaultBinding(defaultBinding), m_mode(VimMode::Normal), m_pendingOperator(VimOperator::None),
          m_pendingFind(VimFindKind::None), m_pendingMarkAction(VimPendingMarkAction::None), m_count(0), m_operatorCount(0), m_pendingTextObject(),
          m_lastFindKind(VimFindKind::None), m_lastSearchBackward(false), m_insertRepeatable(false), m_replaceRestoreOverwrite(false),
          m_visualAnchorLine(0), m_visualAnchorColumn(0), m_visualBlockPreferredColumn(0)
    {
    }

    QString id() const override
    {
        return QStringLiteral("TXS::VimInputBinding");
    }

    QString name() const override
    {
        return QStringLiteral("TXS::VimInputBinding");
    }

    bool isExclusive() const override
    {
        return false;
    }

    bool keyPressEvent(QKeyEvent *event, QEditor *editor) override
    {
        if (!editor)
            return false;
        syncPromptState(editor);
        if (m_mode == VimMode::Insert || m_mode == VimMode::Replace)
            return handleInsertMode(event, editor);
        if (handleRegisterPrefix(event, editor))
            return true;
#ifndef Q_OS_MAC
        if (event->matches(QKeySequence::Paste)
                && (m_mode == VimMode::Normal || m_mode == VimMode::Visual
                    || m_mode == VimMode::VisualLine || m_mode == VimMode::VisualBlock)) {
            beginVisualBlock(editor);
            return true;
        }
#endif
        if (event->matches(QKeySequence::Undo)) {
            editor->undo();
            return true;
        }
        if ((event->modifiers() & Qt::AltModifier) || ((event->modifiers() & Qt::MetaModifier) && !isVisualBlockShortcut(event))) {
            clearPending(editor);
            return false;
        }
        bool handled = false;
        switch (m_mode) {
        case VimMode::OperatorPending:
            handled = handleOperatorPending(event, editor);
            break;
        case VimMode::Visual:
        case VimMode::VisualLine:
        case VimMode::VisualBlock:
            handled = handleVisualMode(event, editor);
            break;
        default:
            handled = handleNormalMode(event, editor);
            break;
        }
        if ((m_mode == VimMode::Normal || m_mode == VimMode::Insert || m_mode == VimMode::Replace)
                && m_pendingFind == VimFindKind::None && m_pendingMarkAction == VimPendingMarkAction::None
                && !m_lastG && !m_pendingReplace && m_count == 0)
            m_selectedRegister = QLatin1Char('"');
        return handled || shouldConsumeNonInsertKey(event);
    }

    void postKeyPressEvent(QKeyEvent *event, QEditor *editor) override
    {
        if ((m_mode == VimMode::Insert || m_mode == VimMode::Replace) && m_defaultBinding)
            m_defaultBinding->postKeyPressEvent(event, editor);
    }

    bool keyReleaseEvent(QKeyEvent *event, QEditor *editor) override
    {
        return m_defaultBinding ? m_defaultBinding->keyReleaseEvent(event, editor) : false;
    }

    void postKeyReleaseEvent(QKeyEvent *event, QEditor *editor) override
    {
        if (m_defaultBinding)
            m_defaultBinding->postKeyReleaseEvent(event, editor);
    }

    bool inputMethodEvent(QInputMethodEvent *event, QEditor *editor) override
    {
        if ((m_mode == VimMode::Insert || m_mode == VimMode::Replace) && m_defaultBinding)
            return m_defaultBinding->inputMethodEvent(event, editor);
        Q_UNUSED(event)
        Q_UNUSED(editor)
        return true;
    }

    void postInputMethodEvent(QInputMethodEvent *event, QEditor *editor) override
    {
        if ((m_mode == VimMode::Insert || m_mode == VimMode::Replace) && m_defaultBinding)
            m_defaultBinding->postInputMethodEvent(event, editor);
    }

    bool mouseMoveEvent(QMouseEvent *event, QEditor *editor) override
    {
        return m_defaultBinding ? m_defaultBinding->mouseMoveEvent(event, editor) : false;
    }

    void postMouseMoveEvent(QMouseEvent *event, QEditor *editor) override
    {
        if (m_defaultBinding)
            m_defaultBinding->postMouseMoveEvent(event, editor);
    }

    bool mousePressEvent(QMouseEvent *event, QEditor *editor) override
    {
        clearPending(editor);
        leaveVisualMode(editor, false);
        return m_defaultBinding ? m_defaultBinding->mousePressEvent(event, editor) : false;
    }

    void postMousePressEvent(QMouseEvent *event, QEditor *editor) override
    {
        if (m_defaultBinding)
            m_defaultBinding->postMousePressEvent(event, editor);
    }

    bool mouseReleaseEvent(QMouseEvent *event, QEditor *editor) override
    {
        if (m_mode == VimMode::Insert || m_mode == VimMode::Replace) {
            return m_defaultBinding ? m_defaultBinding->mouseReleaseEvent(event, editor) : false;
        }
        bool handled = m_defaultBinding ? m_defaultBinding->mouseReleaseEvent(event, editor) : false;
        normalizeNormalCursor(editor);
        setMode(VimMode::Normal, editor);
        return handled;
    }

    void postMouseReleaseEvent(QMouseEvent *event, QEditor *editor) override
    {
        if (m_defaultBinding)
            m_defaultBinding->postMouseReleaseEvent(event, editor);
    }

    bool mouseDoubleClickEvent(QMouseEvent *event, QEditor *editor) override
    {
        clearPending(editor);
        leaveVisualMode(editor, false);
        return m_defaultBinding ? m_defaultBinding->mouseDoubleClickEvent(event, editor) : false;
    }

    void postMouseDoubleClickEvent(QMouseEvent *event, QEditor *editor) override
    {
        if (m_defaultBinding)
            m_defaultBinding->postMouseDoubleClickEvent(event, editor);
    }

    bool contextMenuEvent(QContextMenuEvent *event, QEditor *editor) override
    {
        clearPending(editor);
        leaveVisualMode(editor, false);
        return m_defaultBinding ? m_defaultBinding->contextMenuEvent(event, editor) : false;
    }

    void resetForEditor(QEditor *editor)
    {
        clearPending(editor);
        leaveVisualMode(editor, false);
        setMode(VimMode::Normal, editor);
        m_repeatAction = std::function<void()>();
    }

    void promptClosed(QEditor *editor)
    {
        clearPending(editor);
        setMode(VimMode::Normal, editor);
        normalizeNormalCursor(editor);
    }

    void recordSearch(const QString &text, bool backward)
    {
        m_lastSearchText = text;
        m_lastSearchBackward = backward;
    }

    QString lastSearchText() const
    {
        return m_lastSearchText;
    }

    bool shouldOverrideShortcut(const QKeyEvent *event) const
    {
#ifdef Q_OS_MAC
        Q_UNUSED(event)
        return false;
#else
        return event
               && event->matches(QKeySequence::Paste)
               && (m_mode == VimMode::Normal || m_mode == VimMode::Visual
                   || m_mode == VimMode::VisualLine || m_mode == VimMode::VisualBlock);
#endif
    }

    bool handleEscapeShortcut(QEditor *editor)
    {
        if (!editor)
            return false;

        switch (m_mode) {
        case VimMode::Insert:
        case VimMode::Replace:
            finishInsertSession(editor);
            return true;
        case VimMode::Visual:
        case VimMode::VisualLine:
        case VimMode::VisualBlock:
            leaveVisualMode(editor, true);
            return true;
        case VimMode::OperatorPending:
            clearPending(editor);
            setMode(VimMode::Normal, editor);
            normalizeNormalCursor(editor);
            return true;
        case VimMode::Normal:
            if (m_waitingForRegister || m_selectedRegister != QLatin1Char('"') || m_pendingFind != VimFindKind::None || m_pendingMarkAction != VimPendingMarkAction::None || m_count > 0 || m_lastG || m_pendingReplace || m_waitingForTextObject || m_pendingOperator != VimOperator::None) {
                clearPending(editor);
                normalizeNormalCursor(editor);
                return true;
            }
            return false;
        case VimMode::CommandPrompt:
        case VimMode::SearchForward:
        case VimMode::SearchBackward:
            return false;
        }

        return false;
    }

private:
    bool handleRegisterPrefix(QKeyEvent *event, QEditor *editor)
    {
        if (m_waitingForRegister) {
            m_waitingForRegister = false;
            if (event->text().size() == 1 && VimRegisters::isValidName(event->text().at(0))
                    && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
                m_selectedRegister = event->text().at(0);
            } else {
                clearPending(editor);
                if (event->key() != Qt::Key_Escape && !isCtrlLeftBracket(event))
                    QApplication::beep();
            }
            return true;
        }
        if (m_mode != VimMode::Normal && m_mode != VimMode::Visual
                && m_mode != VimMode::VisualLine && m_mode != VimMode::VisualBlock)
            return false;
        if (m_pendingFind != VimFindKind::None || m_pendingMarkAction != VimPendingMarkAction::None
                || m_pendingReplace || m_lastG)
            return false;
        if (event->text() == QLatin1String("\"")
                && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            m_waitingForRegister = true;
            return true;
        }
        return false;
    }

    void storeRegister(const VimRegister &value, bool yank)
    {
        vimRegisters().write(m_selectedRegister, value, yank);
    }

    QString modeLabel() const
    {
        switch (m_mode) {
        case VimMode::Insert:
            return QStringLiteral("INSERT");
        case VimMode::Replace:
            return QStringLiteral("REPLACE");
        case VimMode::Visual:
            return QStringLiteral("VISUAL");
        case VimMode::VisualLine:
            return QStringLiteral("V-LINE");
        case VimMode::VisualBlock:
            return QStringLiteral("V-BLOCK");
        case VimMode::CommandPrompt:
            return QStringLiteral("COMMAND");
        case VimMode::SearchForward:
        case VimMode::SearchBackward:
            return QStringLiteral("SEARCH");
        case VimMode::OperatorPending:
        case VimMode::Normal:
        default:
            return QStringLiteral("NORMAL");
        }
    }

    static QDocument::CursorRenderingStyle cursorStyleForMode(VimMode mode)
    {
        switch (mode) {
        case VimMode::Insert:
            return QDocument::LineCursorStyle;
        case VimMode::Replace:
            return QDocument::UnderlineCursorStyle;
        default:
            return QDocument::BlockCursorStyle;
        }
    }

    void setMode(VimMode mode, QEditor *editor)
    {
        m_mode = mode;
        if (!editor)
            return;
        if (mode != VimMode::Replace && editor->flag(QEditor::Overwrite) && m_replaceRestoreOverwrite) {
            editor->setFlag(QEditor::Overwrite, false);
            if (editor->document())
                editor->document()->setOverwriteMode(false);
            m_replaceRestoreOverwrite = false;
        }
        editor->setCursorStyle(cursorStyleForMode(mode));
        editor->setInputModeLabel(modeLabel());
        editor->emitCursorPositionChanged();
    }

    void syncPromptState(QEditor *editor)
    {
        if (!m_view || !m_view->vimPromptPanel || m_view->vimPromptPanel->isVisible())
            return;
        if (m_mode == VimMode::CommandPrompt || m_mode == VimMode::SearchForward || m_mode == VimMode::SearchBackward)
            setMode(VimMode::Normal, editor);
    }

    void clearPending(QEditor *editor)
    {
        Q_UNUSED(editor)
        m_selectedRegister = QLatin1Char('"');
        m_waitingForRegister = false;
        m_pendingOperator = VimOperator::None;
        m_pendingFind = VimFindKind::None;
        m_pendingMarkAction = VimPendingMarkAction::None;
        m_pendingTextObject.kind = VimTextObject::None;
        m_waitingForTextObject = false;
        m_pendingTextObjectInner = true;
        m_count = 0;
        m_operatorCount = 0;
        m_lastG = false;
        m_pendingReplace = false;
    }

    int consumeCountOrOne()
    {
        const int value = m_count > 0 ? m_count : 1;
        m_count = 0;
        return value;
    }

    static bool isCtrlLeftBracket(const QKeyEvent *event)
    {
        return event->key() == Qt::Key_BracketLeft && (event->modifiers() & Qt::ControlModifier);
    }

    static bool isVisualBlockShortcut(const QKeyEvent *event)
    {
        if (event->modifiers() & Qt::AltModifier)
            return false;
        if (event->key() == Qt::Key_V)
        {
#ifdef Q_OS_MAC
            return (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) != 0;
#else
            return (event->modifiers() & Qt::ControlModifier) != 0 && (event->modifiers() & Qt::MetaModifier) == 0;
#endif
        }
#ifndef Q_OS_MAC
        if (!(event->modifiers() & Qt::ControlModifier) || (event->modifiers() & Qt::MetaModifier))
            return false;
#endif
        return event->text() == QString(QChar(0x16));
    }

    static bool shouldConsumeNonInsertKey(const QKeyEvent *event)
    {
        if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            return false;

        if (!event->text().isEmpty())
            return true;

        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Backspace:
        case Qt::Key_Delete:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            return true;
        default:
            return false;
        }
    }

    static bool isMarkName(const QChar &mark)
    {
        return (mark >= QLatin1Char('a') && mark <= QLatin1Char('z'))
               || (mark >= QLatin1Char('A') && mark <= QLatin1Char('Z'));
    }

    QDocumentCursor resolvedMarkCursor(const QChar &mark) const
    {
        if (mark == QLatin1Char('\'') || mark == QLatin1Char('`'))
            return m_previousJumpPosition;
        if (!isMarkName(mark))
            return QDocumentCursor();
        return m_marks.value(mark);
    }

    void executeOperatorLineRange(QEditor *editor, int fromLine, int toLine)
    {
        if (!editor || !editor->document())
            return;

        const int firstLine = qMax(0, qMin(fromLine, toLine));
        const int lastLine = qMin(editor->document()->lineCount() - 1, qMax(fromLine, toLine));

        switch (m_pendingOperator) {
        case VimOperator::Delete:
        case VimOperator::Change: {
            storeRegister({VimRegisterType::LineWise, lineRangeText(editor->document(), firstLine, lastLine, true), {}}, false);

            QDocumentCursor cursor(editor->document(), firstLine, 0, lastLine, editor->document()->line(lastLine).length());
            if (lastLine + 1 < editor->document()->lineCount())
                cursor.select(firstLine, 0, lastLine + 1, 0);
            cursor.removeSelectedText();
            editor->setCursor(cursor);
            if (m_pendingOperator == VimOperator::Change)
                startInsertSession(QStringLiteral("i"), editor);
            else {
                setMode(VimMode::Normal, editor);
                normalizeNormalCursor(editor);
            }
            break;
        }
        case VimOperator::Yank:
            storeRegister({VimRegisterType::LineWise, lineRangeText(editor->document(), firstLine, lastLine, true), {}}, true);
            setMode(VimMode::Normal, editor);
            break;
        case VimOperator::Indent:
            shiftLines(editor, firstLine, lastLine, true);
            break;
        case VimOperator::Unindent:
            shiftLines(editor, firstLine, lastLine, false);
            break;
        case VimOperator::None:
            break;
        }
    }

    void executeOperatorMark(QEditor *editor, const QChar &mark, bool linewise)
    {
        const QDocumentCursor target = resolvedMarkCursor(mark);
        if (!target.isValid()) {
            QApplication::beep();
            clearPending(editor);
            setMode(VimMode::Normal, editor);
            normalizeNormalCursor(editor);
            return;
        }

        if (linewise)
            executeOperatorLineRange(editor, editor->cursor().lineNumber(), target.lineNumber());
        else
            applyOperatorOnRange(editor, editor->cursor(), target, false);
    }

    bool handlePendingMark(QKeyEvent *event, QEditor *editor)
    {
        if (event->key() == Qt::Key_Escape || isCtrlLeftBracket(event)) {
            clearPending(editor);
            return true;
        }

        if ((event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) || event->text().size() != 1) {
            QApplication::beep();
            clearPending(editor);
            return true;
        }

        const QChar mark = event->text().at(0);
        switch (m_pendingMarkAction) {
        case VimPendingMarkAction::Set:
            setMark(editor, mark);
            break;
        case VimPendingMarkAction::JumpLine:
            if (m_pendingOperator != VimOperator::None)
                executeOperatorMark(editor, mark, true);
            else
                jumpToMark(editor, mark, true);
            break;
        case VimPendingMarkAction::JumpExact:
            if (m_pendingOperator != VimOperator::None)
                executeOperatorMark(editor, mark, false);
            else
                jumpToMark(editor, mark, false);
            break;
        case VimPendingMarkAction::None:
            break;
        }
        clearPending(editor);
        return true;
    }

    void recordInsertStep(const QKeyEvent *event)
    {
        if (!m_insertRepeatable)
            return;

        if (event->key() == Qt::Key_Backspace) {
            m_insertSteps << VimInsertStep{VimInsertStep::Backspace, QString()};
        } else if (event->key() == Qt::Key_Delete) {
            m_insertSteps << VimInsertStep{VimInsertStep::Delete, QString()};
        } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            m_insertSteps << VimInsertStep{VimInsertStep::NewLine, QString()};
        } else if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !event->text().isEmpty() && event->text().size() == 1) {
            m_insertSteps << VimInsertStep{VimInsertStep::InsertText, event->text()};
        } else {
            m_insertRepeatable = false;
        }
    }

    bool handleInsertMode(QKeyEvent *event, QEditor *editor)
    {
        if (event->key() == Qt::Key_Escape || isCtrlLeftBracket(event)) {
            finishInsertSession(editor);
            return true;
        }
        if (m_defaultBinding && m_defaultBinding->keyPressEvent(event, editor)) {
            recordInsertStep(event);
            m_defaultBinding->postKeyPressEvent(event, editor);
            return true;
        }
        recordInsertStep(event);
        return false;
    }

    void finishInsertSession(QEditor *editor)
    {
        const auto action = m_insertEntryAction;
        const auto steps = m_insertSteps;
        const auto repeatable = m_insertRepeatable && !steps.isEmpty();
        const bool blockInsert = action == QLatin1String("blockI") || action == QLatin1String("blockA");
        setMode(VimMode::Normal, editor);
        if (blockInsert || !steps.isEmpty()) {
            QDocumentCursor cursor = editor->cursor();
            if (cursor.columnNumber() > 0 && (!steps.isEmpty() || action == QLatin1String("blockA")))
                cursor.movePosition(1, QDocumentCursor::PreviousCharacter);
            editor->setCursor(cursor);
        } else if (action == QLatin1String("a") || action == QLatin1String("A")) {
            QDocumentCursor cursor = editor->cursor();
            if (cursor.columnNumber() > 0)
                cursor.movePosition(1, QDocumentCursor::PreviousCharacter);
            editor->setCursor(cursor);
        }
        normalizeNormalCursor(editor);
        if (repeatable) {
            m_repeatAction = [this, action, steps]() {
                replayInsertAction(action, steps);
            };
        }
        m_insertSteps.clear();
        m_insertRepeatable = false;
        m_insertEntryAction.clear();
    }

    void startInsertSession(const QString &entryAction, QEditor *editor, VimMode mode = VimMode::Insert)
    {
        m_insertEntryAction = entryAction;
        m_insertSteps.clear();
        m_insertRepeatable = true;
        if (mode == VimMode::Replace) {
            m_replaceRestoreOverwrite = true;
            editor->setFlag(QEditor::Overwrite, true);
            if (editor->document())
                editor->document()->setOverwriteMode(true);
        }
        setMode(mode, editor);
    }

    void replayInsertAction(const QString &entryAction, const QVector<VimInsertStep> &steps)
    {
        QEditor *editor = m_view ? m_view->editor : nullptr;
        if (!editor)
            return;
        if (entryAction == QLatin1String("i")) {
        } else if (entryAction == QLatin1String("a")) {
            moveRightForAppend(editor);
        } else if (entryAction == QLatin1String("blockA")) {
            moveRightForAppend(editor);
        } else if (entryAction == QLatin1String("I")) {
            moveToLineStartText(editor);
        } else if (entryAction == QLatin1String("A")) {
            moveToLineEnd(editor);
        } else if (entryAction == QLatin1String("o")) {
            openLineBelow(editor);
        } else if (entryAction == QLatin1String("O")) {
            openLineAbove(editor);
        } else if (entryAction == QLatin1String("R")) {
            m_replaceRestoreOverwrite = true;
            editor->setFlag(QEditor::Overwrite, true);
            if (editor->document())
                editor->document()->setOverwriteMode(true);
        }
        for (const VimInsertStep &step : steps)
            applyInsertStep(editor, step);
        setMode(VimMode::Normal, editor);
        if (!steps.isEmpty()) {
            QDocumentCursor cursor = editor->cursor();
            if (cursor.columnNumber() > 0)
                cursor.movePosition(1, QDocumentCursor::PreviousCharacter);
            editor->setCursor(cursor);
        }
        normalizeNormalCursor(editor);
    }

    void applyInsertStep(QEditor *editor, const VimInsertStep &step)
    {
        QDocumentCursor cursor = editor->cursor();
        switch (step.type) {
        case VimInsertStep::InsertText:
            editor->write(step.text);
            break;
        case VimInsertStep::Backspace:
            cursor.deletePreviousChar();
            editor->setCursor(cursor);
            break;
        case VimInsertStep::Delete:
            cursor.deleteChar();
            editor->setCursor(cursor);
            break;
        case VimInsertStep::NewLine:
            editor->insertText(cursor, QStringLiteral("\n"));
            editor->setCursor(cursor);
            break;
        }
    }

    bool handleNormalMode(QKeyEvent *event, QEditor *editor)
    {
        if (event->matches(QKeySequence::Redo) || (event->key() == Qt::Key_R && (event->modifiers() & Qt::ControlModifier))) {
            editor->redo();
            return true;
        }
        if (m_pendingMarkAction != VimPendingMarkAction::None)
            return handlePendingMark(event, editor);
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && event->text().size() == 1 && event->text().at(0).isDigit()) {
            if (event->text() == QLatin1String("0") && m_count == 0) {
                moveToLineStart(editor);
                return true;
            }
            m_count = m_count * 10 + event->text().toInt();
            return true;
        }

        if (event->key() == Qt::Key_Escape || isCtrlLeftBracket(event)) {
            clearPending(editor);
            leaveVisualMode(editor, true);
            setMode(VimMode::Normal, editor);
            return true;
        }

        if (event->key() == Qt::Key_Period && !(event->modifiers() & Qt::ShiftModifier)) {
            const auto repeat = m_repeatAction;
            if (repeat)
                repeat();
            return true;
        }

        if (isVisualBlockShortcut(event)) {
            beginVisualBlock(editor);
            return true;
        }
        if (!(event->modifiers() & Qt::ControlModifier) && !(event->modifiers() & Qt::AltModifier) && !event->text().isEmpty()) {
            const QChar key = event->text().at(0);
            switch (key.unicode()) {
            case 'i': startInsertSession(QStringLiteral("i"), editor); return true;
            case 'a': moveRightForAppend(editor); startInsertSession(QStringLiteral("a"), editor); return true;
            case 'I': moveToLineStartText(editor); startInsertSession(QStringLiteral("I"), editor); return true;
            case 'A': moveToLineEnd(editor); startInsertSession(QStringLiteral("A"), editor); return true;
            case 'o': openLineBelow(editor); startInsertSession(QStringLiteral("o"), editor); return true;
            case 'O': openLineAbove(editor); startInsertSession(QStringLiteral("O"), editor); return true;
            case 'R': startInsertSession(QStringLiteral("R"), editor, VimMode::Replace); return true;
            case 'v': beginVisual(editor); return true;
            case 'V': beginVisualLine(editor); return true;
            case ':': openPrompt(editor, VimPromptPanel::CommandPrompt); return true;
            case '/': openPrompt(editor, VimPromptPanel::SearchForwardPrompt); return true;
            case '?': openPrompt(editor, VimPromptPanel::SearchBackwardPrompt); return true;
            case 'd': beginOperator(VimOperator::Delete, editor); return true;
            case 'c': beginOperator(VimOperator::Change, editor); return true;
            case 'y': beginOperator(VimOperator::Yank, editor); return true;
            case '>': beginOperator(VimOperator::Indent, editor); return true;
            case '<': beginOperator(VimOperator::Unindent, editor); return true;
            case 'p': putRegister(editor, true); return true;
            case 'P': putRegister(editor, false); return true;
            case 'u': editor->undo(); return true;
            case 'x': deleteCharacters(editor, consumeCountOrOne(), false); return true;
            case 'X': deleteCharacters(editor, consumeCountOrOne(), true); return true;
            case 's': substituteCharacters(editor, consumeCountOrOne()); return true;
            case 'S': changeWholeLines(editor, consumeCountOrOne()); return true;
            case 'D': deleteToLineEnd(editor); return true;
            case 'C': changeToLineEnd(editor); return true;
            case 'G': gotoLine(editor, m_count > 0 ? m_count : editor->document()->lineCount()); m_count = 0; return true;
            case 'Y': yankWholeLines(editor, consumeCountOrOne()); return true;
            case 'J': joinLines(editor, consumeCountOrOne()); return true;
            case 'g': m_pendingFind = VimFindKind::None; m_lastG = true; return true;
            case 'f': m_pendingFind = VimFindKind::FindForward; return true;
            case 'F': m_pendingFind = VimFindKind::FindBackward; return true;
            case 't': m_pendingFind = VimFindKind::TillForward; return true;
            case 'T': m_pendingFind = VimFindKind::TillBackward; return true;
            case ';': repeatFind(editor, false); return true;
            case ',': repeatFind(editor, true); return true;
            case '%': moveToMatchingPair(editor); return true;
            case '*': searchWordUnderCursor(editor, false); return true;
            case '#': searchWordUnderCursor(editor, true); return true;
            case 'n': repeatSearch(editor, false); return true;
            case 'N': repeatSearch(editor, true); return true;
            case 'r': m_pendingReplace = true; return true;
            case 'm': m_pendingMarkAction = VimPendingMarkAction::Set; return true;
            case '\'': m_pendingMarkAction = VimPendingMarkAction::JumpLine; return true;
            case '`': m_pendingMarkAction = VimPendingMarkAction::JumpExact; return true;
            default:
                break;
            }
        }

        if (m_pendingReplace && !event->text().isEmpty() && event->text().size() == 1) {
            replaceCharacters(editor, consumeCountOrOne(), event->text().at(0));
            m_pendingReplace = false;
            return true;
        }

        if (m_pendingFind != VimFindKind::None && !event->text().isEmpty() && event->text().size() == 1) {
            executeFind(editor, m_pendingFind, event->text().at(0), consumeCountOrOne());
            return true;
        }

        if (m_lastG) {
            m_lastG = false;
            if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !event->text().isEmpty() && event->text().at(0) == QLatin1Char('g')) {
                gotoLine(editor, m_count > 0 ? m_count : 1);
                m_count = 0;
                return true;
            }
        }

        VimMotion motion;
        if (parseMotion(event, consumeCountOrOne(), motion)) {
            moveByMotion(editor, motion);
            return true;
        }
        return false;
    }

    bool handleOperatorPending(QKeyEvent *event, QEditor *editor)
    {
        if (m_pendingMarkAction != VimPendingMarkAction::None)
            return handlePendingMark(event, editor);
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && event->text().size() == 1 && event->text().at(0).isDigit()) {
            m_count = m_count * 10 + event->text().toInt();
            return true;
        }
        if (event->key() == Qt::Key_Escape || isCtrlLeftBracket(event)) {
            clearPending(editor);
            setMode(VimMode::Normal, editor);
            return true;
        }
        if (m_pendingFind != VimFindKind::None && !event->text().isEmpty() && event->text().size() == 1) {
            VimMotion motion;
            motion.kind = VimMotion::FindCharacter;
            motion.count = qMax(1, m_count);
            motion.findKind = m_pendingFind;
            motion.findChar = event->text().at(0);
            executeOperatorMotion(editor, motion);
            return true;
        }
        if (m_waitingForTextObject && !event->text().isEmpty() && event->text().size() == 1) {
            if (updateTextObject(event->text().at(0))) {
                executeOperatorTextObject(editor, m_pendingTextObject);
                return true;
            }
            clearPending(editor);
            setMode(VimMode::Normal, editor);
            return true;
        }
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !event->text().isEmpty() && event->text().size() == 1) {
            const QChar key = event->text().at(0);
            if ((key == QLatin1Char('d') && m_pendingOperator == VimOperator::Delete) ||
                (key == QLatin1Char('c') && m_pendingOperator == VimOperator::Change) ||
                (key == QLatin1Char('y') && m_pendingOperator == VimOperator::Yank) ||
                (key == QLatin1Char('>') && m_pendingOperator == VimOperator::Indent) ||
                (key == QLatin1Char('<') && m_pendingOperator == VimOperator::Unindent)) {
                executeLinewiseOperator(editor, qMax(1, m_operatorCount * qMax(1, m_count)));
                return true;
            }
            if (key == QLatin1Char('i') || key == QLatin1Char('a')) {
                m_waitingForTextObject = true;
                m_pendingTextObjectInner = (key == QLatin1Char('i'));
                return true;
            }
            if (key == QLatin1Char('f')) {
                m_pendingFind = VimFindKind::FindForward;
                return true;
            }
            if (key == QLatin1Char('F')) {
                m_pendingFind = VimFindKind::FindBackward;
                return true;
            }
            if (key == QLatin1Char('t')) {
                m_pendingFind = VimFindKind::TillForward;
                return true;
            }
            if (key == QLatin1Char('T')) {
                m_pendingFind = VimFindKind::TillBackward;
                return true;
            }
            if (key == QLatin1Char('\'')) {
                m_pendingMarkAction = VimPendingMarkAction::JumpLine;
                return true;
            }
            if (key == QLatin1Char('`')) {
                m_pendingMarkAction = VimPendingMarkAction::JumpExact;
                return true;
            }
        }

        VimMotion motion;
        if (parseMotion(event, qMax(1, m_count), motion)) {
            executeOperatorMotion(editor, motion);
            return true;
        }
        clearPending(editor);
        setMode(VimMode::Normal, editor);
        return true;
    }

    bool handleVisualMode(QKeyEvent *event, QEditor *editor)
    {
        if (event->key() == Qt::Key_Escape || isCtrlLeftBracket(event)) {
            leaveVisualMode(editor, true);
            return true;
        }
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && event->text().size() == 1 && event->text().at(0).isDigit()) {
            if (event->text() == QLatin1String("0") && m_count == 0) {
                moveToLineStart(editor);
                updateVisualSelection(editor);
                return true;
            }
            m_count = m_count * 10 + event->text().toInt();
            return true;
        }
        if (isVisualBlockShortcut(event)) {
            beginVisualBlock(editor);
            return true;
        }
        if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !event->text().isEmpty()) {
            const QChar key = event->text().at(0);
            switch (key.unicode()) {
            case 'v': leaveVisualMode(editor, true); return true;
            case 'V': beginVisualLine(editor); return true;
            case 'I':
                if (m_mode == VimMode::VisualBlock) {
                    beginVisualBlockInsert(editor, false);
                    return true;
                }
                break;
            case 'A':
                if (m_mode == VimMode::VisualBlock) {
                    beginVisualBlockInsert(editor, true);
                    return true;
                }
                break;
            case 'y': yankVisualSelection(editor); return true;
            case 'd': deleteVisualSelection(editor, false); return true;
            case 'c': deleteVisualSelection(editor, true); return true;
            case '>': shiftVisualSelection(editor, true); return true;
            case '<': shiftVisualSelection(editor, false); return true;
            case 'p': replaceVisualSelectionWithRegister(editor); return true;
            default:
                break;
            }
        }
        VimMotion motion;
        if (parseMotion(event, consumeCountOrOne(), motion)) {
            moveByMotion(editor, motion);
            updateVisualSelection(editor);
            return true;
        }
        return true;
    }

    bool parseMotion(QKeyEvent *event, int count, VimMotion &motion)
    {
        motion.count = qMax(1, count);
        if (!event->text().isEmpty() && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            const QChar key = event->text().at(0);
            switch (key.unicode()) {
            case 'h': motion.kind = VimMotion::Left; return true;
            case 'j': motion.kind = VimMotion::Down; return true;
            case 'k': motion.kind = VimMotion::Up; return true;
            case 'l': motion.kind = VimMotion::Right; return true;
            case 'w': motion.kind = VimMotion::WordForward; return true;
            case 'b': motion.kind = VimMotion::WordBackward; return true;
            case 'e': motion.kind = VimMotion::WordEnd; return true;
            case '0': motion.kind = VimMotion::LineStart; return true;
            case '^': motion.kind = VimMotion::LineStartText; return true;
            case '$': motion.kind = VimMotion::LineEnd; return true;
            case 'G': motion.kind = VimMotion::FileEnd; return true;
            case '{': motion.kind = VimMotion::PrevBlock; return true;
            case '}': motion.kind = VimMotion::NextBlock; return true;
            default:
                break;
            }
        }
        return false;
    }

    void moveByMotion(QEditor *editor, const VimMotion &motion)
    {
        QDocumentCursor cursor = editor->cursor();
        if (m_mode == VimMode::VisualBlock)
            cursor = QDocumentCursor(editor->document(), cursor.lineNumber(), m_visualBlockPreferredColumn);
        if (m_mode == VimMode::Visual && cursor.hasSelection() && cursor.columnNumber() > 0)
            cursor.movePosition(1, QDocumentCursor::PreviousCharacter);
        for (int i = 0; i < motion.count; ++i) {
            switch (motion.kind) {
            case VimMotion::Left: cursor.movePosition(1, QDocumentCursor::PreviousCharacter); break;
            case VimMotion::Right: cursor.movePosition(1, QDocumentCursor::NextCharacter); break;
            case VimMotion::Up: cursor.movePosition(1, QDocumentCursor::Up); break;
            case VimMotion::Down: cursor.movePosition(1, QDocumentCursor::Down); break;
            case VimMotion::WordForward: cursor.movePosition(1, QDocumentCursor::NextWord); break;
            case VimMotion::WordBackward: cursor.movePosition(1, QDocumentCursor::PreviousWord); break;
            case VimMotion::WordEnd: cursor.movePosition(1, QDocumentCursor::EndOfWord); break;
            case VimMotion::LineStart: cursor.movePosition(1, QDocumentCursor::StartOfLine); break;
            case VimMotion::LineStartText: cursor.movePosition(1, QDocumentCursor::StartOfLineText); break;
            case VimMotion::LineEnd: cursor.movePosition(1, QDocumentCursor::EndOfLine); break;
            case VimMotion::FileStart: cursor.movePosition(1, QDocumentCursor::Start); break;
            case VimMotion::FileEnd: cursor.movePosition(1, QDocumentCursor::End); break;
            case VimMotion::PrevBlock: cursor.movePosition(1, QDocumentCursor::PreviousBlock); break;
            case VimMotion::NextBlock: cursor.movePosition(1, QDocumentCursor::NextBlock); break;
            case VimMotion::MatchingPair:
            case VimMotion::FindCharacter:
            case VimMotion::None:
                break;
            }
        }
        if (m_mode == VimMode::VisualBlock)
            m_visualBlockPreferredColumn = cursor.columnNumber();
        editor->setCursor(cursor);
        if (m_mode != VimMode::VisualBlock)
            normalizeNormalCursor(editor);
    }

    void beginOperator(VimOperator op, QEditor *editor)
    {
        m_pendingOperator = op;
        m_operatorCount = consumeCountOrOne();
        m_pendingTextObject.kind = VimTextObject::None;
        m_pendingFind = VimFindKind::None;
        setMode(VimMode::OperatorPending, editor);
    }

    void executeLinewiseOperator(QEditor *editor, int count)
    {
        switch (m_pendingOperator) {
        case VimOperator::Delete:
            deleteWholeLines(editor, count, false);
            break;
        case VimOperator::Change:
            deleteWholeLines(editor, count, true);
            break;
        case VimOperator::Yank:
            yankWholeLines(editor, count);
            break;
        case VimOperator::Indent:
            shiftLines(editor, editor->cursor().lineNumber(), editor->cursor().lineNumber() + count - 1, true);
            break;
        case VimOperator::Unindent:
            shiftLines(editor, editor->cursor().lineNumber(), editor->cursor().lineNumber() + count - 1, false);
            break;
        case VimOperator::None:
            break;
        }
        const bool changing = m_pendingOperator == VimOperator::Change;
        clearPending(editor);
        if (!changing)
            setMode(VimMode::Normal, editor);
    }

    void executeOperatorMotion(QEditor *editor, const VimMotion &motion)
    {
        QDocumentCursor cursor = editor->cursor();
        QDocumentCursor target(cursor);
        moveCursorByMotion(target, motion);
        const bool inclusive = motion.kind == VimMotion::WordEnd || motion.kind == VimMotion::FindCharacter;
        applyOperatorOnRange(editor, cursor, target, false, inclusive);
    }

    void executeOperatorTextObject(QEditor *editor, const VimTextObject &textObject)
    {
        QDocumentCursor selection = selectionForTextObject(editor, textObject);
        if (!selection.hasSelection()) {
            clearPending(editor);
            setMode(VimMode::Normal, editor);
            return;
        }
        QDocumentCursor start = selection.selectionStart();
        QDocumentCursor end = selection.selectionEnd();
        applyOperatorOnRange(editor, start, end, true);
    }

    void applyOperatorOnRange(QEditor *editor, const QDocumentCursor &from, const QDocumentCursor &to, bool alreadySelected, bool inclusive = false)
    {
        QDocumentCursor start(from);
        QDocumentCursor end(to);
        if (!alreadySelected)
            QDocumentCursor::sort(start, end);
        if (!alreadySelected && inclusive && end.isValid() && !end.atLineEnd())
            end.movePosition(1, QDocumentCursor::NextCharacter);
        QDocumentCursor selection(editor->document(), start.lineNumber(), start.columnNumber(), end.lineNumber(), end.columnNumber());
        if (!selection.hasSelection())
            selection.select(start.lineNumber(), start.columnNumber(), end.lineNumber(), end.columnNumber());

        switch (m_pendingOperator) {
        case VimOperator::Delete:
            setRegisterFromSelection(selection, VimRegisterType::CharacterWise);
            selection.removeSelectedText();
            editor->setCursor(selection);
            setMode(VimMode::Normal, editor);
            normalizeNormalCursor(editor);
            m_repeatAction = [this, selectionMotion = start, target = to]() {
                Q_UNUSED(selectionMotion)
                Q_UNUSED(target)
            };
            break;
        case VimOperator::Change:
            setRegisterFromSelection(selection, VimRegisterType::CharacterWise);
            selection.removeSelectedText();
            editor->setCursor(selection);
            startInsertSession(QStringLiteral("i"), editor);
            break;
        case VimOperator::Yank:
            setRegisterFromSelection(selection, VimRegisterType::CharacterWise, true);
            editor->setCursor(start);
            setMode(VimMode::Normal, editor);
            normalizeNormalCursor(editor);
            break;
        case VimOperator::Indent:
        case VimOperator::Unindent:
            shiftLines(editor, selection.startLineNumber(), selection.endLineNumber(), m_pendingOperator == VimOperator::Indent);
            break;
        case VimOperator::None:
            break;
        }
        clearPending(editor);
    }

    void moveCursorByMotion(QDocumentCursor &cursor, const VimMotion &motion)
    {
        for (int i = 0; i < motion.count; ++i) {
            switch (motion.kind) {
            case VimMotion::Left: cursor.movePosition(1, QDocumentCursor::PreviousCharacter); break;
            case VimMotion::Right: cursor.movePosition(1, QDocumentCursor::NextCharacter); break;
            case VimMotion::Up: cursor.movePosition(1, QDocumentCursor::Up); break;
            case VimMotion::Down: cursor.movePosition(1, QDocumentCursor::Down); break;
            case VimMotion::WordForward: cursor.movePosition(1, QDocumentCursor::NextWord); break;
            case VimMotion::WordBackward: cursor.movePosition(1, QDocumentCursor::PreviousWord); break;
            case VimMotion::WordEnd: cursor.movePosition(1, QDocumentCursor::EndOfWord); break;
            case VimMotion::LineStart: cursor.movePosition(1, QDocumentCursor::StartOfLine); break;
            case VimMotion::LineStartText: cursor.movePosition(1, QDocumentCursor::StartOfLineText); break;
            case VimMotion::LineEnd: cursor.movePosition(1, QDocumentCursor::EndOfLine); break;
            case VimMotion::FileStart: cursor.movePosition(1, QDocumentCursor::Start); break;
            case VimMotion::FileEnd: cursor.movePosition(1, QDocumentCursor::End); break;
            case VimMotion::PrevBlock: cursor.movePosition(1, QDocumentCursor::PreviousBlock); break;
            case VimMotion::NextBlock: cursor.movePosition(1, QDocumentCursor::NextBlock); break;
            case VimMotion::FindCharacter: applyFindMotion(cursor, motion.findKind, motion.findChar); break;
            case VimMotion::MatchingPair:
            case VimMotion::None:
                break;
            }
        }
    }

    void applyFindMotion(QDocumentCursor &cursor, VimFindKind kind, const QChar &ch)
    {
        const QString lineText = cursor.line().text();
        int column = cursor.columnNumber();
        int index = -1;
        if (kind == VimFindKind::FindForward || kind == VimFindKind::TillForward) {
            index = lineText.indexOf(ch, column + 1);
            if (index >= 0 && kind == VimFindKind::TillForward)
                --index;
        } else {
            index = lineText.lastIndexOf(ch, qMax(0, column - 1));
            if (index >= 0 && kind == VimFindKind::TillBackward)
                ++index;
        }
        if (index >= 0)
            cursor.moveTo(cursor.lineNumber(), qMax(0, index));
    }

    void beginVisual(QEditor *editor)
    {
        const QDocumentCursor cursor = editor->cursor();
        m_visualAnchorLine = cursor.lineNumber();
        m_visualAnchorColumn = cursor.columnNumber();
        editor->clearCursorMirrors();
        setMode(VimMode::Visual, editor);
        updateVisualSelection(editor);
    }

    void beginVisualLine(QEditor *editor)
    {
        const QDocumentCursor cursor = editor->cursor();
        m_visualAnchorLine = cursor.lineNumber();
        m_visualAnchorColumn = 0;
        editor->clearCursorMirrors();
        setMode(VimMode::VisualLine, editor);
        updateVisualSelection(editor);
    }

    void beginVisualBlock(QEditor *editor)
    {
        const QDocumentCursor cursor = editor->cursor();
        m_visualAnchorLine = cursor.lineNumber();
        m_visualAnchorColumn = cursor.columnNumber();
        m_visualBlockPreferredColumn = cursor.columnNumber();
        setMode(VimMode::VisualBlock, editor);
        updateVisualSelection(editor);
    }

    void beginVisualBlockInsert(QEditor *editor, bool append)
    {
        if (!editor)
            return;

        const QDocumentCursor cursor = editor->cursor();
        const int startLine = qMin(m_visualAnchorLine, cursor.lineNumber());
        const int endLine = qMax(m_visualAnchorLine, cursor.lineNumber());
        const int left = qMin(m_visualAnchorColumn, cursor.columnNumber());
        const int right = qMax(m_visualAnchorColumn, cursor.columnNumber()) + 1;

        QList<QDocumentCursor> insertCursors;
        int activeCursorIndex = -1;
        for (int line = startLine; line <= endLine; ++line) {
            const int lineLength = editor->document()->line(line).length();
            const int column = append ? qMin(right, lineLength) : qMin(left, lineLength);
            insertCursors << QDocumentCursor(editor->document(), line, column);
            if (line == cursor.lineNumber())
                activeCursorIndex = insertCursors.size() - 1;
        }

        if (insertCursors.isEmpty())
            return;
        if (activeCursorIndex < 0)
            activeCursorIndex = insertCursors.size() - 1;

        editor->setCursor(insertCursors.at(activeCursorIndex));
        for (int i = 0; i < insertCursors.size(); ++i) {
            if (i == activeCursorIndex)
                continue;
            editor->addCursorMirror(insertCursors.at(i));
        }
        startInsertSession(append ? QStringLiteral("blockA") : QStringLiteral("blockI"), editor);
    }

    void leaveVisualMode(QEditor *editor, bool clearSelection)
    {
        if (!editor)
            return;
        if (clearSelection) {
            QDocumentCursor cursor = editor->cursor();
            cursor.clearSelection();
            editor->setCursor(cursor);
            editor->clearCursorMirrors();
        }
        if (m_mode == VimMode::Visual || m_mode == VimMode::VisualLine || m_mode == VimMode::VisualBlock)
            setMode(VimMode::Normal, editor);
    }

    void updateVisualSelection(QEditor *editor)
    {
        if (!editor)
            return;
        if (m_mode == VimMode::VisualBlock) {
            updateVisualBlock(editor);
            return;
        }
        QDocumentCursor cursor = editor->cursor();
        const int currentLine = cursor.lineNumber();
        int startLine = m_visualAnchorLine;
        int startColumn = m_visualAnchorColumn;
        int endLine = currentLine;
        int endColumn = cursor.columnNumber();
        if (m_mode == VimMode::VisualLine) {
            const int anchorLineLength = editor->document()->line(m_visualAnchorLine).length();
            const int currentLineLength = editor->document()->line(currentLine).length();
            if (currentLine >= m_visualAnchorLine) {
                startColumn = 0;
                endColumn = currentLineLength;
            } else {
                startColumn = anchorLineLength;
                endColumn = 0;
            }
        } else if (!cursor.atLineEnd() || editor->document()->line(currentLine).length() == 0) {
            endColumn += 1;
        }
        QDocumentCursor selection(editor->document(), startLine, startColumn, endLine, endColumn);
        editor->setCursor(selection);
    }

    void updateVisualBlock(QEditor *editor)
    {
        const QDocumentCursor cursor = editor->cursor();
        const int left = qMin(m_visualAnchorColumn, cursor.columnNumber());
        const int right = qMax(m_visualAnchorColumn, cursor.columnNumber()) + 1;
        const int startLine = qMin(m_visualAnchorLine, cursor.lineNumber());
        const int endLine = qMax(m_visualAnchorLine, cursor.lineNumber());
        QList<QDocumentCursor> blockCursors;
        int activeCursorIndex = -1;
        for (int line = startLine; line <= endLine; ++line) {
            const int lineLength = editor->document()->line(line).length();
            const int endColumn = qMin(right, lineLength);
            QDocumentCursor blockCursor(editor->document(), line, qMin(left, lineLength), line, endColumn);
            blockCursors << blockCursor;
            if (line == cursor.lineNumber())
                activeCursorIndex = blockCursors.size() - 1;
        }

        if (blockCursors.isEmpty())
            return;
        if (activeCursorIndex < 0)
            activeCursorIndex = blockCursors.size() - 1;

        editor->setCursor(blockCursors.at(activeCursorIndex));
        for (int i = 0; i < blockCursors.size(); ++i) {
            if (i == activeCursorIndex)
                continue;
            editor->addCursorMirror(blockCursors.at(i));
        }
        editor->viewport()->update();
    }

    void setRegisterFromSelection(const QDocumentCursor &selection, VimRegisterType type, bool yank = false)
    {
        storeRegister({type, selection.selectedText(), {}}, yank);
    }

    void yankWholeLines(QEditor *editor, int count)
    {
        storeRegister({VimRegisterType::LineWise, lineRangeText(editor->document(), editor->cursor().lineNumber(), editor->cursor().lineNumber() + count - 1, true), {}}, true);
        setMode(VimMode::Normal, editor);
    }

    void deleteWholeLines(QEditor *editor, int count, bool enterInsert)
    {
        const QChar name = m_selectedRegister;
        const int firstLine = editor->cursor().lineNumber();
        const int lastLine = qMin(editor->document()->lineCount() - 1, firstLine + count - 1);
        const bool deletingAtDocumentEnd = lastLine + 1 >= editor->document()->lineCount();
        storeRegister({VimRegisterType::LineWise, lineRangeText(editor->document(), firstLine, lastLine, true), {}}, false);
        int landingLine = firstLine;
        QDocumentCursor cursor(editor->document(), firstLine, 0, lastLine, editor->document()->line(lastLine).length());
        if (!enterInsert && deletingAtDocumentEnd && firstLine > 0) {
            landingLine = firstLine - 1;
            cursor.select(firstLine - 1, editor->document()->line(firstLine - 1).length(), lastLine, editor->document()->line(lastLine).length());
        } else if (lastLine + 1 < editor->document()->lineCount()) {
            cursor.select(firstLine, 0, lastLine + 1, 0);
        }
        cursor.removeSelectedText();
        if (enterInsert) {
            editor->setCursor(cursor);
            startInsertSession(QStringLiteral("i"), editor);
        } else {
            editor->setCursor(QDocumentCursor(editor->document(), qMin(landingLine, qMax(0, editor->document()->lineCount() - 1)), 0));
            moveToLineStartText(editor);
            setMode(VimMode::Normal, editor);
            normalizeNormalCursor(editor);
            m_repeatAction = [this, count, name]() {
                m_selectedRegister = name;
                deleteWholeLines(m_view->editor, count, false);
                m_selectedRegister = QLatin1Char('"');
            };
        }
    }

    void deleteCharacters(QEditor *editor, int count, bool backwards)
    {
        const QChar name = m_selectedRegister;
        QDocumentCursor cursor = editor->cursor();
        if (backwards) {
            cursor.movePosition(count, QDocumentCursor::PreviousCharacter, QDocumentCursor::KeepAnchor);
        } else {
            cursor.movePosition(count, QDocumentCursor::NextCharacter, QDocumentCursor::KeepAnchor);
        }
        setRegisterFromSelection(cursor, VimRegisterType::CharacterWise);
        cursor.removeSelectedText();
        editor->setCursor(cursor);
        setMode(VimMode::Normal, editor);
        normalizeNormalCursor(editor);
        m_repeatAction = [this, count, backwards, name]() {
            m_selectedRegister = name;
            deleteCharacters(m_view->editor, count, backwards);
            m_selectedRegister = QLatin1Char('"');
        };
    }

    void substituteCharacters(QEditor *editor, int count)
    {
        deleteCharacters(editor, count, false);
        startInsertSession(QStringLiteral("i"), editor);
    }

    void replaceCharacters(QEditor *editor, int count, const QChar &ch)
    {
        QDocumentCursor cursor = editor->cursor();
        cursor.movePosition(count, QDocumentCursor::NextCharacter, QDocumentCursor::KeepAnchor);
        if (!cursor.hasSelection())
            return;
        cursor.replaceSelectedText(QString(count, ch));
        if (cursor.columnNumber() > 0)
            cursor.movePosition(1, QDocumentCursor::PreviousCharacter);
        editor->setCursor(cursor);
        setMode(VimMode::Normal, editor);
        normalizeNormalCursor(editor);
        m_repeatAction = [this, count, ch]() {
            replaceCharacters(m_view->editor, count, ch);
        };
    }

    void deleteToLineEnd(QEditor *editor)
    {
        const QChar name = m_selectedRegister;
        QDocumentCursor cursor = editor->cursor();
        cursor.movePosition(1, QDocumentCursor::EndOfLine, QDocumentCursor::KeepAnchor);
        setRegisterFromSelection(cursor, VimRegisterType::CharacterWise);
        cursor.removeSelectedText();
        editor->setCursor(cursor);
        normalizeNormalCursor(editor);
        m_repeatAction = [this, name]() {
            m_selectedRegister = name;
            deleteToLineEnd(m_view->editor);
            m_selectedRegister = QLatin1Char('"');
        };
    }

    void changeToLineEnd(QEditor *editor)
    {
        deleteToLineEnd(editor);
        startInsertSession(QStringLiteral("i"), editor);
    }

    void changeWholeLines(QEditor *editor, int count)
    {
        deleteWholeLines(editor, count, true);
    }

    void joinLines(QEditor *editor, int count)
    {
        QDocumentCursor cursor = editor->cursor();
        for (int i = 0; i < count; ++i) {
            if (cursor.lineNumber() + 1 >= editor->document()->lineCount())
                break;
            cursor.movePosition(1, QDocumentCursor::EndOfLine);
            cursor.deleteChar();
            if (cursor.nextChar() != QLatin1Char(' ') && cursor.previousChar() != QLatin1Char(' '))
                cursor.insertText(QStringLiteral(" "));
        }
        editor->setCursor(cursor);
        normalizeNormalCursor(editor);
        m_repeatAction = [this, count]() { joinLines(m_view->editor, count); };
    }

    void shiftLines(QEditor *editor, int startLine, int endLine, bool indent)
    {
        QDocumentCursor cursor(editor->document(), qMin(startLine, endLine), 0, qMin(editor->document()->lineCount() - 1, qMax(startLine, endLine) + 1), 0);
        editor->setCursor(cursor);
        if (indent)
            editor->indentSelection();
        else
            editor->unindentSelection();
        QDocumentCursor newCursor(editor->document(), qMin(startLine, endLine), 0);
        editor->setCursor(newCursor);
        normalizeNormalCursor(editor);
        setMode(VimMode::Normal, editor);
    }

    void shiftVisualSelection(QEditor *editor, bool indent)
    {
        const QDocumentCursor selection = editor->cursor();
        shiftLines(editor, selection.startLineNumber(), selection.endLineNumber(), indent);
        leaveVisualMode(editor, true);
    }

    void yankVisualSelection(QEditor *editor)
    {
        if (m_mode == VimMode::VisualBlock) {
            VimRegister value;
            value.type = VimRegisterType::BlockWise;
            for (const QDocumentCursor &cursor : editor->cursors())
                value.blocks << cursor.selectedText();
            value.text = value.blocks.join(QLatin1Char('\n'));
            storeRegister(value, true);
        } else if (m_mode == VimMode::VisualLine) {
            storeRegister({VimRegisterType::LineWise, lineRangeText(editor->document(), editor->cursor().startLineNumber(), editor->cursor().endLineNumber(), true), {}}, true);
        } else {
            setRegisterFromSelection(editor->cursor(), VimRegisterType::CharacterWise, true);
        }
        leaveVisualMode(editor, true);
    }

    void deleteVisualSelection(QEditor *editor, bool enterInsert)
    {
        if (m_mode == VimMode::VisualBlock) {
            VimRegister value;
            value.type = VimRegisterType::BlockWise;
            for (const QDocumentCursor &cursor : editor->cursors())
                value.blocks << cursor.selectedText();
            value.text = value.blocks.join(QLatin1Char('\n'));
            storeRegister(value, false);
            QList<QDocumentCursor> cursors = editor->cursors();
            for (QDocumentCursor &cursor : cursors)
                cursor.removeSelectedText();
            editor->setCursor(cursors.value(0));
            editor->clearCursorMirrors();
            if (enterInsert)
                startInsertSession(QStringLiteral("i"), editor);
            else
                setMode(VimMode::Normal, editor);
            return;
        }
        if (m_mode == VimMode::VisualLine) {
            const int firstLine = editor->cursor().startLineNumber();
            const int lastLine = editor->cursor().endLineNumber();
            const bool deletingAtDocumentEnd = lastLine + 1 >= editor->document()->lineCount();
            storeRegister({VimRegisterType::LineWise, lineRangeText(editor->document(), firstLine, lastLine, true), {}}, false);

            int landingLine = firstLine;
            QDocumentCursor cursor(editor->document(), firstLine, 0, lastLine, editor->document()->line(lastLine).length());
            if (!enterInsert && deletingAtDocumentEnd && firstLine > 0) {
                landingLine = firstLine - 1;
                cursor.select(firstLine - 1, editor->document()->line(firstLine - 1).length(), lastLine, editor->document()->line(lastLine).length());
            } else if (lastLine + 1 < editor->document()->lineCount()) {
                cursor.select(firstLine, 0, lastLine + 1, 0);
            }
            cursor.removeSelectedText();
            if (enterInsert) {
                editor->setCursor(cursor);
            } else {
                editor->setCursor(QDocumentCursor(editor->document(), qMin(landingLine, qMax(0, editor->document()->lineCount() - 1)), 0));
                moveToLineStartText(editor);
            }
        } else {
            QDocumentCursor cursor = editor->cursor();
            setRegisterFromSelection(cursor, VimRegisterType::CharacterWise);
            cursor.removeSelectedText();
            editor->setCursor(cursor);
        }
        if (enterInsert)
            startInsertSession(QStringLiteral("i"), editor);
        else
            setMode(VimMode::Normal, editor);
        normalizeNormalCursor(editor);
    }

    void replaceVisualSelectionWithRegister(QEditor *editor)
    {
        const VimRegister source = vimRegisters().read(m_selectedRegister);
        if (source.text.isEmpty() && source.blocks.isEmpty()) {
            leaveVisualMode(editor, true);
            return;
        }
        // A visual put updates the unnamed delete register, not its named source.
        m_selectedRegister = QLatin1Char('"');
        editor->document()->beginMacro();
        deleteVisualSelection(editor, false);
        putRegisterValue(editor, false, source);
        editor->document()->endMacro();
    }

    void putRegister(QEditor *editor, bool after)
    {
        const QChar name = m_selectedRegister;
        const VimRegister source = vimRegisters().read(name);
        const int count = consumeCountOrOne();
        editor->document()->beginMacro();
        for (int i = 0; i < count; ++i)
            putRegisterValue(editor, after, source);
        editor->document()->endMacro();
        m_repeatAction = [this, after, name, count]() {
            m_selectedRegister = name;
            m_count = count;
            putRegister(m_view->editor, after);
            m_selectedRegister = QLatin1Char('"');
        };
    }

    void putRegisterValue(QEditor *editor, bool after, const VimRegister &source)
    {
        if (source.text.isEmpty() && source.blocks.isEmpty())
            return;
        QDocumentCursor cursor = editor->cursor();
        if (source.type == VimRegisterType::LineWise) {
            const int lineCountBefore = editor->document()->lineCount();
            int targetLine = cursor.lineNumber() + (after ? 1 : 0);
            targetLine = qMax(0, targetLine);
            QDocumentCursor target(editor->document(), qMin(targetLine, qMax(0, editor->document()->lineCount() - 1)), 0);
            int insertedStartLine = qMin(targetLine, qMax(0, lineCountBefore - 1));
            if (targetLine >= editor->document()->lineCount()) {
                target.movePosition(1, QDocumentCursor::EndOfLine);
                QString text = source.text;
                const bool documentIsEmpty = editor->document()->lineCount() == 1 && editor->document()->line(0).length() == 0;
                insertedStartLine = documentIsEmpty ? 0 : lineCountBefore;
                if (text.endsWith(QLatin1Char('\n')))
                    text.chop(1);
                if (!documentIsEmpty)
                    text.prepend(QLatin1Char('\n'));
                editor->insertText(target, text);
            } else {
                editor->insertText(target, source.text);
            }
            editor->setCursor(QDocumentCursor(editor->document(), insertedStartLine, 0));
            moveToLineStartText(editor);
        } else if (source.type == VimRegisterType::BlockWise) {
            const int baseLine = cursor.lineNumber();
            const int column = cursor.columnNumber() + (after ? 1 : 0);
            for (int i = 0; i < source.blocks.size(); ++i) {
                const int line = qMin(baseLine + i, editor->document()->lineCount() - 1);
                QDocumentCursor block(editor->document(), line, qMin(column, editor->document()->line(line).length()));
                block.insertText(source.blocks.at(i));
            }
        } else {
            if (after && !cursor.atLineEnd())
                cursor.movePosition(1, QDocumentCursor::NextCharacter);
            editor->insertText(cursor, source.text);
            if (cursor.columnNumber() > 0)
                cursor.movePosition(1, QDocumentCursor::PreviousCharacter);
            editor->setCursor(cursor);
        }
        setMode(VimMode::Normal, editor);
        normalizeNormalCursor(editor);

    }

    void openLineBelow(QEditor *editor)
    {
        QDocumentCursor cursor = editor->cursor();
        cursor.movePosition(1, QDocumentCursor::EndOfLine);
        editor->insertText(cursor, QStringLiteral("\n"));
        editor->setCursor(cursor);
    }

    void openLineAbove(QEditor *editor)
    {
        QDocumentCursor cursor = editor->cursor();
        cursor.movePosition(1, QDocumentCursor::StartOfLine);
        editor->insertText(cursor, QStringLiteral("\n"));
        cursor.movePosition(1, QDocumentCursor::PreviousCharacter);
        editor->setCursor(cursor);
    }

    void moveToLineStart(QEditor *editor)
    {
        QDocumentCursor cursor = editor->cursor();
        cursor.movePosition(1, QDocumentCursor::StartOfLine);
        editor->setCursor(cursor);
        normalizeNormalCursor(editor);
    }

    void moveToLineStartText(QEditor *editor)
    {
        QDocumentCursor cursor = editor->cursor();
        cursor.movePosition(1, QDocumentCursor::StartOfLineText);
        editor->setCursor(cursor);
        normalizeNormalCursor(editor);
    }

    void moveToLineEnd(QEditor *editor)
    {
        QDocumentCursor cursor = editor->cursor();
        cursor.movePosition(1, QDocumentCursor::EndOfLine);
        editor->setCursor(cursor);
    }

    void moveRightForAppend(QEditor *editor)
    {
        QDocumentCursor cursor = editor->cursor();
        if (!cursor.atLineEnd())
            cursor.movePosition(1, QDocumentCursor::NextCharacter);
        editor->setCursor(cursor);
    }

    void gotoLine(QEditor *editor, int lineNumber)
    {
        QDocumentCursor cursor(editor->document(), qMax(0, qMin(editor->document()->lineCount() - 1, lineNumber - 1)), 0);
        editor->setCursor(cursor);
        normalizeNormalCursor(editor);
    }

    void moveToMatchingPair(QEditor *editor)
    {
        QDocumentCursor from, to;
        editor->cursor().getMatchingPair(from, to, false);
        if (from.isValid() && to.isValid()) {
            if (from.selectionStart() == editor->cursor().selectionStart())
                editor->setCursor(to.selectionStart());
            else
                editor->setCursor(from.selectionStart());
            normalizeNormalCursor(editor);
        }
    }

    void executeFind(QEditor *editor, VimFindKind kind, const QChar &ch, int count)
    {
        QDocumentCursor cursor = editor->cursor();
        VimMotion motion;
        motion.kind = VimMotion::FindCharacter;
        motion.count = count;
        motion.findKind = kind;
        motion.findChar = ch;
        moveCursorByMotion(cursor, motion);
        editor->setCursor(cursor);
        m_lastFindKind = kind;
        m_lastFindChar = ch;
        normalizeNormalCursor(editor);
        clearPending(editor);
        setMode(VimMode::Normal, editor);
    }

    void repeatFind(QEditor *editor, bool reverse)
    {
        if (m_lastFindKind == VimFindKind::None)
            return;
        VimFindKind kind = m_lastFindKind;
        if (reverse) {
            if (kind == VimFindKind::FindForward) kind = VimFindKind::FindBackward;
            else if (kind == VimFindKind::FindBackward) kind = VimFindKind::FindForward;
            else if (kind == VimFindKind::TillForward) kind = VimFindKind::TillBackward;
            else if (kind == VimFindKind::TillBackward) kind = VimFindKind::TillForward;
        }
        executeFind(editor, kind, m_lastFindChar, consumeCountOrOne());
    }

    void searchWordUnderCursor(QEditor *editor, bool backward)
    {
        QDocumentCursor cursor = editor->cursor();
        cursor.select(QDocumentCursor::WordUnderCursor);
        const QString word = cursor.selectedText();
        if (word.isEmpty())
            return;
        m_lastSearchBackward = backward;
        if (m_view)
            m_view->executeVimSearch(word, backward);
    }

    void repeatSearch(QEditor *editor, bool reverse)
    {
        if (m_lastSearchText.isEmpty())
            return;
        const bool backward = reverse ? !m_lastSearchBackward : m_lastSearchBackward;
        if (backward)
            editor->findPrev();
        else
            editor->findNext();
        normalizeNormalCursor(editor);
    }

    QDocumentCursor selectionForTextObject(QEditor *editor, const VimTextObject &textObject)
    {
        QDocumentCursor cursor = editor->cursor();
        switch (textObject.kind) {
        case VimTextObject::InnerWord:
            cursor.select(QDocumentCursor::WordUnderCursor);
            return cursor;
        case VimTextObject::AroundWord:
            cursor.select(QDocumentCursor::WordUnderCursor);
            cursor.expandSelect(QDocumentCursor::WordUnderCursor);
            return cursor;
        case VimTextObject::InnerParen:
        case VimTextObject::InnerBracket:
        case VimTextObject::InnerBrace:
            cursor.select(QDocumentCursor::ParenthesesInner);
            return cursor;
        case VimTextObject::AroundParen:
        case VimTextObject::AroundBracket:
        case VimTextObject::AroundBrace:
            cursor.select(QDocumentCursor::ParenthesesOuter);
            return cursor;
        case VimTextObject::InnerDoubleQuote:
            return quoteSelection(cursor, QLatin1Char('"'), false);
        case VimTextObject::AroundDoubleQuote:
            return quoteSelection(cursor, QLatin1Char('"'), true);
        case VimTextObject::InnerSingleQuote:
            return quoteSelection(cursor, QLatin1Char('\''), false);
        case VimTextObject::AroundSingleQuote:
            return quoteSelection(cursor, QLatin1Char('\''), true);
        case VimTextObject::None:
            break;
        }
        return QDocumentCursor();
    }

    QDocumentCursor quoteSelection(const QDocumentCursor &baseCursor, const QChar &quote, bool includeQuote)
    {
        QDocumentCursor cursor(baseCursor);
        const QString text = cursor.line().text();
        const int pos = cursor.columnNumber();
        const int left = text.lastIndexOf(quote, pos);
        const bool cursorOnQuote = pos >= 0 && pos < text.size() && text.at(pos) == quote;
        const int right = text.indexOf(quote, pos + (cursorOnQuote ? 1 : 0));
        if (left < 0 || right < 0 || left == right)
            return QDocumentCursor();
        return QDocumentCursor(cursor.document(), cursor.lineNumber(), includeQuote ? left : left + 1, cursor.lineNumber(), includeQuote ? right + 1 : right);
    }

    bool updateTextObject(const QChar &textObjectKey)
    {
        switch (textObjectKey.unicode()) {
        case 'w':
            m_pendingTextObject.kind = m_pendingTextObjectInner ? VimTextObject::InnerWord : VimTextObject::AroundWord;
            return true;
        case '(':
            m_pendingTextObject.kind = m_pendingTextObjectInner ? VimTextObject::InnerParen : VimTextObject::AroundParen;
            return true;
        case '[':
            m_pendingTextObject.kind = m_pendingTextObjectInner ? VimTextObject::InnerBracket : VimTextObject::AroundBracket;
            return true;
        case '{':
            m_pendingTextObject.kind = m_pendingTextObjectInner ? VimTextObject::InnerBrace : VimTextObject::AroundBrace;
            return true;
        case '"':
            m_pendingTextObject.kind = m_pendingTextObjectInner ? VimTextObject::InnerDoubleQuote : VimTextObject::AroundDoubleQuote;
            return true;
        case '\'':
            m_pendingTextObject.kind = m_pendingTextObjectInner ? VimTextObject::InnerSingleQuote : VimTextObject::AroundSingleQuote;
            return true;
        default:
            return false;
        }
    }

    void normalizeNormalCursor(QEditor *editor)
    {
        if (!editor || m_mode == VimMode::Insert || m_mode == VimMode::Replace)
            return;
        QDocumentCursor cursor = editor->cursor();
        cursor.clearSelection();
        const QDocumentLine line = cursor.line();
        if (line.isValid() && line.length() > 0 && cursor.columnNumber() >= line.length())
            cursor.moveTo(cursor.lineNumber(), line.length() - 1);
        editor->setCursor(cursor);
    }

    void setMark(QEditor *editor, const QChar &mark)
    {
        if (!editor || !editor->document())
            return;

        if (!isMarkName(mark)) {
            QApplication::beep();
            return;
        }

        QDocumentCursor cursor = editor->cursor();
        cursor.clearSelection();
        cursor.setAutoUpdated(true);
        cursor.setAutoErasable(false);
        m_marks.insert(mark, cursor);
    }

    void jumpToMark(QEditor *editor, const QChar &mark, bool linewise)
    {
        if (!editor || !editor->document())
            return;

        QDocumentCursor target = resolvedMarkCursor(mark);
        if (!target.isValid()) {
            QApplication::beep();
            return;
        }

        QDocumentCursor previous = editor->cursor();
        previous.clearSelection();
        previous.setAutoUpdated(true);
        previous.setAutoErasable(false);
        m_previousJumpPosition = previous;

        target.clearSelection();
        if (linewise) {
            editor->setCursor(QDocumentCursor(editor->document(), target.lineNumber(), 0));
            moveToLineStartText(editor);
        } else {
            editor->setCursor(target);
            normalizeNormalCursor(editor);
        }
        editor->ensureCursorVisible(QEditor::Navigation);
    }

    QString lineRangeText(QDocument *document, int startLine, int endLine, bool trailingNewline) const
    {
        QStringList lines;
        for (int line = startLine; line <= qMin(endLine, document->lineCount() - 1); ++line)
            lines << document->line(line).text();
        QString text = lines.join(QStringLiteral("\n"));
        if (trailingNewline)
            text += QStringLiteral("\n");
        return text;
    }

    void openPrompt(QEditor *editor, VimPromptPanel::PromptKind kind)
    {
        if (!m_view || !m_view->vimPromptPanel)
            return;
        setMode(kind == VimPromptPanel::CommandPrompt ? VimMode::CommandPrompt : (kind == VimPromptPanel::SearchBackwardPrompt ? VimMode::SearchBackward : VimMode::SearchForward), editor);
        m_view->vimPromptPanel->openPrompt(kind);
    }

    LatexEditorView *m_view;
    QEditorInputBindingInterface *m_defaultBinding;
    VimMode m_mode;
    QChar m_selectedRegister = QLatin1Char('"');
    bool m_waitingForRegister = false;
    VimOperator m_pendingOperator;
    VimFindKind m_pendingFind;
    VimPendingMarkAction m_pendingMarkAction;
    int m_count;
    int m_operatorCount;
    VimTextObject m_pendingTextObject;
    VimFindKind m_lastFindKind;
    QChar m_lastFindChar;
    QString m_lastSearchText;
    bool m_lastSearchBackward;
    bool m_insertRepeatable;
    bool m_replaceRestoreOverwrite;
    bool m_lastG = false;
    bool m_pendingReplace = false;
    bool m_waitingForTextObject = false;
    bool m_pendingTextObjectInner = true;
    int m_visualAnchorLine;
    int m_visualAnchorColumn;
    int m_visualBlockPreferredColumn;
    QString m_insertEntryAction;
    QVector<VimInsertStep> m_insertSteps;
    std::function<void()> m_repeatAction;
    QHash<QChar, QDocumentCursor> m_marks;
    QDocumentCursor m_previousJumpPosition;
};

void VimPromptPanelImpl::closePrompt()
{
    if (m_view)
        m_view->setVimPromptVisible(false);
    else
        hide();
    m_kind = NoPrompt;
    m_messageLabel->clear();
    m_messageLabel->hide();
    if (m_view && m_view->editor) {
        if (m_view->vimInputBinding)
            m_view->vimInputBinding->promptClosed(m_view->editor);
        m_view->editor->setFocus();
    }
}

VimInputBinding *createVimInputBinding(LatexEditorView *view, QEditorInputBindingInterface *defaultBinding)
{
    return new VimInputBindingImpl(view, defaultBinding);
}

VimPromptPanel *createVimPromptPanel(LatexEditorView *view)
{
    return new VimPromptPanelImpl(view);
}
