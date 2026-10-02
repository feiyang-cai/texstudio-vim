
#ifndef QT_NO_DEBUG
#include "mostQtHeaders.h"
#include "latexeditorview_t.h"
#include "latexeditorview.h"
#include "latexdocument.h"
#include "latexeditorview_config.h"
#include "qdocumentcursor.h"
#include "qdocument.h"
#include "qeditor.h"
#include "testutil.h"
#include "vimregisters.h"
#include <QtTest/QtTest>

namespace {
bool skipVimUiTestInQuickRuns()
{
    if (globalExecuteAllTests)
        return false;
    QTest::qSkip("Vim UI coverage is only enabled in --execute-all-tests.", __FILE__, __LINE__);
    return true;
}
}

LatexEditorViewTest::LatexEditorViewTest(LatexEditorView* view): edView(view){}

void LatexEditorViewTest::insertHardLineBreaks_data(){
	QTest::addColumn<QString>("text");
	QTest::addColumn<int>("start");
	QTest::addColumn<int>("end");
	QTest::addColumn<int>("length");
	QTest::addColumn<QString>("newText");

	//-------------cursor without selection--------------
	QTest::newRow("one break in single line")
		<< "a\nhallo welt\nb"
		<< 1 << 1
		<< 5
		<< "a\nhallo\nwelt\nb";
	QTest::newRow("multiple breaks in single line")
		<< "a\nhallo welt miau x y z ping pong thing\nb"
		<< 1 << 1
		<< 5
		<< "a\nhallo\nwelt\nmiau\nx y z\nping\npong\nthing\nb";
	QTest::newRow("one break in multi lines")
		<< "a\nhallo welt\nb\nxyz\nc"
		<< 0 << 3
		<< 5
		<< "a\nhallo\nwelt\nb\nxyz\nc";
	QTest::newRow("multiple breaks in multiple lines")
		<< "hallo welt ilias ting ping 12 34\ntest test test 7 test test\nend"
		<< 0 << 1
		<< 5
		<< "hallo\nwelt\nilias\nting\nping\n12 34\ntest\ntest\ntest\n7\ntest\ntest\nend";
	QTest::newRow("long words")
		<< "hello world yipyip yeah\nabc def ghi ijk\nend"
		<< 0 << 1
		<< 5
		<< "hello\nworld\nyipyip\nyeah\nabc\ndef\nghi\nijk\nend";
	QTest::newRow("comments")
		<< "hello %world yipyip yeah\n%abc def ghi ijk\nend"
		<< 0 << 1
		<< 5
		<< "hello\n%world\n%yipyip\n%yeah\n%abc\n%def\n%ghi\n%ijk\nend";
	QTest::newRow("comments too long") //"x y z" is ok, "%x y z" not
		<< "hello x y z %x y z world a b c yipyip yeah\n%abc def ghi ijk\nend"
		<< 0 << 1
		<< 5
		<< "hello\nx y z\n%x y\n%z\n%world\n%a b\n%c\n%yipyip\n%yeah\n%abc\n%def\n%ghi\n%ijk\nend";
	QTest::newRow("comments and percent")
		<< "mui muo\\% mua muip abc %def ghi ijk\nend"
		<< 0 << 0
		<< 5
		<< "mui\nmuo\\%\nmua\nmuip\nabc\n%def\n%ghi\n%ijk\nend";

}
void LatexEditorViewTest::insertHardLineBreaks(){
	QFETCH(QString, text);
	QFETCH(int, start);
	QFETCH(int, end);
	QFETCH(int, length);
	QFETCH(QString, newText);

	edView->editor->setText(text, false);
	if (start==end)
		edView->editor->setCursor(edView->editor->document()->cursor(start,0,start,1));
	else
		edView->editor->setCursor(edView->editor->document()->cursor(start,0,end+1,0));
	edView->insertHardLineBreaks(length,false,false);
    edView->editor->document()->setLineEndingDirect(QDocument::Unix,true);
	QEQUAL(edView->editor->document()->text(), newText);

	if (start!=end) { //repeat with different cursor position
		edView->editor->setText(text, false);
		edView->editor->setCursor(edView->editor->document()->cursor(start,1,end,1));
		edView->insertHardLineBreaks(length, false, false);
        edView->editor->document()->setLineEndingDirect(QDocument::Unix,true);
		QEQUAL(edView->editor->document()->text(), newText);
	}
}
void LatexEditorViewTest::inMathEnvironment_data(){
	QTest::addColumn<QString>("text");
	QTest::addColumn<QString>("inmath");

	QTest::newRow("closed")
			<<  "a$bc$de\\[f\\]g"
            << "fftttffffttfff";

	QTest::newRow("open")
			<<  "xy$z"
			<< "ffftt";
}
void LatexEditorViewTest::inMathEnvironment(){
	QFETCH(QString, text);
	QFETCH(QString, inmath);
	edView->editor->setText(text);
    edView->document->startSyntaxChecker();
    
    edView->document->synChecker.waitForQueueProcess(); // wait for syntax checker to finish (as it runs in a parallel thread)

	QDocumentCursor c = edView->editor->document()->cursor(0,0);
	for (int i=0;i<inmath.size();i++) {
		c.setColumnNumber(i);
		bool posinmath = inmath.at(i) == 't';
		QEQUAL(edView->isInMathHighlighting(c), posinmath );
	}

}

