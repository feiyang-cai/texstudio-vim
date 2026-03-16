
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
#include <QtTest/QtTest>
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

    QTest::keyClick(edView->editor, Qt::Key_R, Qt::ShiftModifier);
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
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("abc", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_I);
    QTest::keyClicks(edView->editor, "X");
    QTest::keyClick(edView->editor, Qt::Key_Escape);

    QEQUAL(edView->editor->document()->text(), QString("Xabc"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursor().columnNumber(), 0);

    QTest::keyClick(edView->editor, Qt::Key_X);
    QEQUAL(edView->editor->document()->text(), QString("abc"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimVisualLineStaysOnCurrentLine()
{
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(1, 2, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_V, Qt::ShiftModifier);

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
    QEQUAL(edView->editor->document()->text(), QString("alpha\n"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimVisualBlockCtrlV()
{
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
    QTest::keyClick(edView->editor, Qt::Key_V, visualBlockModifier);
#endif

    QEQUAL(edView->editor->inputModeLabel(), QString("V-BLOCK"));
    QEQUAL(edView->editor->cursor().lineNumber(), 0);
    QEQUAL(edView->editor->cursor().columnNumber(), 0);

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
    QTest::keyClick(edView->editor, Qt::Key_V, visualBlockModifier);
#endif

    QTest::keyClick(edView->editor, Qt::Key_J);
    QTest::keyClick(edView->editor, Qt::Key_D);

    QEQUAL(edView->editor->document()->text(), QString("lpha\neta\ngamma"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursorMirrorCount(), 0);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimVisualBlockInsertAtStart()
{
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
    QTest::keyClick(edView->editor, Qt::Key_V, visualBlockModifier);
#endif

    QTest::keyClick(edView->editor, Qt::Key_J);
    QTest::keyClick(edView->editor, Qt::Key_I, Qt::ShiftModifier);
    QTest::keyClicks(edView->editor, "X");
    QTest::keyClick(edView->editor, Qt::Key_Escape);

    QEQUAL(edView->editor->document()->text(), QString("Xalpha\nXbeta\ngamma"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursorMirrorCount(), 0);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimCloseElementEscapesInsertMode()
{
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("abc", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_I);
    QTest::keyClicks(edView->editor, "X");

    QVERIFY(edView->closeElement());
    QEQUAL(edView->editor->document()->text(), QString("Xabc"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursor().columnNumber(), 0);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimCloseElementIsConsumedInNormalMode()
{
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("abc", false);
    edView->editor->setCursorPosition(0, 1, false);
    edView->editor->setFocus();

    QVERIFY(edView->closeElement());
    QEQUAL(edView->editor->document()->text(), QString("abc"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));
    QEQUAL(edView->editor->cursor().columnNumber(), 1);

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimPromptEnterDoesNotInsertNewline()
{
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

    QEQUAL(edView->editor->document()->text(), QString("alpha\nbeta\ngamma"));
    QEQUAL(edView->editor->cursor().lineNumber(), 1);
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimPromptHistory()
{
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

    QEQUAL(edView->editor->document()->text(), QString(""));
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

    QEQUAL(edView->editor->document()->text(), QString("one\ntwo\nthree\none\ntwo\nthree\n"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimDeleteLastLineMovesToPreviousLine()
{
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("one\n  two\nthree", false);
    edView->editor->setCursorPosition(2, 0, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_D);
    QTest::keyClick(edView->editor, Qt::Key_D);

    QEQUAL(edView->editor->document()->text(), QString("one\n  two"));
    QEQUAL(edView->editor->cursor().lineNumber(), 1);
    QEQUAL(edView->editor->cursor().columnNumber(), 2);
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimLinewisePasteKeepsCursorOnInsertedText()
{
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("    alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_Y, Qt::ShiftModifier);
    QTest::keyClick(edView->editor, Qt::Key_P);

    QEQUAL(edView->editor->document()->text(), QString("    alpha\n    alpha\nbeta\ngamma"));
    QEQUAL(edView->editor->cursor().lineNumber(), 1);
    QEQUAL(edView->editor->cursor().columnNumber(), 4);
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimNormalModeConsumesUnhandledPrintableKeys()
{
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("abc", false);
    edView->editor->setCursorPosition(0, 0, false);
    edView->editor->setFocus();

    QTest::keyClick(edView->editor, Qt::Key_H, Qt::ShiftModifier);

    QEQUAL(edView->editor->document()->text(), QString("abc"));
    QEQUAL(edView->editor->inputModeLabel(), QString("NORMAL"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimExSubstituteCommands()
{
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("foo foo\nfoo foo\nbar", false);
    edView->editor->setCursorPosition(0, 0, false);

    QVERIFY(edView->executeVimExCommand("s/foo/X/"));
    QEQUAL(edView->editor->document()->text(), QString("X foo\nfoo foo\nbar"));

    QVERIFY(edView->executeVimExCommand("%s/foo/Y/g"));
    QEQUAL(edView->editor->document()->text(), QString("X Y\nY Y\nbar"));

    edView->editor->setText("foo foo\nfoo foo\nfoo foo", false);
    QVERIFY(edView->executeVimExCommand("1,2s/foo/Z/"));
    QEQUAL(edView->editor->document()->text(), QString("Z foo\nZ foo\nfoo foo"));

    edView->executeVimSearch("foo", false);
    edView->editor->setCursorPosition(2, 0, false);
    QVERIFY(edView->executeVimExCommand("s//Q/g"));
    QEQUAL(edView->editor->document()->text(), QString("Z foo\nZ foo\nQ Q"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

void LatexEditorViewTest::vimExCommands()
{
    const int oldMode = edView->getConfig()->editingMode;
    edView->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    edView->updateSettings();

    edView->editor->setText("alpha\nbeta\ngamma", false);
    edView->editor->setCursorPosition(0, 0, false);

    QVERIFY(edView->executeVimExCommand("2"));
    QEQUAL(edView->editor->cursor().lineNumber(), 1);

    QObject::disconnect(edView, SIGNAL(vimCommandRequested(QString)), nullptr, nullptr);
    QSignalSpy commandSpy(edView, SIGNAL(vimCommandRequested(QString)));
    QVERIFY(edView->executeVimExCommand(":w"));
    QEQUAL(commandSpy.count(), 1);
    QEQUAL(commandSpy.takeFirst().at(0).toString(), QString("w"));

    edView->executeVimSearch("beta", false);
    QEQUAL(edView->getSearchText(), QString("beta"));
    QVERIFY(edView->executeVimExCommand("noh"));

    edView->getConfig()->editingMode = oldMode;
    edView->updateSettings();
}

#endif