void LatexEditorViewTest::vimEditingModeSwitches()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;

    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursorStyle(), QDocument::BlockCursorStyle);

    edView->getConfig()->editingMode = LatexEditorViewConfig::StandardEditing;
    edView->updateSettings();
    QEQUAL(edView->editor->inputModeLabel(), QString());
    QEQUAL(edView->editor->cursorStyle(), QDocument::AutoCursorStyle);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimCursorStyles()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("abc", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QEQUAL(edView->editor->cursorStyle(), QDocument::BlockCursorStyle);

    QTest::keyClick(edView->editor, Qt::Key_I);
    QEQUAL(edView->editor->cursorStyle(), QDocument::LineCursorStyle);

    QTest::keyClick(edView->editor, Qt::Key_Escape);
    QEQUAL(edView->editor->cursorStyle(), QDocument::BlockCursorStyle);

    QTest::keyClicks(edView->editor, "R");
    QEQUAL(edView->editor->cursorStyle(), QDocument::UnderlineCursorStyle);

    QTest::keyClick(edView->editor, Qt::Key_Escape);
    QEQUAL(edView->editor->cursorStyle(), QDocument::BlockCursorStyle);

    edView->getConfig()->editingMode = LatexEditorViewConfig::StandardEditing;
    edView->updateSettings();
    QEQUAL(edView->editor->cursorStyle(), QDocument::AutoCursorStyle);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimInsertEscape()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("abc", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_I);
    QTest::keyClicks(edView->editor, "X");
    QTest::keyClick(edView->editor, Qt::Key_Escape);

    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("Xabc"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursor().columnNumber(), 0);

    QTest::keyClick(edView->editor, Qt::Key_X);
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("abc"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimVisualLineStaysOnCurrentLine()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(1, 2, false);
    edView->editor->setFocus();

    QTest::keyClicks(edView->editor, "V");

    QEQUAL(edView->editor->inputModeLabel(), QString("V-LINE"));
    QEQUAL(edView->editor->cursor().lineNumber(), 1);
    QEQUAL(edView->editor->cursor().columnNumber(), QString("beta").length());
    QEQUAL(edView->editor->cursor().startLineNumber(), 1);
    QEQUAL(edView->editor->cursor().endLineNumber(), 1);

    QTest::keyClick(edView->editor, Qt::Key_J);

    QEQUAL(edView->editor->cursor().lineNumber(), 2);
    QEQUAL(edView->editor->cursor().startLineNumber(), 1);
    QEQUAL(edView->editor->cursor().endLineNumber(), 2);

    QTest::keyClick(edView->editor, Qt::Key_D);
    // The source had no trailing newline; deleting its tail keeps that property.
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("alpha"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimVisualBlockCtrlV()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

#ifdef Q_OS_MAC
    const Qt::KeyboardModifiers visualBlockModifier = Qt::MetaModifier;
    QKeyEvent visualBlockShortcut(QEvent::KeyPress, Qt::Key_V, visualBlockModifier);
    QCoreApplication::sendEvent(edView->editor, &visualBlockShortcut);
#else
    const Qt::KeyboardModifiers visualBlockModifier = Qt::ControlModifier;
    QKeyEvent visualBlockShortcutOverride(QEvent::ShortcutOverride, Qt::Key_V, visualBlockModifier);
    QCoreApplication::sendEvent(edView->editor, &visualBlockShortcutOverride);
    QVERIFY(visualBlockShortcutOverride.isAccepted());
    QTest::keyClick(edView->editor, Qt::Key_V, visualBlockModifier);
#endif

    QEQUAL(edView->editor->inputModeLabel(), QString("V-BLOCK"));
    QEQUAL(edView->editor->cursor().lineNumber(), 0);
    // The document cursor spans the inclusive visual block cell.
    QEQUAL(edView->editor->cursor().selectionStart().columnNumber(), 0);
    QEQUAL(edView->editor->cursor().selectedText(), QString("a"));

    QTest::keyClick(edView->editor, Qt::Key_J);

    QEQUAL(edView->editor->inputModeLabel(), QString("V-BLOCK"));
    QEQUAL(edView->editor->cursorMirrorCount(), 1);
    QVERIFY(edView->editor->cursor().hasSelection());
    QVERIFY(edView->editor->cursorMirror(0).hasSelection());
    QEQUAL(edView->editor->cursor().lineNumber(), 1);
    QEQUAL(edView->editor->cursorMirror(0).lineNumber(), 0);
    QStringList blockTexts;
    blockTexts << edView->editor->cursor().selectedText() << edView->editor->cursorMirror(0).selectedText();
    blockTexts.sort();
    QVERIFY(blockTexts == (QStringList() << "a" << "b"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimVisualBlockDeleteAffectsAllRows()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

#ifdef Q_OS_MAC
    const Qt::KeyboardModifiers visualBlockModifier = Qt::MetaModifier;
    QKeyEvent visualBlockShortcut(QEvent::KeyPress, Qt::Key_V, visualBlockModifier);
    QCoreApplication::sendEvent(edView->editor, &visualBlockShortcut);
#else
    const Qt::KeyboardModifiers visualBlockModifier = Qt::ControlModifier;
    QKeyEvent visualBlockShortcutOverride(QEvent::ShortcutOverride, Qt::Key_V, visualBlockModifier);
    QCoreApplication::sendEvent(edView->editor, &visualBlockShortcutOverride);
    QVERIFY(visualBlockShortcutOverride.isAccepted());
    QTest::keyClick(edView->editor, Qt::Key_V, visualBlockModifier);
#endif

    QTest::keyClick(edView->editor, Qt::Key_J);
    QTest::keyClick(edView->editor, Qt::Key_D);

    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("lpha\neta\ngamma"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursorMirrorCount(), 0);
    QTest::keyClicks(edView->editor, "u");
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("alpha\nbeta\ngamma"));
    QTest::keyClick(edView->editor, Qt::Key_R, Qt::ControlModifier);
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("lpha\neta\ngamma"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimVisualBlockInsertAtStart()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

#ifdef Q_OS_MAC
    const Qt::KeyboardModifiers visualBlockModifier = Qt::MetaModifier;
    QKeyEvent visualBlockShortcut(QEvent::KeyPress, Qt::Key_V, visualBlockModifier);
    QCoreApplication::sendEvent(edView->editor, &visualBlockShortcut);
#else
    const Qt::KeyboardModifiers visualBlockModifier = Qt::ControlModifier;
    QKeyEvent visualBlockShortcutOverride(QEvent::ShortcutOverride, Qt::Key_V, visualBlockModifier);
    QCoreApplication::sendEvent(edView->editor, &visualBlockShortcutOverride);
    QVERIFY(visualBlockShortcutOverride.isAccepted());
    QTest::keyClick(edView->editor, Qt::Key_V, visualBlockModifier);
#endif

    QTest::keyClick(edView->editor, Qt::Key_J);
    QTest::keyClicks(edView->editor, "I");
    QTest::keyClicks(edView->editor, "X");
    QTest::keyClick(edView->editor, Qt::Key_Escape);

    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("Xalpha\nXbeta\ngamma"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursorMirrorCount(), 0);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimCloseElementEscapesInsertMode()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("abc", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->window()->show();
    edView->window()->raise();
    edView->window()->activateWindow();
    edView->editor->setFocus();
    QVERIFY(QTest::qWaitForWindowActive(edView->window()));
    QVERIFY(edView->editor->hasFocus());

    QTest::keyClick(edView->editor, Qt::Key_I);
    QTest::keyClicks(edView->editor, "X");

    QVERIFY(edView->closeElement());
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("Xabc"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursor().columnNumber(), 0);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimCloseElementIsConsumedInNormalMode()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("abc", false);
    edView->editor->setCursorPosition(0, 1, false);
    edView->window()->show();
    edView->window()->raise();
    edView->window()->activateWindow();
    edView->editor->setFocus();
    QVERIFY(QTest::qWaitForWindowActive(edView->window()));
    QVERIFY(edView->editor->hasFocus());

    QVERIFY(edView->closeElement());
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("abc"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursor().columnNumber(), 1);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimPromptEnterDoesNotInsertNewline()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_Colon, Qt::ShiftModifier);

    QWidget *promptPanel = edView->findChild<QWidget *>(QStringLiteral("vimPromptPanel"));
    QVERIFY(promptPanel);
    QLineEdit *promptEdit = promptPanel->findChild<QLineEdit *>();
    QVERIFY(promptEdit);

    QTest::keyClicks(promptEdit, "2");
    QTest::keyClick(promptEdit, Qt::Key_Return);

    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("alpha\nbeta\ngamma"));
    QEQUAL(edView->editor->cursor().lineNumber(), 1);
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimPromptHistory()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_Colon, Qt::ShiftModifier);
    QWidget *promptPanel = edView->findChild<QWidget *>(QStringLiteral("vimPromptPanel"));
    QVERIFY(promptPanel);
    QLineEdit *promptEdit = promptPanel->findChild<QLineEdit *>();
    QVERIFY(promptEdit);

    QTest::keyClicks(promptEdit, "2");
    QTest::keyClick(promptEdit, Qt::Key_Return);

    QTest::keyClick(edView->editor, Qt::Key_Colon, Qt::ShiftModifier);
    QTest::keyClick(promptEdit, Qt::Key_Up);
    QEQUAL(promptEdit->text(), QString("2"));
    QTest::keyClick(promptEdit, Qt::Key_Down);
    QEQUAL(promptEdit->text(), QString());
    QTest::keyClick(promptEdit, Qt::Key_Escape);

    QTest::keyClick(edView->editor, Qt::Key_Slash);
    QTest::keyClicks(promptEdit, "beta");
    QTest::keyClick(promptEdit, Qt::Key_Return);

    QTest::keyClick(edView->editor, Qt::Key_Question, Qt::ShiftModifier);
    QTest::keyClick(promptEdit, Qt::Key_Up);
    QEQUAL(promptEdit->text(), QString("beta"));
    QTest::keyClick(promptEdit, Qt::Key_Escape);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimMarks()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("  alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 4, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_M);
    QTest::keyClick(edView->editor, Qt::Key_A);

    edView->editor->setCursorPosition(2, 1, false);

    QKeyEvent lineMarkJump(QEvent::KeyPress, Qt::Key_Apostrophe, Qt::NoModifier, QStringLiteral("'"));
    QCoreApplication::sendEvent(edView->editor, &lineMarkJump);
    QTest::keyClick(edView->editor, Qt::Key_A);

    QEQUAL(edView->editor->cursor().lineNumber(), 0);
    QEQUAL(edView->editor->cursor().columnNumber(), 2);

    edView->editor->setCursorPosition(2, 1, false);

    QKeyEvent exactMarkJump(QEvent::KeyPress, Qt::Key_QuoteLeft, Qt::NoModifier, QStringLiteral("`"));
    QCoreApplication::sendEvent(edView->editor, &exactMarkJump);
    QTest::keyClick(edView->editor, Qt::Key_A);

    QEQUAL(edView->editor->cursor().lineNumber(), 0);
    QEQUAL(edView->editor->cursor().columnNumber(), 4);

    QKeyEvent previousJump(QEvent::KeyPress, Qt::Key_QuoteLeft, Qt::NoModifier, QStringLiteral("`"));
    QCoreApplication::sendEvent(edView->editor, &previousJump);
    QCoreApplication::sendEvent(edView->editor, &previousJump);

    QEQUAL(edView->editor->cursor().lineNumber(), 2);
    QEQUAL(edView->editor->cursor().columnNumber(), 1);

    edView->editor->setText("one\ntwo\nthree", false);
    edView->editor->setCursorPosition(2, 1, false);
    QTest::keyClick(edView->editor, Qt::Key_M);
    QTest::keyClick(edView->editor, Qt::Key_A);

    edView->editor->setCursorPosition(0, 0, false);
    QTest::keyClick(edView->editor, Qt::Key_D);
    QKeyEvent linewiseDeleteToMark(QEvent::KeyPress, Qt::Key_Apostrophe, Qt::NoModifier, QStringLiteral("'"));
    QCoreApplication::sendEvent(edView->editor, &linewiseDeleteToMark);
    QTest::keyClick(edView->editor, Qt::Key_A);

    QEQUAL(edView->editor->document()->textLines().join("\n"), QString(""));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->editor->setText("one\ntwo\nthree", false);
    edView->editor->setCursorPosition(0, 0, false);
    QTest::keyClick(edView->editor, Qt::Key_M);
    QTest::keyClick(edView->editor, Qt::Key_A);

    edView->editor->setCursorPosition(2, 0, false);
    QTest::keyClick(edView->editor, Qt::Key_Y);
    QKeyEvent linewiseYankToMark(QEvent::KeyPress, Qt::Key_Apostrophe, Qt::NoModifier, QStringLiteral("'"));
    QCoreApplication::sendEvent(edView->editor, &linewiseYankToMark);
    QTest::keyClick(edView->editor, Qt::Key_A);
    QTest::keyClick(edView->editor, Qt::Key_P);

    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("one\ntwo\nthree\none\ntwo\nthree"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimDeleteLastLineMovesToPreviousLine()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("one\n  two\nthree", false);
    edView->editor->setCursorPosition(2, 0, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_D);
    QTest::keyClick(edView->editor, Qt::Key_D);

    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("one\n  two"));
    QEQUAL(edView->editor->cursor().lineNumber(), 1);
    QEQUAL(edView->editor->cursor().columnNumber(), 2);
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimLinewisePasteKeepsCursorOnInsertedText()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("    alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QTest::keyClicks(edView->editor, "Y");
    QTest::keyClick(edView->editor, Qt::Key_P);

    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("    alpha\n    alpha\nbeta\ngamma"));
    QEQUAL(edView->editor->cursor().lineNumber(), 1);
    QEQUAL(edView->editor->cursor().columnNumber(), 4);
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimNormalModeConsumesUnhandledPrintableKeys()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("abc", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QTest::keyClicks(edView->editor, "H");

    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("abc"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimExSubstituteCommands()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("foo foo\nfoo foo\nbar", false);
    edView->editor->setCursorPosition(0, 0, false);

    QVERIFY(edView->executeVimExCommand("s/foo/X/"));
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("X foo\nfoo foo\nbar"));

    QVERIFY(edView->executeVimExCommand("%s/foo/Y/g"));
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("X Y\nY Y\nbar"));

    edView->editor->setText("foo foo\nfoo foo\nfoo foo", false);
    QVERIFY(edView->executeVimExCommand("1,2s/foo/Z/"));
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("Z foo\nZ foo\nfoo foo"));

    edView->executeVimSearch("foo", false);
    edView->editor->setCursorPosition(2, 0, false);
    QVERIFY(edView->executeVimExCommand("s//Q/g"));
    QEQUAL(edView->editor->document()->textLines().join("\n"), QString("Z foo\nZ foo\nQ Q"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimExCommands()
{
    if (skipVimUiTestInQuickRuns())
        return;

    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 0, false);

    QVERIFY(edView->executeVimExCommand("2"));
    QEQUAL(edView->editor->cursor().lineNumber(), 1);

    {
        LatexDocument isolatedDocument;
        LatexEditorView isolatedView(nullptr, edView->getConfig(), &isolatedDocument);
        isolatedView.getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
        isolatedView.updateSettings();

        QSignalSpy commandSpy(&isolatedView, SIGNAL(vimCommandRequested(QString)));
        QVERIFY(commandSpy.isValid());
        QVERIFY(isolatedView.executeVimExCommand(":w"));
        QEQUAL(commandSpy.count(), 1);
        const QList<QVariant> commandArguments = commandSpy.takeFirst();
        QEQUAL(commandArguments.at(0).toString(), QString("w"));
    }

    edView->executeVimSearch("beta", false);
    QEQUAL(edView->getSearchText(), QString("beta"));
    QVERIFY(edView->executeVimExCommand("noh"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}


void LatexEditorViewTest::vimRegisterCommands_data()
{
    QTest::addColumn<QString>("initial");
    QTest::addColumn<QString>("keys");
    QTest::addColumn<QString>("expected");
    QTest::newRow("named character yank") << "abc" << "\"ayl\"ap" << "aabc";
    QTest::newRow("named line yank survives delete") << "one\ntwo\nthree" << "\"ayyjdd\"aP" << "one\none\nthree";
    QTest::newRow("uppercase append character") << "abc" << "\"ayl\"Ayl\"ap" << "aaabc";
    QTest::newRow("uppercase append lines") << "one\ntwo\nthree" << "\"ayyj\"Ayy\"aP" << "one\none\ntwo\ntwo\nthree";
    QTest::newRow("black hole preserves unnamed") << "abc" << "yl\"_xp" << "bac";
    QTest::newRow("yank history survives delete") << "one\ntwo\nthree" << "yyjdd\"0P" << "one\none\nthree";
    QTest::newRow("small delete register") << "abc" << "xyl\"-p" << "bac";
    QTest::newRow("numbered delete rotation") << "one\ntwo\nthree" << "dddd\"2P" << "one\nthree";
    QTest::newRow("gg from last line") << "one\ntwo\nthree" << "Gggdd" << "two\nthree";
    QTest::newRow("replace with command character") << "abc" << "rx" << "xbc";
    QTest::newRow("replace with digit") << "abc" << "r1" << "1bc";
    QTest::newRow("find command character") << "abcabc" << "fax" << "abcbc";
    QTest::newRow("named paste count") << "abc" << "\"ayl\"a3p" << "aaaabc";
    QTest::newRow("count before register") << "one\ntwo\nthree" << "2\"ayyG\"aP" << "one\ntwo\none\ntwo\nthree";
    QTest::newRow("visual named yank") << "abc" << "vl\"ay\"aP" << "ababc";
    QTest::newRow("visual paste uses original source") << "abc" << "ylvlp" << "ac";
    QTest::newRow("explicit visual paste") << "abc" << "\"ayl vl\"ap" << "ac";
    QTest::newRow("replace does not overwrite yank") << "abc" << "ylrzp" << "zabc";
    QTest::newRow("register selection ends after command") << "abc" << "\"aylx\"ap" << "bac";
    QTest::newRow("repeat named deletion") << "abcd" << "\"ax.\"aP" << "bcd";
    QTest::newRow("repeat named paste") << "abc" << "\"ayl\"ap." << "aaabc";
    QTest::newRow("visual paste preserves named source") << "abc" << "\"ayl vl\"ap\"aP" << "aac";
    QTest::newRow("visual paste last character") << "abc" << "yl$vp" << "aba";
    QTest::newRow("visual paste undo") << "abc" << "ylvlpu" << "abc";
    QTest::newRow("empty visual paste is harmless") << "abc" << "vl\"zp" << "abc";
    QTest::newRow("repeat named line delete") << "one\ntwo\nthree" << "\"add.\"aP" << "two\nthree";
    QTest::newRow("line paste at EOF") << "one\ntwo" << "yyGp" << "one\ntwo\none";
    QTest::newRow("counted paste undo") << "abc" << "\"ayl\"a3pu" << "abc";
    QTest::newRow("named delete text object") << "word next" << "\"adiw" << " next";
    QTest::newRow("delete word end inclusive") << "word next" << "de" << " next";
    QTest::newRow("delete next word exclusive") << "word next" << "dw" << "next";
    QTest::newRow("delete counted words") << "one two\nthree" << "d2w" << "three";
    QTest::newRow("yank word end") << "word next" << "yeP" << "wordword next";
    QTest::newRow("left stays on line") << "one\ntwo" << "j99h" << "one\ntwo";
    QTest::newRow("text object named yank") << "word next" << "\"ayiw\"aP" << "wordword next";
}

void LatexEditorViewTest::vimRegisterCommands()
{
    if (skipVimUiTestInQuickRuns())
        return;
    QFETCH(QString, initial);
    QFETCH(QString, keys);
    QFETCH(QString, expected);
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();
    vimRegisters() = VimRegisters();
    edView->editor->setText(initial, false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();
    QTest::keyClicks(edView->editor, keys);
    const QString actual = edView->editor->document()->textLines().join("\n");
    const QString mode = edView->editor->inputModeLabel();
    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
    QCOMPARE(actual, expected);
    QCOMPARE(mode, QString("NORMAL"));
}

void LatexEditorViewTest::vimPhysicalModifierEvents()
{
    if (skipVimUiTestInQuickRuns())
        return;
    LatexEditorViewConfig config = *edView->getConfig();
    config.editingMode = LatexEditorViewConfig::VimEditing;
    LatexDocument document;
    LatexEditorView view(nullptr, &config, &document);
    view.editor->setText("one\ntwo\nthree", false);
    view.editor->setCursorPosition(0, 0, false);
    vimRegisters() = VimRegisters();
    QTest::keyClicks(view.editor, "\"ayyjdd\"a");
    QTest::keyPress(view.editor, Qt::Key_Shift);
    QTest::keyClicks(view.editor, "P", Qt::ShiftModifier);
    QTest::keyRelease(view.editor, Qt::Key_Shift);
    QCOMPARE(view.editor->document()->textLines().join("\n"), QString("one\none\nthree"));
    // Shift is also separate when selecting an uppercase append destination.
    QTest::keyClick(view.editor, Qt::Key_QuoteDbl, Qt::ShiftModifier);
    QTest::keyPress(view.editor, Qt::Key_Shift);
    QTest::keyClicks(view.editor, "A", Qt::ShiftModifier);
    QTest::keyRelease(view.editor, Qt::Key_Shift);
    QTest::keyClicks(view.editor, "yy");
    QCOMPARE(vimRegisters().read('a').text, QString("one\none\n"));
    QTest::keyClicks(view.editor, "d");
    QTest::keyPress(view.editor, Qt::Key_Control);
    QTest::keyRelease(view.editor, Qt::Key_Control);
    QTest::keyClicks(view.editor, "d");
    QCOMPARE(view.editor->document()->textLines().join("\n"), QString("one\nthree"));
    // Typing uppercase text must also remain repeatable with dot.
    view.editor->setText("abc", false);
    view.editor->setCursorPosition(0, 0, false);
    QTest::keyClicks(view.editor, "i");
    QTest::keyPress(view.editor, Qt::Key_Shift);
    QTest::keyClicks(view.editor, "X", Qt::ShiftModifier);
    QTest::keyRelease(view.editor, Qt::Key_Shift);
    QTest::keyClick(view.editor, Qt::Key_BracketLeft, Qt::ControlModifier);
    QCOMPARE(view.editor->inputModeLabel(), QString("NORMAL"));
    QTest::keyClicks(view.editor, "l.");
    QCOMPARE(view.editor->document()->textLines().join("\n"), QString("XXabc"));
}

void LatexEditorViewTest::vimRegisterStore()
{
    VimRegisters registers;
    const VimRegister first{VimRegisterType::LineWise, "first\n", {}};
    const VimRegister second{VimRegisterType::LineWise, "second\n", {}};
    registers.write('"', first, true);
    registers.write('"', second, false);
    QCOMPARE(registers.read('0').text, first.text);
    QCOMPARE(registers.read('1').text, second.text);
    registers.write('_', first, false);
    QCOMPARE(registers.read('"').text, second.text);
    QCOMPARE(registers.read('1').text, second.text);
    registers.write('a', first, true);
    registers.write('A', second, true);
    QCOMPARE(registers.read('a').text, QString("first\nsecond\n"));
    QCOMPARE(registers.read('A').text, registers.read('a').text);
    QCOMPARE(registers.read('0').text, first.text);
    const VimRegister block{VimRegisterType::BlockWise, "a\nb", {"a", "b"}};
    registers.write('b', block, true);
    registers.write('B', block, true);
    QCOMPARE(registers.read('b').blocks, QStringList({"aa", "bb"}));
    QCOMPARE(registers.read('b').type, VimRegisterType::BlockWise);
    for (int i = 1; i <= 10; ++i)
        registers.write('"', {VimRegisterType::LineWise, QString::number(i) + "\n", {}}, false);
    QCOMPARE(registers.read('1').text, QString("10\n"));
    QCOMPARE(registers.read('9').text, QString("2\n"));
    registers.write('"', {VimRegisterType::CharacterWise, "x", {}}, false);
    QCOMPARE(registers.read('-').text, QString("x"));
    QCOMPARE(registers.read('1').text, QString("10\n"));
}

void LatexEditorViewTest::vimRegistersSharedAcrossViews()
{
    if (skipVimUiTestInQuickRuns())
        return;
    LatexEditorViewConfig config = *edView->getConfig();
    config.editingMode = LatexEditorViewConfig::VimEditing;
    LatexDocument firstDocument, secondDocument;
    LatexEditorView first(nullptr, &config, &firstDocument);
    LatexEditorView second(nullptr, &config, &secondDocument);
    first.editor->setText("source", false);
    first.editor->setCursorPosition(0, 0, false);
    QTest::keyClicks(first.editor, "\"zyiw");
    second.editor->setText("target", false);
    second.editor->setCursorPosition(0, 0, false);
    QTest::keyClicks(second.editor, "\"zP");
    QCOMPARE(second.editor->document()->textLines().join("\n"), QString("sourcetarget"));
    QTest::keyClicks(second.editor, "\"");
    // This view is isolated from the main window's global Escape action.
    QTest::keyClick(second.editor, Qt::Key_BracketLeft, Qt::ControlModifier);
    QTest::keyClicks(second.editor, "x");
    QCOMPARE(second.editor->document()->textLines().join("\n"), QString("sourctarget"));
    QCOMPARE(second.editor->inputModeLabel(), QString("NORMAL"));
    QCOMPARE(vimRegisters().read('z').text, QString("source"));
}

void LatexEditorViewTest::vimClipboardRegisters()
{
    if (skipVimUiTestInQuickRuns())
        return;
    QClipboard *clipboard = QApplication::clipboard();
    const QString previous = clipboard->text();
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();
    edView->editor->setText("one\ntwo", false);
    edView->editor->setCursorPosition(0, 0, false);
    QTest::keyClicks(edView->editor, "\"+yy");
    const QString yanked = clipboard->text();
    QTest::keyClicks(edView->editor, "j\"+P");
    const QString pasted = edView->editor->document()->textLines().join("\n");
    clipboard->setText("external");
    edView->editor->setText("target", false);
    edView->editor->setCursorPosition(0, 0, false);
    QTest::keyClicks(edView->editor, "\"+P");
    const QString externalPaste = edView->editor->document()->textLines().join("\n");
    const VimRegister block{VimRegisterType::BlockWise, "a\nb", {"a", "b"}};
    vimRegisters().write('+', block, true);
    const VimRegister restored = vimRegisters().read('+');
    clipboard->setText(previous);
    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
    QCOMPARE(yanked, QString("one\n"));
    QCOMPARE(pasted, QString("one\none\ntwo"));
    QCOMPARE(externalPaste, QString("externaltarget"));
    QCOMPARE(restored.type, VimRegisterType::BlockWise);
    QCOMPARE(restored.blocks, block.blocks);
}

void LatexEditorViewTest::vimDesktopClipboard()
{
    if (skipVimUiTestInQuickRuns())
        return;
    qInfo() << "desktop platform:" << QGuiApplication::platformName()
            << "primary selection:" << QApplication::clipboard()->supportsSelection();
    LatexEditorViewConfig config = *edView->getConfig();
    config.editingMode = LatexEditorViewConfig::VimEditing;
    LatexDocument document;
    LatexEditorView view(nullptr, &config, &document);
    view.resize(900, 500);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.editor->setText("original", false);
    // Register payloads use LF, while saving must retain the document format.
    view.editor->document()->setLineEndingDirect(QDocument::Windows, true);
    view.editor->setCursorPosition(0, 0, false);
    view.editor->setFocus();
    QClipboard *clipboard = QApplication::clipboard();
    const QString previous = clipboard->text();
    clipboard->setText(QString::fromUtf8("αβ\r\n第二行\r\n"));
    const VimRegister external = vimRegisters().read('+');
    QTest::keyClicks(view.editor, "\"+P");
    const QString actual = view.editor->document()->textLines().join("\n");
    clipboard->setText(previous);
    QCOMPARE(external.text, QString::fromUtf8("αβ\n第二行\n"));
    QCOMPARE(external.type, VimRegisterType::LineWise);
    QCOMPARE(actual, QString::fromUtf8("αβ\n第二行\noriginal"));
    QCOMPARE(view.editor->document()->text(), QString::fromUtf8("αβ\r\n第二行\r\noriginal"));

    const VimRegister payload{VimRegisterType::LineWise, "primary\n", {}};
    vimRegisters().write('*', payload, true);
    const VimRegister restored = vimRegisters().read('*');
    QCOMPARE(restored.text, payload.text);
    QCOMPARE(restored.type, payload.type);
    const QString screenshotDir = qEnvironmentVariable("TEXSTUDIO_TEST_SCREENSHOT_DIR");
    if (!screenshotDir.isEmpty()) {
        QVERIFY(QDir().mkpath(screenshotDir));
        QTest::qWait(50);
        QVERIFY(view.grab().save(screenshotDir + "/vim-desktop.png"));
    }
    view.hide();
}


void LatexEditorViewTest::vimDocumentedModes_data()
{
    QTest::addColumn<QString>("keys");
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("expected");
    QTest::newRow("insert") << "iZ" << "INSERT" << "Zabc";
    QTest::newRow("append") << "aZ" << "INSERT" << "aZbc";
    QTest::newRow("insert-start") << "lIZ" << "INSERT" << "Zabc";
    QTest::newRow("append-end") << "AZ" << "INSERT" << "abcZ";
    QTest::newRow("open-below") << "oZ" << "INSERT" << "abc\nZ";
    QTest::newRow("open-above") << "OZ" << "INSERT" << "Z\nabc";
    QTest::newRow("replace-overwrite") << "RXY" << "REPLACE" << "XYc";
    QTest::newRow("visual-char") << "vld" << "NORMAL" << "c";
    QTest::newRow("visual-line") << "Vd" << "NORMAL" << "";
    QTest::newRow("single-replace") << "rZ" << "NORMAL" << "Zbc";
    QTest::newRow("substitute-char") << "sZ" << "INSERT" << "Zbc";
    QTest::newRow("change-line") << "ccZ" << "INSERT" << "Z";
    QTest::newRow("delete-end") << "lD" << "NORMAL" << "a";
    QTest::newRow("change-end") << "lCZ" << "INSERT" << "aZ";
}

void LatexEditorViewTest::vimDocumentedModes()
{
    if (skipVimUiTestInQuickRuns()) return;
    QFETCH(QString, keys);
    QFETCH(QString, mode);
    QFETCH(QString, expected);
    LatexEditorViewConfig config = *edView->getConfig();
    config.editingMode = LatexEditorViewConfig::VimEditing;
    config.autoindent = false;
    config.parenComplete = false;
    LatexDocument document;
    LatexEditorView view(nullptr, &config, &document);
    view.editor->setText("abc", false);
    view.editor->setCursorPosition(0, 0, false);
    QTest::keyClicks(view.editor, keys);
    QCOMPARE(view.editor->inputModeLabel(), mode);
    QCOMPARE(document.textLines().join("\n"), expected);
    QTest::keyClick(view.editor, Qt::Key_BracketLeft, Qt::ControlModifier);
    QCOMPARE(view.editor->inputModeLabel(), QString("NORMAL"));
}

void LatexEditorViewTest::vimSubstituteFlags_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<bool>("handled");
    QTest::newRow("current-first") << "s/foo/X/" << "X foo\nFOO foo\nfoo" << true;
    QTest::newRow("current-global") << "s/foo/X/g" << "X X\nFOO foo\nfoo" << true;
    QTest::newRow("all-lines") << "%s/foo/X/g" << "X X\nFOO X\nX" << true;
    QTest::newRow("numeric-range") << "2,3s/foo/X/" << "foo foo\nFOO X\nX" << true;
    QTest::newRow("case-insensitive") << "%s/foo/X/gi" << "X X\nX X\nX" << true;
    QTest::newRow("case-sensitive") << "%s/foo/X/gI" << "X X\nFOO X\nX" << true;
    QTest::newRow("alternate-delimiter") << "s#foo#X#g" << "X X\nFOO foo\nfoo" << true;
    QTest::newRow("matched-text") << "s/foo/[&]/" << "[foo] foo\nFOO foo\nfoo" << true;
    QTest::newRow("regex") << "%s/f.o/X/g" << "X X\nFOO X\nX" << true;
    QTest::newRow("invalid-flag") << "s/foo/X/z" << "foo foo\nFOO foo\nfoo" << false;
    QTest::newRow("missing-pattern") << "s/absent/X/" << "foo foo\nFOO foo\nfoo" << false;
}

void LatexEditorViewTest::vimSubstituteFlags()
{
    if (skipVimUiTestInQuickRuns()) return;
    QFETCH(QString, command);
    QFETCH(QString, expected);
    QFETCH(bool, handled);
    LatexEditorViewConfig config = *edView->getConfig();
    config.editingMode = LatexEditorViewConfig::VimEditing;
    LatexDocument document;
    LatexEditorView view(nullptr, &config, &document);
    view.editor->setText("foo foo\nFOO foo\nfoo", false);
    view.editor->setCursorPosition(0, 0, false);
    QCOMPARE(view.executeVimExCommand(command), handled);
    QCOMPARE(document.textLines().join("\n"), expected);
}


void LatexEditorViewTest::vimSearchNavigation()
{
    if (skipVimUiTestInQuickRuns()) return;
    LatexEditorViewConfig config = *edView->getConfig();
    config.editingMode = LatexEditorViewConfig::VimEditing;
    LatexDocument document;
    LatexEditorView view(nullptr, &config, &document);
    view.editor->setText("start\nneedle\nother\nneedle\nend", false);
    view.editor->setCursorPosition(0, 0, false);
    QTest::keyClicks(view.editor, "/");
    QWidget *panel = view.findChild<QWidget *>("vimPromptPanel");
    QVERIFY(panel);
    QLineEdit *prompt = panel->findChild<QLineEdit *>();
    QVERIFY(prompt);
    QTest::keyClicks(prompt, "needle");
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(view.editor->cursor().lineNumber(), 1);
    QTest::keyClicks(view.editor, "n");
    QCOMPARE(view.editor->cursor().lineNumber(), 3);
    QTest::keyClicks(view.editor, "N");
    QCOMPARE(view.editor->cursor().lineNumber(), 1);
    view.editor->setCursorPosition(4, 0, false);
    QTest::keyClicks(view.editor, "?");
    QTest::keyClicks(prompt, "needle");
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(view.editor->cursor().lineNumber(), 3);
    QTest::keyClicks(view.editor, "n");
    QCOMPARE(view.editor->cursor().lineNumber(), 1);
    QTest::keyClicks(view.editor, "N");
    QCOMPARE(view.editor->cursor().lineNumber(), 3);
    // Cancelling a prompt must leave both text and cursor untouched.
    const int line = view.editor->cursor().lineNumber();
    QTest::keyClicks(view.editor, "/");
    QTest::keyClicks(prompt, "end");
    QTest::keyClick(prompt, Qt::Key_BracketLeft, Qt::ControlModifier);
    QCOMPARE(view.editor->cursor().lineNumber(), line);
    QCOMPARE(document.textLines().join("\n"), QString("start\nneedle\nother\nneedle\nend"));
}

void LatexEditorViewTest::vimSubstitutePrompt()
{
    if (skipVimUiTestInQuickRuns()) return;
    LatexEditorViewConfig config = *edView->getConfig();
    config.editingMode = LatexEditorViewConfig::VimEditing;
    LatexDocument document;
    LatexEditorView view(nullptr, &config, &document);
    view.editor->setText("foo foo\nfoo", false);
    QTest::keyClicks(view.editor, ":");
    QWidget *panel = view.findChild<QWidget *>("vimPromptPanel");
    QVERIFY(panel);
    QLineEdit *prompt = panel->findChild<QLineEdit *>();
    QVERIFY(prompt);
    QTest::keyClicks(prompt, "%s/foo/X/g");
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(document.textLines().join("\n"), QString("X X\nX"));
    QCOMPARE(view.editor->inputModeLabel(), QString("NORMAL"));
    QTest::keyClicks(view.editor, "u");
    QCOMPARE(document.textLines().join("\n"), QString("foo foo\nfoo"));
    QKeyEvent redoOverride(QEvent::ShortcutOverride, Qt::Key_R, Qt::ControlModifier);
    QCoreApplication::sendEvent(view.editor, &redoOverride);
    QVERIFY(redoOverride.isAccepted());
    QTest::keyClick(view.editor, Qt::Key_R, Qt::ControlModifier);
    QCOMPARE(document.textLines().join("\n"), QString("X X\nX"));
#ifdef Q_OS_MAC
    QTest::keyClicks(view.editor, "u");
    QKeyEvent physicalControlRedo(QEvent::ShortcutOverride, Qt::Key_R, Qt::MetaModifier);
    QCoreApplication::sendEvent(view.editor, &physicalControlRedo);
    QVERIFY(physicalControlRedo.isAccepted());
    QTest::keyClick(view.editor, Qt::Key_R, Qt::MetaModifier);
    QCOMPARE(document.textLines().join("\n"), QString("X X\nX"));
#endif
    QTest::keyClicks(view.editor, ":");
    QTest::keyClicks(prompt, "s/X/Y/z");
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(document.textLines().join("\n"), QString("X X\nX"));
    QVERIFY(!panel->isHidden());
    prompt->selectAll();
    QTest::keyClicks(prompt, "%s/X/Y/g");
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(document.textLines().join("\n"), QString("Y Y\nY"));
}


void LatexEditorViewTest::vimDocumentedMotions_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("startLine");
    QTest::addColumn<int>("startColumn");
    QTest::addColumn<QString>("keys");
    QTest::addColumn<int>("line");
    QTest::addColumn<int>("column");
    const QString text = "  one two\nabc def\nlast";
    QTest::newRow("left") << text << 0 << 3 << "h" << 0 << 2;
    QTest::newRow("right-count") << text << 0 << 0 << "3l" << 0 << 3;
    QTest::newRow("down") << text << 0 << 3 << "j" << 1 << 3;
    QTest::newRow("up") << text << 1 << 3 << "k" << 0 << 3;
    QTest::newRow("down-clamps-column") << text << 0 << 8 << "2j" << 2 << 3;
    QTest::newRow("vertical-restores-column") << QString("abcdef\nx\nabcdef") << 0 << 4 << "jj" << 2 << 4;
    QTest::newRow("vertical-count-restores-column") << QString("abcdef\nx\nabcdef") << 0 << 4 << "2j" << 2 << 4;
    QTest::newRow("line-start") << text << 0 << 5 << "0" << 0 << 0;
    QTest::newRow("first-nonblank") << text << 0 << 0 << "^" << 0 << 2;
    QTest::newRow("line-end") << text << 0 << 0 << "$" << 0 << 8;
    QTest::newRow("first-line") << text << 2 << 0 << "gg" << 0 << 0;
    QTest::newRow("last-line") << text << 0 << 0 << "G" << 2 << 0;
    QTest::newRow("counted-line") << text << 0 << 0 << "2G" << 1 << 0;
    QTest::newRow("word-forward") << text << 0 << 2 << "w" << 0 << 6;
    QTest::newRow("word-backward") << text << 0 << 6 << "b" << 0 << 2;
    QTest::newRow("word-end") << text << 0 << 2 << "e" << 0 << 4;
    QTest::newRow("word-count") << text << 0 << 2 << "2w" << 1 << 0;
    const QString find = "abaca";
    QTest::newRow("find-forward") << find << 0 << 0 << "fa" << 0 << 2;
    QTest::newRow("find-backward") << find << 0 << 4 << "Fa" << 0 << 2;
    QTest::newRow("till-forward") << find << 0 << 0 << "ta" << 0 << 1;
    QTest::newRow("till-backward") << find << 0 << 4 << "Ta" << 0 << 3;
    QTest::newRow("find-repeat") << find << 0 << 0 << "fa;" << 0 << 4;
    QTest::newRow("find-reverse") << find << 0 << 0 << "fa;," << 0 << 2;
    QTest::newRow("find-count") << find << 0 << 0 << "2fa" << 0 << 4;
    QTest::newRow("find-missing") << find << 0 << 0 << "fz" << 0 << 0;
    QTest::newRow("left-boundary") << text << 0 << 0 << "9h" << 0 << 0;
    QTest::newRow("right-boundary") << text << 0 << 0 << "99l" << 0 << 8;
}

void LatexEditorViewTest::vimDocumentedMotions()
{
    if (skipVimUiTestInQuickRuns()) return;
    QFETCH(QString, text);
    QFETCH(int, startLine);
    QFETCH(int, startColumn);
    QFETCH(QString, keys);
    QFETCH(int, line);
    QFETCH(int, column);
    LatexEditorViewConfig config = *edView->getConfig();
    config.editingMode = LatexEditorViewConfig::VimEditing;
    LatexDocument document;
    LatexEditorView view(nullptr, &config, &document);
    view.editor->setText(text, false);
    view.editor->setCursorPosition(startLine, startColumn, false);
    QTest::keyClicks(view.editor, keys);
    QCOMPARE(view.editor->cursor().lineNumber(), line);
    QCOMPARE(view.editor->cursor().columnNumber(), column);
    QCOMPARE(document.textLines().join("\n"), text);
}


void LatexEditorViewTest::vimSubstituteConfirmation()
{
    if (skipVimUiTestInQuickRuns()) return;
    LatexEditorViewConfig config = *edView->getConfig();
    config.editingMode = LatexEditorViewConfig::VimEditing;
    LatexDocument document;
    LatexEditorView view(nullptr, &config, &document);
    view.editor->setText("foo foo foo", false);
    int confirmations = 0;
    bool unexpectedDialog = false;
    QTimer responder;
    QObject::connect(&responder, &QTimer::timeout, [&]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            QMessageBox *box = qobject_cast<QMessageBox *>(widget);
            if (!box || !box->isVisible()) continue;
            if (box->standardButtons().testFlag(QMessageBox::Yes)
                    && box->standardButtons().testFlag(QMessageBox::No)) {
                ++confirmations;
                box->button(confirmations == 2 ? QMessageBox::No : QMessageBox::Yes)->click();
            } else {
                unexpectedDialog = true;
                box->reject();
            }
        }
    });
    responder.start(10);
    const bool handled = view.executeVimExCommand("s/foo/X/gc");
    responder.stop();
    QVERIFY(handled);
    QVERIFY(!unexpectedDialog);
    QCOMPARE(confirmations, 3);
    QCOMPARE(document.textLines().join("\n"), QString("X foo X"));
    QTest::keyClicks(view.editor, "u");
    QCOMPARE(document.textLines().join("\n"), QString("foo foo foo"));
}

#endif

