/***************************************************************************
 *   copyright       : (C) 2008 by Benito van der Zander                   *
 *   http://www.xm1math.net/texmaker/                                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "latexeditorview.h"
#include "latexeditorview_config.h"

#include "filedialog.h"
#include "latexcompleter.h"
#include "latexdocument.h"
#include "smallUsefulFunctions.h"
#include "spellerutility.h"
#include "tablemanipulation.h"

#include "qdocumentline.h"
#include "qdocumentline_p.h"
#include "qdocumentcommand.h"

#include "qlinemarksinfocenter.h"
#include "qformatfactory.h"
#include "qlanguagedefinition.h"
#include "qnfadefinition.h"
#include "qnfa.h"

#include "qcodeedit.h"
#include "qeditor.h"
#include "qeditorinputbinding.h"
#include "qlinemarkpanel.h"
#include "qlinenumberpanel.h"
#include "qfoldpanel.h"
#include "qgotolinepanel.h"
#include "qlinechangepanel.h"
#include "qstatuspanel.h"
#include "qsearchreplacepanel.h"
#include "qdocumentsearch.h"
#include "latexrepository.h"

#include "latexparser/latexparsing.h"

#include "latexcompleter_config.h"

#include "scriptengine.h"
#include "diffoperations.h"

#include "help.h"

#include "bidiextender.h"

#include <functional>
#include <random>

//------------------------------Default Input Binding--------------------------------
/*!
 * \brief default keyboard binding for normal operation
 */
class LatexDefaultInputBinding: public QEditorInputBinding
{
	//  Q_OBJECT not possible because inputbinding is no qobject
public:
    LatexDefaultInputBinding(): completerConfig(nullptr), editorViewConfig(nullptr), contextMenu(nullptr), isDoubleClick(false) {}
	virtual QString id() const
	{
		return "TXS::DefaultInputBinding";
	}
	virtual QString name() const
	{
		return "TXS::DefaultInputBinding";
	}

	virtual bool keyPressEvent(QKeyEvent *event, QEditor *editor);
	virtual void postKeyPressEvent(QKeyEvent *event, QEditor *editor);
    virtual void postInputMethodEvent(QInputMethodEvent *event, QEditor *editor);
	virtual bool keyReleaseEvent(QKeyEvent *event, QEditor *editor);
	virtual bool mousePressEvent(QMouseEvent *event, QEditor *editor);
	virtual bool mouseReleaseEvent(QMouseEvent *event, QEditor *editor);
	virtual bool mouseDoubleClickEvent(QMouseEvent *event, QEditor *editor);
	virtual bool mouseMoveEvent(QMouseEvent *event, QEditor *editor);
	virtual bool contextMenuEvent(QContextMenuEvent *event, QEditor *editor);
private:
	bool runMacros(QKeyEvent *event, QEditor *editor);
	bool autoInsertLRM(QKeyEvent *event, QEditor *editor);
    bool tryStartCompletionForTrigger(const QString &text, QEditor *editor, bool allowInsertion);
    void maybeOpenSingleCharCompleter(const QString &text, QEditor *editor);
	void checkLinkOverlay(QPoint mousePos, Qt::KeyboardModifiers modifiers, QEditor *editor);
	friend class LatexEditorView;
	const LatexCompleterConfig *completerConfig;
	const LatexEditorViewConfig *editorViewConfig;

	QMenu *contextMenu;
	QString lastSpellCheckedWord;

	QPoint lastMousePressLeft;
	bool isDoubleClick;  // event sequence of a double click: press, release, double click, release - this is true on the second release
    Qt::KeyboardModifiers modifiersWhenPressed;

};

static const QString LRMStr = QChar(LRM);

bool LatexDefaultInputBinding::runMacros(QKeyEvent *event, QEditor *editor)
{
	Q_ASSERT(completerConfig);
    QLanguageDefinition *language = editor->document() ? editor->document()->languageDefinition() : nullptr;
	QDocumentLine line = editor->cursor().selectionStart().line();
	int column = editor->cursor().selectionStart().columnNumber();
    QString prev = line.text().mid(0, column) + event->text();
    if(event->text().isEmpty() && event->key()==Qt::Key_Tab){
        // workaround for #2866 (tab as trigger in macro on osx)
        prev+="\t";
    }
    const LatexDocument *doc = qobject_cast<LatexDocument *>(editor->document());
    StackEnvironment env;

	foreach (const Macro &m, completerConfig->userMacros) {
		if (m.checkState() != Qt::Checked) continue;
		if (!m.isActiveForTrigger(Macro::ST_REGEX)) continue;
		if (!m.isActiveForLanguage(language)) continue;
        if(m.hasFormatTriggers()){
            // check formats at column from overlays
            QList<int> formats=m.getFormatExcludeTriggers();
            if(!formats.isEmpty()){
                QFormatRange fr = line.getOverlayAt(column, formats);
                if(fr.isValid()){
                    continue;
                }
            }
            formats=m.getFormatTriggers();
            if(!formats.isEmpty()){
                QFormatRange fr = line.getOverlayAt(column, formats);
                if(!fr.isValid()){
                    continue;
                }
            }
        }
        QStringList envTriggers = m.getTriggerInEnvs();
        if(!envTriggers.isEmpty()){
            if(env.isEmpty()){
                doc->getEnv(editor->cursor().lineNumber(),env);
            }

            // use topEnv as trigger env
            const QStringList ignoreEnv = {"document","normal"};
            bool inMath=false;
            bool passed=false;
            if(!env.isEmpty() && !ignoreEnv.contains(env.top().name)){
                QString envName=env.top().name;
                QStringList envAliases = doc->lp->environmentAliases.values(envName);
                bool aliasFound=std::any_of(envAliases.cbegin(),envAliases.cend(),[&envTriggers](const QString &alias){
                    return envTriggers.contains(alias);
                });
                if(envTriggers.contains(envName)|| aliasFound){
                    passed=true;
                    if(envName=="math"){
                        // continued math mode from previous line
                        passed=false;
                        inMath=true;
                    }
                }
            }
            // special treatment for math env as that be be toggled with special symbols
            if(!passed && envTriggers.contains("math")){
                QVector<QParenthesis>parenthesis=line.parentheses();

                for(int i=0;i<parenthesis.size();++i){
                    QParenthesis &p=parenthesis[i];
                    if(p.id==61){
                        if(p.offset<column){
                            inMath=(p.role & QParenthesis::Open)>0;
                        }
                        if(p.offset>=column){
                            break;
                        }
                    }
                }
                if(!inMath){
                    continue; // skip further trigger checks
                }else{
                    passed=true;
                }
            }
            if(!passed)
                continue; // skip further trigger checks, no valid env found
        }
        const QRegularExpression &r = m.triggerRegex;
        QRegularExpressionMatch match=r.match(prev);
        if (match.hasMatch()) {
            // force last match which basically is right-most match, see #2448
            while(true){
                int offset=match.capturedStart()+1;
                QRegularExpressionMatch test=r.match(prev,offset);
                if(test.hasMatch()){
                    match=test;
                }else{
                    break;
                }
            }
			QDocumentCursor c = editor->cursor();
			bool block = false;
            int realMatchLen = match.capturedLength();
            //if (m.triggerLookBehind) realMatchLen -= match.captured(1).length();
			if (c.hasSelection() || realMatchLen > 1)
				block = true;
			if (block) editor->document()->beginMacro();
			if (c.hasSelection()) {
				editor->cutBuffer = c.selectedText();
				c.removeSelectedText();
			}
            if (match.capturedLength() > 1) {
				c.movePosition(realMatchLen - 1, QDocumentCursor::PreviousCharacter, QDocumentCursor::KeepAnchor);
				c.removeSelectedText();
                editor->setCursor(c);
			}

			LatexEditorView *view = editor->property("latexEditor").value<LatexEditorView *>();
			REQUIRE_RET(view, true);
            emit view->execMacro(m, MacroExecContext(Macro::ST_REGEX, match.capturedTexts()));
			if (block) editor->document()->endMacro();
			editor->cutBuffer.clear();
			editor->emitCursorPositionChanged(); //prevent rogue parenthesis highlightations
			/*			if (editor->languageDefinition())
			editor->languageDefinition()->clearMatches(editor->document());
			*/
			return true;
		}
	}
	return false;
}

bool LatexDefaultInputBinding::autoInsertLRM(QKeyEvent *event, QEditor *editor)
{
	const QString &text = event->text();
	if (editorViewConfig->autoInsertLRM && text.length() == 1 && editor->cursor().isRTL()) {
		if (text.at(0) == '}') {
			bool autoOverride = editor->isAutoOverrideText("}");
			bool previousIsLRM = editor->cursor().previousChar().unicode() == LRM;
			bool block = previousIsLRM || autoOverride;
			if (block) editor->document()->beginMacro();
			if (previousIsLRM) editor->cursor().deletePreviousChar(); //todo mirrors
			if (autoOverride) {
				editor->write("}"); //separated, so autooverride works
				editor->write(LRMStr);
			} else editor->write("}" + LRMStr);
			if (block) editor->document()->endMacro();
			return true;
		}
	}
	return false;
}

void LatexDefaultInputBinding::checkLinkOverlay(QPoint mousePos, Qt::KeyboardModifiers modifiers, QEditor *editor)
{
	if (modifiers == Qt::ControlModifier) {
		LatexEditorView *edView = qobject_cast<LatexEditorView *>(editor->parentWidget());
		QDocumentCursor cursor = editor->cursorForPosition(mousePos);
		edView->checkForLinkOverlay(cursor);
	} else {
		// reached for example when Ctrl+Shift is pressed
		LatexEditorView *edView = qobject_cast<LatexEditorView *>(editor->parentWidget()); //a qobject is necessary to retrieve events
		edView->removeLinkOverlay();
	}
}

bool LatexDefaultInputBinding::keyPressEvent(QKeyEvent *event, QEditor *editor)
{
    if (tryStartCompletionForTrigger(event->text(), editor, true)) {
		return true;
	}
    if (!event->text().isEmpty() || event->key()==Qt::Key_Tab) {
		if (!editor->flag(QEditor::Overwrite) && runMacros(event, editor))
			return true;
		if (autoInsertLRM(event, editor))
			return true;
	} else {
		if (event->key() == Qt::Key_Control) {
			editor->setMouseTracking(true);
			QPoint mousePos(editor->mapToFrame(editor->mapFromGlobal(QCursor::pos())));
			checkLinkOverlay(mousePos, event->modifiers(), editor);
		}
	}
	if (LatexEditorView::hideTooltipWhenLeavingLine != -1 && editor->cursor().lineNumber() != LatexEditorView::hideTooltipWhenLeavingLine) {
		LatexEditorView::hideTooltipWhenLeavingLine = -1;
		QToolTip::hideText();
	}
	return false;
}

void LatexDefaultInputBinding::postKeyPressEvent(QKeyEvent *event, QEditor *editor)
{
    maybeOpenSingleCharCompleter(event->text(), editor);
}

void LatexDefaultInputBinding::postInputMethodEvent(QInputMethodEvent *event, QEditor *editor)
{
    if (!event)
        return;

    const QString text = event->commitString();
    if (text.isEmpty())
        return;

    if (tryStartCompletionForTrigger(text, editor, false))
        return;

    maybeOpenSingleCharCompleter(text, editor);
}

bool LatexDefaultInputBinding::tryStartCompletionForTrigger(const QString &text, QEditor *editor, bool allowInsertion)
{
    if (!LatexEditorView::completer || !LatexEditorView::completer->acceptTriggerString(text)
            || (editor->currentPlaceHolder() >= 0 && editor->currentPlaceHolder() < editor->placeHolderCount()
                && !editor->getPlaceHolder(editor->currentPlaceHolder()).mirrors.isEmpty()
                && editor->getPlaceHolder(editor->currentPlaceHolder()).affector == BracketInvertAffector::instance())
            || editor->flag(QEditor::Overwrite)) {
        return false;
    }

    editor->emitNeedUpdatedCompleter();
    const bool autoOverriden = allowInsertion && editor->isAutoOverrideText(text);
    if (allowInsertion) {
        if (editorViewConfig->autoInsertLRM && text == "\\" && editor->cursor().isRTL())
            editor->write(LRMStr + text);
        else
            editor->write(text);
    }

    if (autoOverriden) {
        LatexEditorView::completer->complete(editor, LatexCompleter::CF_OVERRIDEN_BACKSLASH);
        return true;
    }

    EnumsTokenType::TokenType ctx = Parsing::getCompleterContext(editor->cursor().line().handle(), editor->cursor().columnNumber());
    if (ctx == EnumsTokenType::def)
        return true;

    const LatexDocument *doc = qobject_cast<LatexDocument *>(editor->document());
    StackEnvironment env;
    doc->getEnv(editor->cursor().lineNumber(), env);
    const QStringList ignoreEnv = {"document", "normal"};
    if (!env.isEmpty() && !ignoreEnv.contains(env.top().name)) {
        QString envName = env.top().name;
        QStringList envAliases = doc->lp->environmentAliases.values(envName);
        if (!envAliases.isEmpty())
            envName = envAliases.first();
        LatexEditorView::completer->setFilter(envName);
    }

    LatexCompleter::CompletionFlags flags = ctx == EnumsTokenType::width ? LatexCompleter::CF_FORCE_LENGTH : LatexCompleter::CompletionFlag(0);
    if (ctx >= Token::specialArg) {
        const int df = int(ctx - Token::specialArg);
        const QString cmd = LatexEditorView::completer->getLatexParser().mapSpecialArgs.value(df);
        LatexEditorView::completer->setWorkPath(cmd);
        flags = LatexCompleter::CF_FORCE_SPECIALOPTION;
    }
    LatexEditorView::completer->complete(editor, flags);
    return true;
}

void LatexDefaultInputBinding::maybeOpenSingleCharCompleter(const QString &text, QEditor *editor)
{
    if (text.length() != 1)
        return;

    const QChar c = text.at(0);
    if (c != ',' && !c.isLetter())
        return;

    LatexEditorView *view = editor->property("latexEditor").value<LatexEditorView *>();
    Q_ASSERT(view);
    if (completerConfig && completerConfig->enabled)
        view->mayNeedToOpenCompleter(c != ',');
}

bool LatexDefaultInputBinding::keyReleaseEvent(QKeyEvent *event, QEditor *editor)
{
	if (event->key() == Qt::Key_Control) {
		editor->setMouseTracking(false);
		LatexEditorView *edView = qobject_cast<LatexEditorView *>(editor->parentWidget()); //a qobject is necessary to retrieve events
		edView->removeLinkOverlay();
	}
	return false;
}

bool LatexDefaultInputBinding::mousePressEvent(QMouseEvent *event, QEditor *editor)
{
    LatexEditorView *edView = nullptr;

	switch (event->button()) {
	case Qt::XButton1:
		edView = qobject_cast<LatexEditorView *>(editor->parentWidget());
		emit edView->mouseBackPressed();
		return true;
	case Qt::XButton2:
		edView = qobject_cast<LatexEditorView *>(editor->parentWidget());
		emit edView->mouseForwardPressed();
		return true;
	case Qt::LeftButton:
		edView = qobject_cast<LatexEditorView *>(editor->parentWidget());
		emit edView->cursorChangeByMouse();
		lastMousePressLeft = event->pos();
        modifiersWhenPressed = event->modifiers();
		return false;
	default:
		return false;
	}
}

bool LatexDefaultInputBinding::mouseReleaseEvent(QMouseEvent *event, QEditor *editor)
{
	if (isDoubleClick) {
		isDoubleClick = false;
		return false;
	}
	isDoubleClick = false;

    if (event->modifiers() == Qt::ControlModifier && modifiersWhenPressed == event->modifiers() && event->button() == Qt::LeftButton) {
		// Ctrl+LeftClick
		int distanceSqr = (event->pos().x() - lastMousePressLeft.x()) * (event->pos().x() - lastMousePressLeft.x()) + (event->pos().y() - lastMousePressLeft.y()) * (event->pos().y() - lastMousePressLeft.y());
		if (distanceSqr > 4) // allow the user to accidentially move the mouse a bit
			return false;

		LatexEditorView *edView = qobject_cast<LatexEditorView *>(editor->parentWidget()); //a qobject is necessary to retrieve events
		if (!edView) return false;
		QDocumentCursor cursor = editor->cursorForPosition(editor->mapToContents(event->pos()));

		if (edView->hasLinkOverlay()) {
			LinkOverlay lo = edView->getLinkOverlay();
			switch (lo.type) {
			case LinkOverlay::RefOverlay:
				emit edView->gotoDefinition(cursor);
				return true;
			case LinkOverlay::FileOverlay:
                emit edView->openFile(lo.m_link.isEmpty() ? lo.text() : lo.m_link);
				return true;
			case LinkOverlay::UrlOverlay:
				if (!QDesktopServices::openUrl(lo.text())) {
					UtilsUi::txsWarning(LatexEditorView::tr("Could not open url:") + "\n" + lo.text());
				}
				return true;
			case LinkOverlay::UsepackageOverlay:
				edView->openPackageDocumentation(lo.text());
				return true;
			case LinkOverlay::BibFileOverlay:
                emit edView->openFile(lo.text(), "bib");
				return true;
			case LinkOverlay::CiteOverlay:
				emit edView->gotoDefinition(cursor);
				return true;
			case LinkOverlay::CommandOverlay:
				emit edView->gotoDefinition(cursor);
				return true;
			case LinkOverlay::EnvOverlay:
				emit edView->gotoDefinition(cursor);
				return true;
			case LinkOverlay::Invalid:
				break;
			}
		}

		if (!editor->languageDefinition()) return false;
		if (editor->languageDefinition()->language() != "(La)TeX")
			return false;
		emit edView->syncPDFRequested(cursor);
		return true;
	}
	return false;
}

bool LatexDefaultInputBinding::mouseDoubleClickEvent(QMouseEvent *event, QEditor *editor)
{
	Q_UNUSED(event)
	Q_UNUSED(editor)
	isDoubleClick = true;
	return false;
}

bool LatexDefaultInputBinding::contextMenuEvent(QContextMenuEvent *event, QEditor *editor)
{
    if (!contextMenu) contextMenu = new QMenu(nullptr);
	contextMenu->clear();
	contextMenu->setProperty("isSpellingPopulated", QVariant());  // delete information on spelling
	QDocumentCursor cursor;
	if (event->reason() == QContextMenuEvent::Mouse) cursor = editor->cursorForPosition(editor->mapToContents(event->pos()));
	else cursor = editor->cursor();
	LatexEditorView *edView = qobject_cast<LatexEditorView *>(editor->parentWidget()); //a qobject is necessary to retrieve events
	REQUIRE_RET(edView, false);

	// check for context menu on preview picture
	QRect pictRect = cursor.line().getCookie(QDocumentLine::PICTURE_COOKIE_DRAWING_POS).toRect();
	if (pictRect.isValid()) {
		QPoint posInDocCoordinates(event->pos().x() + edView->editor->horizontalOffset(), event->pos().y() + edView->editor->verticalOffset());
		if (pictRect.contains(posInDocCoordinates)) {
			// get removePreviewAction
			// ok, this is not an ideal way of doing it because (i) it has to be in the baseActions (at least we Q_ASSERT this) and (ii) the iteration over the baseActions
			// Alternatives: 1) include configmanager and use ConfigManager->getInstance()->getManagedAction() - Disadvantage: additional dependency
			//               2) explicitly pass it to the editorView (like the base actions, but separate) - Disadvantage: code overhead
			//               3) Improve the concept of base actions:
			//                    LatexEditorView::addContextAction(QAction); called when creating the editorView
			//                    LatexEditorView::getContextAction(QString); used here to populate the menu
			bool removePreviewActionFound = false;
			foreach (QAction *act, LatexEditorView::s_baseActions) {
				if (act->objectName().endsWith("removePreviewLatex")) {
                    // inline preview context menu supplies the calling point in doc coordinates as data
                    LatexEditorView::s_contextMenuRow = editor->document()->indexOf(editor->lineAtPosition(posInDocCoordinates));
                    // slight performance penalty for use of lineNumber(), which is not stictly necessary because
                    // we convert it back to a QDocumentLine, but easier to handle together with the other cases
					contextMenu->addAction(act);
					removePreviewActionFound = true;
					break;
				}
			}
			Q_ASSERT(removePreviewActionFound);


			QVariant vPixmap = cursor.line().getCookie(QDocumentLine::PICTURE_COOKIE);
			if (vPixmap.isValid()) {
                (contextMenu->addAction(LatexEditorView::tr("Copy Image"), edView, SLOT(copyImageFromAction())))->setData(vPixmap);
                (contextMenu->addAction(LatexEditorView::tr("Save Image As..."), edView, SLOT(saveImageFromAction())))->setData(vPixmap);
			}
			contextMenu->exec(event->globalPos());

            // reset context menu position
            LatexEditorView::s_contextMenuRow = -1;
            LatexEditorView::s_contextMenuCol = -1;

			return true;
		}
	}

	// normal context menu
	bool validPosition = cursor.isValid() && cursor.line().isValid();
	//LatexParser::ContextType context = LatexParser::Unknown;
	QString ctxCommand;
	if (validPosition) {
		QFormatRange fr;
		//spell checking

		if (edView->speller) {
			int pos;
			if (cursor.hasSelection()) pos = (cursor.columnNumber() + cursor.anchorColumnNumber()) / 2;
			else pos = cursor.columnNumber();

			foreach (const int f, edView->grammarFormats) {
				fr = cursor.line().getOverlayAt(pos, f);
				if (fr.length > 0 && fr.format == f) {
					QVariant var = cursor.line().getCookie(QDocumentLine::GRAMMAR_ERROR_COOKIE);
					if (var.isValid()) {
                        edView->wordSelection=QDocumentCursor(editor->document(), cursor.lineNumber(), fr.offset);
                        edView->wordSelection.movePosition(fr.length, QDocumentCursor::NextCharacter, QDocumentCursor::KeepAnchor);
                        //editor->setCursor(wordSelection);

						const QList<GrammarError> &errors = var.value<QList<GrammarError> >();
						for (int i = 0; i < errors.size(); i++)
							if (errors[i].offset <= cursor.columnNumber() && errors[i].offset + errors[i].length >= cursor.columnNumber()) {
								edView->addReplaceActions(contextMenu, errors[i].corrections, true);
								break;
							}
					}
				}
			}

			fr = cursor.line().getOverlayAt(pos, SpellerUtility::spellcheckErrorFormat);
			if (fr.length > 0 && fr.format == SpellerUtility::spellcheckErrorFormat) {
				QString word = cursor.line().text().mid(fr.offset, fr.length);
				if (!(editor->cursor().hasSelection() && editor->cursor().selectedText().length() > 0) || editor->cursor().selectedText() == word
				        || editor->cursor().selectedText() == lastSpellCheckedWord) {
					lastSpellCheckedWord = word;
					word = latexToPlainWord(word);
                    edView->wordSelection=QDocumentCursor(editor->document(), cursor.lineNumber(), fr.offset);
                    edView->wordSelection.movePosition(fr.length, QDocumentCursor::NextCharacter, QDocumentCursor::KeepAnchor);
                    //editor->setCursor(wordSelection);

					if ((editorViewConfig->contextMenuSpellcheckingEntryLocation == 0) ^ (event->modifiers() & editorViewConfig->contextMenuKeyboardModifiers)) {
						edView->addSpellingActions(contextMenu, lastSpellCheckedWord, false);
						contextMenu->addSeparator();
					} else {
						QMenu *spellingMenu = contextMenu->addMenu(LatexEditorView::tr("Spelling"));
						spellingMenu->setProperty("word", lastSpellCheckedWord);
						edView->connect(spellingMenu, SIGNAL(aboutToShow()), edView, SLOT(populateSpellingMenu()));
					}
				}
			}
		}
		//citation checking
		int f = edView->citationMissingFormat;
		if (cursor.hasSelection()) fr = cursor.line().getOverlayAt((cursor.columnNumber() + cursor.anchorColumnNumber()) / 2, f);
		else fr = cursor.line().getOverlayAt(cursor.columnNumber(), f);
		if (fr.length > 0 && fr.format == f) {
			QString word = cursor.line().text().mid(fr.offset, fr.length);
            //editor->setCursor(editor->document()->cursor(cursor.lineNumber(), fr.offset, cursor.lineNumber(), fr.offset + fr.length)); // no need to select word as it is not changed anyway, see also #1034
			QAction *act = new QAction(LatexEditorView::tr("New BibTeX Entry %1").arg(word), contextMenu);
			edView->connect(act, SIGNAL(triggered()), edView, SLOT(requestCitation()));
			contextMenu->addAction(act);
			contextMenu->addSeparator();
		}
		//check input/include
		//find context of cursor
		QDocumentLineHandle *dlh = cursor.line().handle();
		TokenList tl = dlh->getCookieLocked(QDocumentLine::LEXER_COOKIE).value<TokenList>();
		int i = Parsing::getTokenAtCol(tl, cursor.columnNumber());
		Token tk;
		if (i >= 0)
			tk = tl.at(i);

		if (tk.type == Token::file) {
            Token cmdTk=Parsing::getCommandTokenFromToken(tl,tk);
            QString fn=tk.getText();
            if(cmdTk.dlh && cmdTk.getText()=="\\subimport"){
                int i=tl.indexOf(cmdTk);
                TokenList tl2=tl.mid(i); // in case of several cmds in one line
                QString path=Parsing::getArg(tl,Token::definition);
                if(!path.endsWith("/")){
                    path+="/";
                }
                fn=path+fn;
            }
			QAction *act = new QAction(LatexEditorView::tr("Open %1").arg(tk.getText()), contextMenu);
            act->setData(fn);
			edView->connect(act, SIGNAL(triggered()), edView, SLOT(openExternalFile()));
			contextMenu->addAction(act);
		}
		// bibliography command
		if (tk.type == Token::bibfile) {
			QAction *act = new QAction(LatexEditorView::tr("Open Bibliography"), contextMenu);
			QString bibFile;
			bibFile = tk.getText() + ".bib";
			act->setData(bibFile);
			edView->connect(act, SIGNAL(triggered()), edView, SLOT(openExternalFile()));
			contextMenu->addAction(act);
		}
		//package help
		if (tk.type == Token::package || tk.type == Token::documentclass) {
			QAction *act = new QAction(LatexEditorView::tr("Open package documentation"), contextMenu);
			QString packageName = tk.getText();
			act->setText(act->text().append(QString(" (%1)").arg(packageName)));
			act->setData(packageName);
			edView->connect(act, SIGNAL(triggered()), edView, SLOT(openPackageDocumentation()));
			contextMenu->addAction(act);
		}
		// help for any "known" command
		if (tk.type == Token::command) {
			ctxCommand = tk.getText();
			QString command = ctxCommand;
			if (ctxCommand == "\\begin" || ctxCommand == "\\end")
				command = ctxCommand + "{" + Parsing::getArg(tl.mid(i + 1), dlh, 0, ArgumentList::Mandatory) + "}";
			QString package = edView->document->parent->findPackageByCommand(command);
			package.chop(4);
			if (!package.isEmpty()) {
				QAction *act = new QAction(LatexEditorView::tr("Open package documentation"), contextMenu);
				act->setText(act->text().append(QString(" (%1)").arg(package)));
				act->setData(package + "#" + command);
				edView->connect(act, SIGNAL(triggered()), edView, SLOT(openPackageDocumentation()));
				contextMenu->addAction(act);
			}
		}
		// help for "known" environments
		if (tk.type == Token::beginEnv || tk.type == Token::env) {
			QString command = "\\begin{" + tk.getText() + "}";
			QString package = edView->document->parent->findPackageByCommand(command);
			package.chop(4);
			if (!package.isEmpty()) {
				QAction *act = new QAction(LatexEditorView::tr("Open package documentation"), contextMenu);
				act->setText(act->text().append(QString(" (%1)").arg(package)));
				act->setData(package + "#" + command);
				edView->connect(act, SIGNAL(triggered()), edView, SLOT(openPackageDocumentation()));
				contextMenu->addAction(act);
			}
		}
        if (/* tk.type==Tokens::bibRef || TODO: bibliography references not yet handled by token system */tk.type >= Token::specialArg || tk.type == Token::labelRef) {
			QAction *act = new QAction(LatexEditorView::tr("Go to Definition"), contextMenu);
			act->setData(QVariant().fromValue<QDocumentCursor>(cursor));
			edView->connect(act, SIGNAL(triggered()), edView, SLOT(emitGotoDefinitionFromAction()));
			contextMenu->addAction(act);
		}
		if (tk.type == Token::label || tk.type == Token::labelRef || tk.type == Token::labelRefList) {
			QAction *act = new QAction(LatexEditorView::tr("Find Usages"), contextMenu);
			act->setData(tk.getText());
			act->setProperty("doc", QVariant::fromValue<LatexDocument *>(edView->document));
			edView->connect(act, SIGNAL(triggered()), edView, SLOT(emitFindLabelUsagesFromAction()));
			contextMenu->addAction(act);
		}
        if (tk.type >= Token::specialArg) {
            // finnd usage
            QAction *act = new QAction(LatexEditorView::tr("Find Usages"), contextMenu);
            act->setData(tk.getText());
            act->setProperty("doc", QVariant::fromValue<LatexDocument *>(edView->document));
            act->setProperty("type", tk.type);
            edView->connect(act, SIGNAL(triggered()), edView, SLOT(emitFindSpecialUsagesFromAction()));
            contextMenu->addAction(act);
        }
        if (tk.type == Token::defSpecialArg) {
            LatexDocument *doc=edView->document;
            QString def=doc->getCmdfromSpecialArgToken(tk);
            QStringList vals=doc->lp->mapSpecialArgs.values();
            int k=vals.indexOf(def);
            if(k>-1){
                QAction *act = new QAction(LatexEditorView::tr("Find Usages"), contextMenu);
                act->setData(tk.getText());
                act->setProperty("doc", QVariant::fromValue<LatexDocument *>(edView->document));
                act->setProperty("type", Token::specialArg+k);
                edView->connect(act, SIGNAL(triggered()), edView, SLOT(emitFindSpecialUsagesFromAction()));
                contextMenu->addAction(act);
            }
        }
		if (tk.type == Token::word) {
			QAction *act = new QAction(LatexEditorView::tr("Thesaurus..."), contextMenu);
			act->setData(QPoint(cursor.anchorLineNumber(), cursor.anchorColumnNumber()));
			edView->connect(act, SIGNAL(triggered()), edView, SLOT(triggeredThesaurus()));
			contextMenu->addAction(act);
		}

		//resolve differences
		if (edView) {
			QList<int> fids;
			fids << edView->deleteFormat << edView->insertFormat << edView->replaceFormat;
			foreach (int fid, fids) {
				if (cursor.hasSelection()) fr = cursor.line().getOverlayAt((cursor.columnNumber() + cursor.anchorColumnNumber()) / 2, fid);
				else fr = cursor.line().getOverlayAt(cursor.columnNumber(), fid);
				if (fr.length > 0 ) {
					QVariant var = cursor.line().getCookie(QDocumentLine::DIFF_LIST_COOCKIE);
					if (var.isValid()) {
						DiffList diffList = var.value<DiffList>();
						//QString word=cursor.line().text().mid(fr.offset,fr.length);
						DiffOp op;
						op.start = -1;
						foreach (op, diffList) {
							if (op.start <= cursor.columnNumber() && op.start + op.length >= cursor.columnNumber()) {
								break;
							}
							op.start = -1;
						}
						if (op.start >= 0) {
							QAction *act = new QAction(LatexEditorView::tr("use yours"), contextMenu);
							act->setData(QPoint(cursor.lineNumber(), cursor.columnNumber()));
							edView->connect(act, SIGNAL(triggered()), edView, SLOT(emitChangeDiff()));
							contextMenu->addAction(act);
							act = new QAction(LatexEditorView::tr("use other's"), contextMenu);
							act->setData(QPoint(-cursor.lineNumber() - 1, cursor.columnNumber()));
							edView->connect(act, SIGNAL(triggered()), edView, SLOT(emitChangeDiff()));
							contextMenu->addAction(act);
							break;
						}
					}
				}
			}
		}
		contextMenu->addSeparator();
	}
	contextMenu->addActions(LatexEditorView::s_baseActions);
    // set context menu position
    LatexEditorView::s_contextMenuRow = cursor.anchorLineNumber();
    LatexEditorView::s_contextMenuCol = cursor.anchorColumnNumber();

	if (validPosition) {
		contextMenu->addSeparator();

		QAction *act = new QAction(LatexEditorView::tr("Go to PDF"), contextMenu);
		act->setData(QVariant().fromValue<QDocumentCursor>(cursor));
		edView->connect(act, SIGNAL(triggered()), edView, SLOT(emitSyncPDFFromAction()));
		contextMenu->addAction(act);
	}


	if (event->reason() == QContextMenuEvent::Mouse) contextMenu->exec(event->globalPos());
	else {
        QPointF curPoint = editor->cursor().documentPosition();
		curPoint.ry() += editor->document()->getLineSpacing();
        contextMenu->exec(editor->mapToGlobal(editor->mapFromContents(curPoint.toPoint())));
	}
    // reset position of context menu
    LatexEditorView::s_contextMenuRow = -1;
    LatexEditorView::s_contextMenuCol = -1;


	event->accept();

	return true;
}

bool LatexDefaultInputBinding::mouseMoveEvent(QMouseEvent *event, QEditor *editor)
{
	checkLinkOverlay(editor->mapToContents(event->pos()), event->modifiers(), editor);
	return false;
}

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

enum class VimRegisterType {
    CharacterWise,
    LineWise,
    BlockWise
};

struct VimRegister {
    VimRegisterType type = VimRegisterType::CharacterWise;
    QString text;
    QStringList blocks;
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

struct VimSubstituteCommand {
    int startLine = 0;
    int endLine = 0;
    QString pattern;
    QString replacement;
    bool global = false;
    bool confirm = false;
    bool caseSensitive = true;
};

enum class VimSubstituteParseResult {
    NotSubstitute,
    Parsed,
    Invalid
};

static VimRegister g_vimRegister;
}

class VimPromptPanel : public QPanel
{
public:
    enum PromptKind {
        NoPrompt,
        CommandPrompt,
        SearchForwardPrompt,
        SearchBackwardPrompt
    };

    Q_PANEL(VimPromptPanel, "Vim Prompt Panel")

    explicit VimPromptPanel(QWidget *parent = nullptr)
        : QPanel(parent), m_view(qobject_cast<LatexEditorView *>(parent)), m_promptLabel(new QLabel(this)), m_lineEdit(new QLineEdit(this)),
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

        const QPointer<VimPromptPanel> guard(this);
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

class VimInputBinding : public QEditorInputBindingInterface
{
public:
    explicit VimInputBinding(LatexEditorView *view, LatexDefaultInputBinding *defaultBinding)
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
            if (m_pendingFind != VimFindKind::None || m_pendingMarkAction != VimPendingMarkAction::None || m_count > 0 || m_lastG || m_pendingReplace || m_waitingForTextObject || m_pendingOperator != VimOperator::None) {
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
            g_vimRegister.type = VimRegisterType::LineWise;
            g_vimRegister.blocks.clear();
            g_vimRegister.text = lineRangeText(editor->document(), firstLine, lastLine, true);

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
            g_vimRegister.type = VimRegisterType::LineWise;
            g_vimRegister.blocks.clear();
            g_vimRegister.text = lineRangeText(editor->document(), firstLine, lastLine, true);
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
            if (m_repeatAction)
                m_repeatAction();
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
        clearPending(editor);
        setMode(VimMode::Normal, editor);
    }

    void executeOperatorMotion(QEditor *editor, const VimMotion &motion)
    {
        QDocumentCursor cursor = editor->cursor();
        QDocumentCursor target(cursor);
        moveCursorByMotion(target, motion);
        applyOperatorOnRange(editor, cursor, target, false);
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

    void applyOperatorOnRange(QEditor *editor, const QDocumentCursor &from, const QDocumentCursor &to, bool alreadySelected)
    {
        QDocumentCursor start(from);
        QDocumentCursor end(to);
        if (!alreadySelected)
            QDocumentCursor::sort(start, end);
        if (!alreadySelected && end.isValid() && !end.atEnd())
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
            setRegisterFromSelection(selection, VimRegisterType::CharacterWise);
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

    void setRegisterFromSelection(const QDocumentCursor &selection, VimRegisterType type)
    {
        g_vimRegister.type = type;
        g_vimRegister.text = selection.selectedText();
        g_vimRegister.blocks.clear();
    }

    void yankWholeLines(QEditor *editor, int count)
    {
        g_vimRegister.type = VimRegisterType::LineWise;
        g_vimRegister.blocks.clear();
        g_vimRegister.text = lineRangeText(editor->document(), editor->cursor().lineNumber(), editor->cursor().lineNumber() + count - 1, true);
        setMode(VimMode::Normal, editor);
    }

    void deleteWholeLines(QEditor *editor, int count, bool enterInsert)
    {
        const int firstLine = editor->cursor().lineNumber();
        const int lastLine = qMin(editor->document()->lineCount() - 1, firstLine + count - 1);
        const bool deletingAtDocumentEnd = lastLine + 1 >= editor->document()->lineCount();
        g_vimRegister.type = VimRegisterType::LineWise;
        g_vimRegister.blocks.clear();
        g_vimRegister.text = lineRangeText(editor->document(), firstLine, lastLine, true);
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
        }
    }

    void deleteCharacters(QEditor *editor, int count, bool backwards)
    {
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
        m_repeatAction = [this, count, backwards]() {
            deleteCharacters(m_view->editor, count, backwards);
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
        setRegisterFromSelection(cursor, VimRegisterType::CharacterWise);
        cursor.replaceSelectedText(QString(count, ch));
        editor->setCursor(cursor);
        setMode(VimMode::Normal, editor);
        normalizeNormalCursor(editor);
        m_repeatAction = [this, count, ch]() {
            replaceCharacters(m_view->editor, count, ch);
        };
    }

    void deleteToLineEnd(QEditor *editor)
    {
        QDocumentCursor cursor = editor->cursor();
        cursor.movePosition(1, QDocumentCursor::EndOfLine, QDocumentCursor::KeepAnchor);
        setRegisterFromSelection(cursor, VimRegisterType::CharacterWise);
        cursor.removeSelectedText();
        editor->setCursor(cursor);
        normalizeNormalCursor(editor);
        m_repeatAction = [this]() { deleteToLineEnd(m_view->editor); };
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
            g_vimRegister.type = VimRegisterType::BlockWise;
            g_vimRegister.blocks.clear();
            for (const QDocumentCursor &cursor : editor->cursors())
                g_vimRegister.blocks << cursor.selectedText();
            g_vimRegister.text = g_vimRegister.blocks.join(QStringLiteral("\n"));
        } else if (m_mode == VimMode::VisualLine) {
            g_vimRegister.type = VimRegisterType::LineWise;
            g_vimRegister.blocks.clear();
            g_vimRegister.text = lineRangeText(editor->document(), editor->cursor().startLineNumber(), editor->cursor().endLineNumber(), true);
        } else {
            setRegisterFromSelection(editor->cursor(), VimRegisterType::CharacterWise);
        }
        leaveVisualMode(editor, true);
    }

    void deleteVisualSelection(QEditor *editor, bool enterInsert)
    {
        if (m_mode == VimMode::VisualBlock) {
            g_vimRegister.type = VimRegisterType::BlockWise;
            g_vimRegister.blocks.clear();
            for (const QDocumentCursor &cursor : editor->cursors())
                g_vimRegister.blocks << cursor.selectedText();
            g_vimRegister.text = g_vimRegister.blocks.join(QStringLiteral("\n"));
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
            g_vimRegister.type = VimRegisterType::LineWise;
            g_vimRegister.blocks.clear();
            g_vimRegister.text = lineRangeText(editor->document(), firstLine, lastLine, true);

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
        deleteVisualSelection(editor, false);
        putRegister(editor, false);
    }

    void putRegister(QEditor *editor, bool after)
    {
        QDocumentCursor cursor = editor->cursor();
        if (g_vimRegister.type == VimRegisterType::LineWise) {
            const int lineCountBefore = editor->document()->lineCount();
            int targetLine = cursor.lineNumber() + (after ? 1 : 0);
            targetLine = qMax(0, targetLine);
            QDocumentCursor target(editor->document(), qMin(targetLine, qMax(0, editor->document()->lineCount() - 1)), 0);
            int insertedStartLine = qMin(targetLine, qMax(0, lineCountBefore - 1));
            if (targetLine >= editor->document()->lineCount()) {
                target.movePosition(1, QDocumentCursor::EndOfLine);
                QString text = g_vimRegister.text;
                const bool documentIsEmpty = editor->document()->lineCount() == 1 && editor->document()->line(0).length() == 0;
                insertedStartLine = documentIsEmpty ? 0 : lineCountBefore;
                if (!documentIsEmpty)
                    text.prepend(QLatin1Char('\n'));
                editor->insertText(target, text);
            } else {
                editor->insertText(target, g_vimRegister.text);
            }
            editor->setCursor(QDocumentCursor(editor->document(), insertedStartLine, 0));
            moveToLineStartText(editor);
        } else if (g_vimRegister.type == VimRegisterType::BlockWise) {
            const int baseLine = cursor.lineNumber();
            const int column = cursor.columnNumber() + (after ? 1 : 0);
            for (int i = 0; i < g_vimRegister.blocks.size(); ++i) {
                const int line = qMin(baseLine + i, editor->document()->lineCount() - 1);
                QDocumentCursor block(editor->document(), line, qMin(column, editor->document()->line(line).length()));
                block.insertText(g_vimRegister.blocks.at(i));
            }
        } else {
            if (after && !cursor.atLineEnd())
                cursor.movePosition(1, QDocumentCursor::NextCharacter);
            editor->insertText(cursor, g_vimRegister.text);
            editor->setCursor(cursor);
        }
        setMode(VimMode::Normal, editor);
        normalizeNormalCursor(editor);
        m_repeatAction = [this, after]() { putRegister(m_view->editor, after); };
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
    LatexDefaultInputBinding *m_defaultBinding;
    VimMode m_mode;
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

void VimPromptPanel::closePrompt()
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

//----------------------------------LatexEditorView-----------------------------------
LatexCompleter *LatexEditorView::completer = nullptr;
int LatexEditorView::hideTooltipWhenLeavingLine = -1;
QList<QAction *> LatexEditorView::s_baseActions;
int LatexEditorView::s_contextMenuRow = -1;
int LatexEditorView::s_contextMenuCol = -1;

//Q_DECLARE_METATYPE(LatexEditorView *)

LatexEditorView::LatexEditorView(QWidget *parent, LatexEditorViewConfig *aconfig, LatexDocument *doc)
    : QWidget(parent),
      document(nullptr),
      lineNumberPanelAction(nullptr),
      lineMarkPanelAction(nullptr),
      lineFoldPanelAction(nullptr),
      lineChangePanelAction(nullptr),
      statusPanelAction(nullptr),
      searchReplacePanelAction(nullptr),
      gotoLinePanelAction(nullptr),
      vimPromptPanelAction(nullptr),
      lineMarkPanel(nullptr),
      lineNumberPanel(nullptr),
      searchReplacePanel(nullptr),
      gotoLinePanel(nullptr),
      statusPanel(nullptr),
      vimPromptPanel(nullptr),
      m_point(),
      wordSelection(),
      latexPackageList(nullptr),
      spellerManager(nullptr),
      speller(nullptr),
      useDefaultSpeller(true),
      curChangePos(-1),
      defaultInputBinding(nullptr),
      vimInputBinding(nullptr),
      config(aconfig),
      bibReader(nullptr),
      help(nullptr)
{
	Q_ASSERT(config);

	QVBoxLayout *mainlay = new QVBoxLayout(this);
	mainlay->setSpacing(0);
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
    mainlay->setMargin(0);
#else
    mainlay->setContentsMargins(0,0,0,0);
#endif

	codeeditor = new QCodeEdit(false, this, doc);
	editor = codeeditor->editor();
    document = doc;

	editor->setProperty("latexEditor", QVariant::fromValue<LatexEditorView *>(this));

	lineMarkPanel = new QLineMarkPanel;
    lineMarkPanel->setCursor(Qt::PointingHandCursor);
	lineMarkPanelAction = codeeditor->addPanel(lineMarkPanel, QCodeEdit::West, false);
	lineNumberPanel = new QLineNumberPanel;
    lineNumberPanelAction = codeeditor->addPanel(lineNumberPanel, QCodeEdit::West, false);
	QFoldPanel *foldPanel = new QFoldPanel;
	lineFoldPanelAction = codeeditor->addPanel(foldPanel, QCodeEdit::West, false);
	lineChangePanelAction = codeeditor->addPanel(new QLineChangePanel, QCodeEdit::West, false);

	statusPanel = new QStatusPanel;
	statusPanel->setFont(QApplication::font());
	statusPanelAction = codeeditor->addPanel(statusPanel, QCodeEdit::South, false);

	gotoLinePanel = new QGotoLinePanel;
	gotoLinePanel->setFont(QApplication::font());
	gotoLinePanelAction = codeeditor->addPanel(gotoLinePanel, QCodeEdit::South, false);

    searchReplacePanel = new QSearchReplacePanel;
	searchReplacePanel->setFont(QApplication::font());
	searchReplacePanelAction = codeeditor->addPanel(searchReplacePanel, QCodeEdit::South, false);
    searchReplacePanel->hide();
    connect(searchReplacePanel, SIGNAL(showExtendedSearch()), this, SIGNAL(showExtendedSearch()));

    vimPromptPanel = new VimPromptPanel(this);
    vimPromptPanel->setFont(QApplication::font());
    vimPromptPanelAction = codeeditor->addPanel(vimPromptPanel, QCodeEdit::South, false);
    vimPromptPanel->hide();

	connect(lineMarkPanel, SIGNAL(lineClicked(int)), this, SLOT(lineMarkClicked(int)));
    connect(lineMarkPanel, SIGNAL(toolTipRequested(int,int)), this, SLOT(lineMarkToolTip(int,int)));
    connect(lineMarkPanel, SIGNAL(contextMenuRequested(int,QPoint)), this, SLOT(lineMarkContextMenuRequested(int,QPoint)));
    connect(foldPanel, SIGNAL(contextMenuRequested(int,QPoint)), this, SLOT(foldContextMenuRequested(int,QPoint)));
	connect(editor, SIGNAL(hovered(QPoint)), this, SLOT(mouseHovered(QPoint)));
	//connect(editor->document(),SIGNAL(contentsChange(int, int)),this,SLOT(documentContentChanged(int, int)));
    connect(editor->document(), SIGNAL(lineDeleted(QDocumentLineHandle*,int)), this, SLOT(lineDeleted(QDocumentLineHandle*,int)));

	connect(doc, SIGNAL(spellingDictChanged(QString)), this, SLOT(changeSpellingDict(QString)));
    connect(doc, SIGNAL(bookmarkRemoved(QDocumentLineHandle*)), this, SIGNAL(bookmarkRemoved(QDocumentLineHandle*)));
    connect(doc, SIGNAL(bookmarkAdded(QDocumentLineHandle*,int)), this, SIGNAL(bookmarkAdded(QDocumentLineHandle*,int)));

	//editor->setFlag(QEditor::CursorJumpPastWrap,false);
	editor->disableAccentHack(config->hackDisableAccentWorkaround);

    defaultInputBinding = new LatexDefaultInputBinding();
    vimInputBinding = new VimInputBinding(this, defaultInputBinding);
	defaultInputBinding->completerConfig = completer->getConfig();
	defaultInputBinding->editorViewConfig = config;
	Q_ASSERT(defaultInputBinding->completerConfig);
	editor->document()->setLineEndingDirect(QDocument::Local);
    rebindInputMode();
	mainlay->addWidget(editor);

	setFocusProxy(editor);

	//containedLabels.setPattern("(\\\\label)\\{(.+)\\}");
	//containedReferences.setPattern("(\\\\ref|\\\\pageref)\\{(.+)\\}");
	updateSettings();

	lp = LatexParser::getInstance();
}

LatexEditorView::~LatexEditorView()
{
	delete searchReplacePanel; // to force deletion of m_search before document. Otherwise crashes can come up (linux)
	delete codeeditor; //explicit call destructor of codeeditor (although it has a parent, it is no qobject itself, but passed it to editor)
    delete vimInputBinding;
    delete defaultInputBinding;

	if (bibReader) {
		bibReader->quit();
		bibReader->wait();
	}
}

void LatexEditorView::updateReplacementList(const QSharedPointer<LatexParser> cmds, bool forceUpdate)
{
	QMap<QString, QString> replacementList;
	bool differenceExists = false;
    foreach (QString elem, cmds->possibleCommands["%replace"]) {
		int i = elem.indexOf(" ");
		if (i > 0) {
			replacementList.insert(elem.left(i), elem.mid(i + 1));
			if (mReplacementList.value(elem.left(i)) != elem.mid(i + 1))
				differenceExists = true;
		}
	}
	if (differenceExists || replacementList.count() != mReplacementList.count() || forceUpdate) {
		mReplacementList = replacementList;
        document->setReplacementList(mReplacementList);
        reCheckSyntax(0); //force complete spellcheck
    }
}

/*!
 * \brief Helper class to update the palete of editor/codeeditor
 * QCodeeditor does not use inheritance from a widget, so any palette automatism is disabled
 * To counteract this deficiency, codeeditors children (panels) are updated explicitely
 *
 * It would probably be better to adapt qcodeeditor to an inheritance based model, but that may take effort and time
 * \param pal new palette
 */
void LatexEditorView::updatePalette(const QPalette &pal)
{
    editor->setPalette(pal);
    for(QPanel *p:codeeditor->panels()){
        p->setPalette(pal);
    }
    editor->horizontalScrollBar()->setPalette(pal);
    editor->verticalScrollBar()->setPalette(pal);
}
/*!
 * \brief force an redraw/update on all sidepanels like numberlines, foldlines, etc.
 */
void LatexEditorView::updatePanels()
{
    for(QPanel *p:codeeditor->panels()){
        p->update();
    }
}

void LatexEditorView::paste()
{
	if (completer->isVisible()) {
		const QMimeData *d = QApplication::clipboard()->mimeData();

		if ( d ) {
			QString txt;
			if ( d->hasFormat("text/plain") )
				txt = d->text();
			else if ( d->hasFormat("text/html") )
				txt = d->html();

			if (txt.contains("\n"))
				txt.clear();

			if (txt.isEmpty()) {
				completer->close();
				editor->paste();
			} else {
				completer->insertText(txt);
			}
		}
	} else {
		editor->paste();
	}
}

void LatexEditorView::insertSnippet(QString text)
{
	CodeSnippet(text).insert(editor);
}

void LatexEditorView::deleteLines(bool toStart, bool toEnd)
{
	QList<QDocumentCursor> cursors = editor->cursors();
	if (cursors.empty()) return;
	document->beginMacro();
	for (int i=0;i<cursors.size();i++)
		cursors[i].removeSelectedText();

	int cursorLine = cursors[0].lineNumber();
	QMultiMap< int, QDocumentCursor* > map = getSelectedLines(cursors);
	QList<int> lines = map.uniqueKeys();
	QList<QDocumentCursor> newMirrors;
	for (int i=lines.size()-1;i>=0;i--) {
		QList<QDocumentCursor*> cursors = map.values(lines[i]);
		REQUIRE(cursors.size());
		if (toStart && toEnd) cursors[0]->eraseLine();
		else {
			int len = document->line(lines[i]).length();
			int column = toStart ? 0 : len;
			foreach (QDocumentCursor* c, cursors)
				if (toStart) column = qMax(c->columnNumber(), column);
				else column = qMin(c->columnNumber(), column);
			QDocumentCursor c = document->cursor(lines[i], column, lines[i], toStart ? 0 : len);
			c.removeSelectedText();

			if (!toStart || !toEnd){
				if (lines[i] == cursorLine) editor->setCursor(c);
				else newMirrors << c;
			}
		}
	}
	document->endMacro();
    editor->setCursor(cursors[0]);
	if (!toStart || !toEnd)
		for (int i=0;i<newMirrors.size();i++)
            editor->addCursorMirror(newMirrors[i]); //one cursor / line
}
/*!
 * \brief cut lines
 * Cut lines are copied into clipboard. To do so sensibly, cursors are sorted by line number first.
 */
void LatexEditorView::cutLines()
{
    QDocumentCursor cur = editor->cursor();
    if(cur.hasSelection()){
        editor->cut();
        return;
    }
    QList<QDocumentCursor> cursors = editor->cursors();
    if (cursors.empty()) return;
    // sort cursors by start linenumber
    std::sort(cursors.begin(),cursors.end());
    document->beginMacro();
    QString clipboard;
    int lastEndLine=-1;
    QList<int> skipCursors;
    for (int i=0;i<cursors.size();i++){
        int begincolumn,beginline,endcolumn,endline;
        cursors[i].boundaries(beginline,begincolumn,endline,endcolumn);
        if(lastEndLine>=0){
            if(beginline<=lastEndLine){ // second cursor in same line
                beginline=lastEndLine+1; // force start to next line
                if(endline<beginline){
                    skipCursors<<i;
                    continue; // skip cursor if endline is before beginline, i.e. cursor intersected previous extended cursor
                }
            }
        }
        cursors[i].select(beginline,0,endline,-1);
        lastEndLine=endline;
        clipboard+=cursors[i].selectedText()+"\n";
    }
    // delete lines later to not mess cursor positions
    for (int i=0;i<cursors.size();i++){
        if(skipCursors.contains(i)) continue;
        cursors[i].eraseLine();
    }
    document->endMacro();
    QApplication::clipboard()->setText(clipboard);
    editor->setCursor(cursors[0]);
}

void LatexEditorView::moveLines(int delta)
{
	REQUIRE(delta == -1 || delta == +1);
	QList<QDocumentCursor> cursors = editor->cursors();
	for (int i=0;i<cursors.length();i++)
		cursors[i].setAutoUpdated(false);
	QList<QPair<int, int> > blocks = getSelectedLineBlocks();
	document->beginMacro();
	int i = delta < 0 ? blocks.size() - 1 : 0;
    QVector<bool>skipMove(cursors.length(),false);
	while (i >= 0 && i < blocks.size()) {
		//edit
        if ((delta < 0 && blocks[i].first==0)||(delta > 0 && blocks[i].second==(document->lineCount()-1))) {
            skipMove[i]=true;
            i += delta;
            continue;
        }
        bool skipCursorUp=blocks[i].second==(document->lineCount()-1);
		QDocumentCursor edit = document->cursor(blocks[i].first, 0, blocks[i].second);
		QString text = edit.selectedText();
        edit.removeSelectedText();
        edit.eraseLine();
        if (delta < 0) {
            if(!skipCursorUp){
                // special treatment of last line of document
                edit.movePosition(1, QDocumentCursor::PreviousLine);
            }
			edit.movePosition(1, QDocumentCursor::StartOfLine);
			edit.insertText(text + "\n");
		} else {
			edit.movePosition(1, QDocumentCursor::EndOfLine);
			edit.insertText("\n" + text);
		}
		i += delta;
	}
	document->endMacro();
	//move cursors
	for (int i=0;i<cursors.length();i++) {
		cursors[i].setAutoUpdated(true);
        if(skipMove[i]) continue;
		if (cursors[i].hasSelection()) {
			cursors[i].setAnchorLineNumber(cursors[i].anchorLineNumber() + delta);
			cursors[i].setLineNumber(cursors[i].lineNumber() + delta, QDocumentCursor::KeepAnchor);
		} else
			cursors[i].setLineNumber(cursors[i].lineNumber() + delta);
		if (i == 0) editor->setCursor(cursors[i]);
		else editor->addCursorMirror(cursors[i]);
	}
}

QList<QPair<int, int> > LatexEditorView::getSelectedLineBlocks()
{
	QList<QDocumentCursor> cursors = editor->cursors();
	QList<int> lines;
	//get affected lines
	for (int i=0;i<cursors.length();i++) {
		if (cursors[i].hasSelection()) {
			QDocumentSelection sel = cursors[i].selection();
			for (int l=sel.startLine;l<=sel.endLine;l++)
				lines << l;
		} else lines << cursors[i].lineNumber();
	}
    std::sort(lines.begin(),lines.end());
	//merge blocks as speed up and to remove duplicates
	QList<QPair<int, int> > result;
	int i = 0;
	while (i < lines.size()) {
		int start = lines[i];
		int end = lines[i];
		i++;
		while ( i >= 0 && i < lines.size() && (lines[i] == end || lines[i] == end + 1)) {
			end = lines[i];
			i++;
		}
		result << QPair<int,int> (start, end);
	}
	return result;
}

QMultiMap<int, QDocumentCursor* > LatexEditorView::getSelectedLines(QList<QDocumentCursor>& cursors)
{
	QMultiMap<int, QDocumentCursor* > map;
	for (int i=0;i<cursors.length();i++) {
		if (cursors[i].hasSelection()) {
			QDocumentSelection sel = cursors[i].selection();
			for (int l=sel.startLine;l<=sel.endLine;l++)
				map.insert(l, &cursors[i]);
		} else map.insert(cursors[i].lineNumber(), &cursors[i]);
	}
	return map;
}

bool cursorPointerLessThan(QDocumentCursor* c1, QDocumentCursor* c2)
{
	return c1->columnNumber() < c2->columnNumber();
}

void LatexEditorView::alignMirrors()
{
	QList<QDocumentCursor> cursors = editor->cursors();
	QMultiMap<int, QDocumentCursor* > map = getSelectedLines(cursors);
	QList<int> lines = map.uniqueKeys();
	QList<QList<QDocumentCursor*> > cs;
	int colCount = 0;
	foreach (int l, lines) {
		QList<QDocumentCursor*> row = map.values(l);
		colCount = qMax(colCount, row.size());
        std::sort(row.begin(), row.end(), cursorPointerLessThan);
		cs.append(row);
	}
	document->beginMacro();
	for (int col=0;col<colCount;col++) {
		int pos = 0;
		for (int j=0;j<cs.size();j++)
			if (col < cs[j].size())
				pos = qMax(pos, cs[j][col]->columnNumber());
		for (int j=0;j<cs.size();j++)
			if (col < cs[j].size() && pos > cs[j][col]->columnNumber()) {
				cs[j][col]->insertText(QString(pos -  cs[j][col]->columnNumber(), ' '));
			}
	}
	document->endMacro();
}

void LatexEditorView::checkForLinkOverlay(QDocumentCursor cursor)
{
	if (cursor.atBlockEnd()) {
		removeLinkOverlay();
		return;
	}

	bool validPosition = cursor.isValid() && cursor.line().isValid();
	if (validPosition) {
		QDocumentLineHandle *dlh = cursor.line().handle();

        TokenList tl = dlh->getCookieLocked(QDocumentLine::LEXER_COOKIE).value<TokenList>();
		Token tk = Parsing::getTokenAtCol(dlh, cursor.columnNumber());

		if (tk.type == Token::labelRef || tk.type == Token::labelRefList) {
			setLinkOverlay(LinkOverlay(tk, LinkOverlay::RefOverlay));
		} else if (tk.type == Token::file) {
            Token cmdTk=Parsing::getCommandTokenFromToken(tl,tk);
            QString fn=tk.getText();
            if(cmdTk.dlh && cmdTk.getText()=="\\subimport"){
                int i=tl.indexOf(cmdTk);
                TokenList tl2=tl.mid(i); // in case of several cmds in one line
                QString path=Parsing::getArg(tl,Token::definition);
                if(!path.endsWith("/")){
                    path+="/";
                }
                fn=path+fn;
            }
            if(document->getStateImportedFile()){
                fn+="#";  // mark as relative to current file (subimport -> input)
            }

            LinkOverlay lo(tk, LinkOverlay::FileOverlay);
            lo.m_link=fn;
            setLinkOverlay(lo);
		} else if (tk.type == Token::url) {
			setLinkOverlay(LinkOverlay(tk, LinkOverlay::UrlOverlay));
		} else if (tk.type == Token::package) {
			setLinkOverlay(LinkOverlay(tk, LinkOverlay::UsepackageOverlay));
		} else if (tk.type == Token::bibfile) {
			setLinkOverlay(LinkOverlay(tk, LinkOverlay::BibFileOverlay));
		} else if (tk.type == Token::bibItem) {
			setLinkOverlay(LinkOverlay(tk, LinkOverlay::CiteOverlay));
		} else if (tk.type == Token::beginEnv || tk.type == Token::env) {
			setLinkOverlay(LinkOverlay(tk, LinkOverlay::EnvOverlay));
		} else if (tk.type == Token::commandUnknown) {
			setLinkOverlay(LinkOverlay(tk, LinkOverlay::CommandOverlay));
		} else if (tk.type == Token::command && tk.getText() != "\\begin" && tk.getText() != "\\end") {
			// avoid link overlays on \begin and \end; instead, the user can click the environment name
			setLinkOverlay(LinkOverlay(tk, LinkOverlay::CommandOverlay));
		} else {
			if (linkOverlay.isValid()) removeLinkOverlay();
		}
	} else {
		if (linkOverlay.isValid()) removeLinkOverlay();
	}
}

void LatexEditorView::setLinkOverlay(const LinkOverlay &overlay)
{
	if (linkOverlay.isValid()) {
		if (overlay == linkOverlay) {
			return; // same overlay
		} else {
			removeLinkOverlay();
		}
	}

	linkOverlay = overlay;
	linkOverlay.docLine.addOverlay(linkOverlay.formatRange);
	editor->viewport()->update(); // immediately apply the overlay
	linkOverlayStoredCursor = editor->viewport()->cursor();
	editor->viewport()->setCursor(Qt::PointingHandCursor);
}

void LatexEditorView::removeLinkOverlay()
{
	if (linkOverlay.isValid()) {
		linkOverlay.docLine.removeOverlay(linkOverlay.formatRange);
		linkOverlay = LinkOverlay();
		editor->viewport()->update(); // immediately apply the overlay
		editor->viewport()->setCursor(linkOverlayStoredCursor);
	}
}

bool LatexEditorView::isNonTextFormat(int format)
{
	if (format <= 0) return false;
	return format == numbersFormat
	       || format == verbatimFormat
	       || format == pictureFormat
	       || format == pweaveDelimiterFormat
	       || format == pweaveBlockFormat
	       || format == sweaveDelimiterFormat
	       || format == sweaveBlockFormat
	       || format == math_DelimiterFormat
	       || format == asymptoteBlockFormat;
}

void LatexEditorView::selectOptionInLatexArg(QDocumentCursor &cur)
{
	QString startDelims = "[{, \t\n";
	int startCol = cur.columnNumber();
	while (!cur.atLineStart() && !startDelims.contains(cur.previousChar())) {
		cur.movePosition(1, QDocumentCursor::PreviousCharacter);
	}
	cur.setColumnNumber(startCol, QDocumentCursor::KeepAnchor);
	QString endDelims = "]}, \t\n";
	while (!cur.atLineEnd() && !endDelims.contains(cur.nextChar())) {
		cur.movePosition(1, QDocumentCursor::NextCharacter, QDocumentCursor::KeepAnchor);
	}
}

void LatexEditorView::temporaryHighlight(QDocumentCursor cur)
{
	if (!cur.hasSelection()) return;
	REQUIRE(editor->document());

	QDocumentLine docLine(cur.selectionStart().line());
	if (cur.endLineNumber() != cur.startLineNumber()) {
		// TODO: proper highlighting of selections spanning more than one line. Currently just highlight to the end of the first line:
		cur = cur.selectionStart();
		cur.movePosition(1, QDocumentCursor::EndOfLine, QDocumentCursor::KeepAnchor);
	}

	QFormatRange highlight(cur.startColumnNumber(), cur.endColumnNumber() - cur.startColumnNumber(), editor->document()->getFormatId("search"));
	docLine.addOverlay(highlight);
	tempHighlightQueue.append(QPair<QDocumentLine, QFormatRange>(docLine, highlight));
	QTimer::singleShot(1000, this, SLOT(removeTemporaryHighlight()));
}

void LatexEditorView::removeTemporaryHighlight()
{
	if (!tempHighlightQueue.isEmpty()) {
		QDocumentLine docLine(tempHighlightQueue.first().first);
		docLine.removeOverlay(tempHighlightQueue.first().second);
		tempHighlightQueue.removeFirst();
	}
}

void LatexEditorView::displayLineGrammarErrorsInternal(int lineNr, const QList<GrammarError> &errors)
{
    const QList<int> nonTextFormats = {numbersFormat, verbatimFormat, pictureFormat, pweaveDelimiterFormat, pweaveBlockFormat,
                                       sweaveDelimiterFormat, sweaveBlockFormat, math_DelimiterFormat, asymptoteBlockFormat};
	QDocumentLine line = document->line(lineNr);
	foreach (const int f, grammarFormats)
		line.clearOverlays(f);
	foreach (const GrammarError &error, errors) {
		int f;
		if (error.error == GET_UNKNOWN) f = grammarMistakeFormat;
		else {
            int index = static_cast<int>(error.error) - 1;
			REQUIRE(index < grammarFormats.size());
			if (grammarFormatsDisabled[index]) continue;
			f = grammarFormats[index];
		}
        if (config->hideNonTextGrammarErrors){
            QFormatRange overlays=line.getOverlayAt(error.offset,nonTextFormats);
            if(overlays.length>0)
                continue;
        }
		line.addOverlay(QFormatRange(error.offset, error.length, f));
	}
	//todo: check for width changing like if (changed && ff->format(wordRepetitionFormat).widthChanging()) line.handle()->updateWrapAndNotifyDocument(i);
}

void LatexEditorView::lineGrammarChecked(LatexDocument *doc, QDocumentLineHandle *lineHandle, int lineNr, const QList<GrammarError> &errors)
{
	if (doc != this->document) return;
    lineNr = document->indexOf(lineHandle, lineNr);
	if (lineNr < 0) return; //line already deleted
	displayLineGrammarErrorsInternal(lineNr, errors);
	document->line(lineNr).setCookie(QDocumentLine::GRAMMAR_ERROR_COOKIE, QVariant::fromValue<QList<GrammarError> >(errors));
}

void LatexEditorView::setGrammarOverlayDisabled(int type, bool newValue)
{
	REQUIRE(type >= 0 && type < grammarFormatsDisabled.size());
	if (newValue == grammarFormatsDisabled[type]) return;
	grammarFormatsDisabled[type] = newValue;
}

void LatexEditorView::updateGrammarOverlays()
{
	for (int i = 0; i < document->lineCount(); i++)
		displayLineGrammarErrorsInternal(i, document->line(i).getCookie(QDocumentLine::GRAMMAR_ERROR_COOKIE).value<QList<GrammarError> >());
	editor->viewport()->update();
}

void LatexEditorView::viewActivated()
{
	if (!LatexEditorView::completer) return;
}

/*!
 * Returns the name to be displayed when a short textual reference to the editor is required
 * such as in the tab or in a list of open documents.
 * This name is not necessarily unique.
 */
QString LatexEditorView::displayName() const
{
	return (!editor || editor->fileName().isEmpty() ? tr("untitled") : editor->name());
}

/*!
 * Returns the displayName() with properly escaped ampersands for UI elements
 * such as tabs and actions.
 */
QString LatexEditorView::displayNameForUI() const
{
	return displayName().replace('&', "&&");
}

void LatexEditorView::complete(int flags)
{
	if (!LatexEditorView::completer) return;
	setFocus();
	LatexEditorView::completer->complete(editor, LatexCompleter::CompletionFlags(flags));
}

void LatexEditorView::jumpChangePositionBackward()
{
	if (changePositions.size() == 0) return;
	for (int i = changePositions.size() - 1; i >= 0; i--)
		if (!changePositions[i].isValid()) {
			changePositions.removeAt(i);
			if (i <= curChangePos) curChangePos--;
		}
	if (curChangePos >= changePositions.size() - 1) curChangePos = changePositions.size() - 1;
	else if (curChangePos >= 0 && curChangePos < changePositions.size() - 1) curChangePos++;
	else if (editor->cursor().line().handle() == changePositions.first().dlh()) curChangePos = 1;
	else curChangePos = 0;
	if (curChangePos >= 0 && curChangePos < changePositions.size())
		editor->setCursorPosition(changePositions[curChangePos].lineNumber(), changePositions[curChangePos].columnNumber());
}

void LatexEditorView::jumpChangePositionForward()
{
	for (int i = changePositions.size() - 1; i >= 0; i--)
		if (!changePositions[i].isValid()) {
			changePositions.removeAt(i);
			if (i <= curChangePos) curChangePos--;
		}
	if (curChangePos > 0) {
		curChangePos--;
		editor->setCursorPosition(changePositions[curChangePos].lineNumber(), changePositions[curChangePos].columnNumber());
	}
}

void LatexEditorView::jumpToBookmark(int bookmarkNumber)
{
	int markLine = editor->document()->findNextMark(bookMarkId(bookmarkNumber), editor->cursor().lineNumber(), editor->cursor().lineNumber() - 1);
	if (markLine >= 0) {
		emit saveCurrentCursorToHistoryRequested();
		editor->setCursorPosition(markLine, 0, false);
		editor->ensureCursorVisible(QEditor::NavigationToHeader);
		editor->setFocus();
	}
}

void LatexEditorView::removeBookmark(QDocumentLineHandle *dlh, int bookmarkNumber)
{
	if (!dlh)
		return;
	int rmid = bookMarkId(bookmarkNumber);
	if (hasBookmark(dlh, bookmarkNumber)) {
		document->removeMark(dlh, rmid);
		emit bookmarkRemoved(dlh);
	}
}

void LatexEditorView::removeBookmark(int lineNr, int bookmarkNumber)
{
	removeBookmark(document->line(lineNr).handle(), bookmarkNumber);
}

void LatexEditorView::addBookmark(int lineNr, int bookmarkNumber)
{
	int rmid = bookMarkId(bookmarkNumber);
    if (bookmarkNumber >= 0){
        int ln=document->findNextMark(rmid);
		document->line(ln).removeMark(rmid);
        editor->removeMark(document->line(ln).handle(),"bookmark");
    }
    if (!document->line(lineNr).hasMark(rmid)){
		document->line(lineNr).addMark(rmid);
        editor->addMark(document->line(lineNr).handle(),Qt::darkMagenta,"bookmark");
    }
}

bool LatexEditorView::hasBookmark(int lineNr, int bookmarkNumber)
{
	int rmid = bookMarkId(bookmarkNumber);
	return document->line(lineNr).hasMark(rmid);
}

bool LatexEditorView::hasBookmark(QDocumentLineHandle *dlh, int bookmarkNumber)
{
	if (!dlh)
		return false;
	int rmid = bookMarkId(bookmarkNumber);
	QList<int> m_marks = document->marks(dlh);
	return m_marks.contains(rmid);
}
/*!
 * \brief check if line has any bookmark (bookmarkid -1 .. 9)
 * \param dlh
 * \return bookmark number [-1:9] , -2 -> not found
 */
int LatexEditorView::hasBookmark(QDocumentLineHandle *dlh)
{
    if (!dlh)
        return false;
    int rmidLow = bookMarkId(-1);
    int rmidUp = bookMarkId(9);
    QList<int> m_marks = document->marks(dlh);
    for(int id:m_marks){
        if(id>=rmidLow && id<=rmidUp){
            return id;
        }
    }
    return -2; // none found
}


bool LatexEditorView::toggleBookmark(int bookmarkNumber, QDocumentLine line)
{
	if (!line.isValid()) line = editor->cursor().line();
	int rmid = bookMarkId(bookmarkNumber);
	if (line.hasMark(rmid)) {
		line.removeMark(rmid);
		emit bookmarkRemoved(line.handle());
		return false;
	}
	if (bookmarkNumber >= 0) {
		int ln = editor->document()->findNextMark(rmid);
		if (ln >= 0) {
			editor->document()->line(ln).removeMark(rmid);
			emit bookmarkRemoved(editor->document()->line(ln).handle());
		}
	}
	for (int i = -1; i < 10; i++) {
		int rmid = bookMarkId(i);
		if (line.hasMark(rmid)) {
			line.removeMark(rmid);
			emit bookmarkRemoved(line.handle());
		}
	}
	line.addMark(rmid);
	emit bookmarkAdded(line.handle(), bookmarkNumber);
	return true;
}

bool LatexEditorView::gotoLineHandleAndSearchCommand(const QDocumentLineHandle *dlh, const QSet<QString> &searchFor, const QString &id)
{
	if (!dlh) return false;
	int ln = dlh->document()->indexOf(dlh);
	if (ln < 0) return false;
	QString lineText = dlh->document()->line(ln).text();
	int col = 0;
	foreach (const QString &cmd, searchFor) {
		col = lineText.indexOf(cmd + "{" + id);
        if (col < 0) col = lineText.indexOf(QRegularExpression(QRegularExpression::escape(cmd) + "\\[[^\\]{}()\\\\]+\\]\\{" + QRegularExpression::escape(id))); //for \command[options]{id}
		if (col >= 0) {
			col += cmd.length() + 1;
			break;
		}
	}
	//Q_ASSERT(col >= 0);
	bool colFound = (col >= 0);
	if (col < 0) col = 0;
	editor->setCursorPosition(ln, col, false);
	editor->ensureCursorVisible(QEditor::Navigation);
	if (colFound) {
		QDocumentCursor highlightCursor(editor->cursor());
		highlightCursor.movePosition(id.length(), QDocumentCursor::NextCharacter, QDocumentCursor::KeepAnchor);
		temporaryHighlight(highlightCursor);
	}
	return true;
}

bool LatexEditorView::gotoLineHandleAndSearchString(const QDocumentLineHandle *dlh, const QString &str)
{
	if (!dlh) return false;
	int ln = dlh->document()->indexOf(dlh);
	if (ln < 0) return false;
	QString lineText = dlh->document()->line(ln).text();
	int col = lineText.indexOf(str);
	bool colFound = (col >= 0);
	if (col < 0) col = 0;
	editor->setCursorPosition(ln, col, false);
	editor->ensureCursorVisible(QEditor::Navigation);
	if (colFound) {
		QDocumentCursor highlightCursor(editor->cursor());
		highlightCursor.movePosition(str.length(), QDocumentCursor::NextCharacter, QDocumentCursor::KeepAnchor);
		temporaryHighlight(highlightCursor);
	}
	return true;
}

bool LatexEditorView::gotoLineHandleAndSearchLabel(const QDocumentLineHandle *dlh, const QString &label)
{
	return gotoLineHandleAndSearchCommand(dlh, LatexParser::getInstance().possibleCommands["%label"], label);
}

bool LatexEditorView::gotoLineHandleAndSearchBibItem(const QDocumentLineHandle *dlh, const QString &bibId)
{
	return gotoLineHandleAndSearchCommand(dlh, LatexParser::getInstance().possibleCommands["%bibitem"], bibId);
}

//collapse/expand every possible line
void LatexEditorView::foldEverything(bool unFold)
{
	QDocument *doc = editor->document();
	QLanguageDefinition *ld = doc->languageDefinition();
	if (!ld) return;
	QFoldedLineIterator fli = ld->foldedLineIterator(doc, 0);
	for (int i = 0; i < doc->lines(); i++, ++fli)
		if (fli.open) {
			if (unFold) ld->expand(doc, i);
			else ld->collapse(doc, i);
		}
}

//collapse/expand lines at the top level
void LatexEditorView::foldLevel(bool unFold, int level)
{
	QDocument *doc = editor->document();
	QLanguageDefinition *ld = doc->languageDefinition();
	if (!ld) return;
	for (QFoldedLineIterator fli = ld->foldedLineIterator(doc);
	        fli.line.isValid(); ++fli) {
		if (fli.openParentheses.size() == level && fli.open) {
			if (unFold) ld->expand(doc, fli.lineNr);
			else ld->collapse(doc, fli.lineNr);
		}
	}/*
 QDocument* doc = editor->document();
 QLanguageDefinition* ld = doc->languageDefinition();
 int depth=0;
 for (int n = 0; n < doc->lines(); ++n) {
  QDocumentLine b=doc->line(n);
  if (b.isHidden()) continue;

  int flags = ld->blockFlags(doc, n, depth);
  short open = QCE_FOLD_OPEN_COUNT(flags);
  short close = QCE_FOLD_CLOSE_COUNT(flags);

  depth -= close;

  if (depth < 0)
   depth = 0;

  depth += open;

  if (depth==level) {
   if (unFold && (flags & QLanguageDefinition::Collapsed))
    ld->expand(doc,n);
   else if (!unFold && (flags & QLanguageDefinition::Collapsible))
    ld->collapse(doc,n);
  }
  if (ld->blockFlags(doc, n, depth) & QLanguageDefinition::Collapsed)
   depth -= open; // outermost block folded : none of the opening is actually opened
 }*/
}

//Collapse at the first possible point before/at line
void LatexEditorView::foldBlockAt(bool unFold, int line)
{
	editor->document()->foldBlockAt(unFold, line);
}

void LatexEditorView::zoomIn()
{
	editor->zoom(1);
}

void LatexEditorView::zoomOut()
{
	editor->zoom(-1);
}

void LatexEditorView::resetZoom()
{
	editor->resetZoom();
}

void LatexEditorView::cleanBib()
{
	QDocument *doc = editor->document();
	for (int i = doc->lines() - 1; i >= 0; i--) {
		QString trimLine = doc->line(i).text().trimmed();
		if (trimLine.startsWith("OPT") || trimLine.startsWith("ALT"))
			doc->execute(new QDocumentEraseCommand(i, 0, i + 1, 0, doc));
	}
	setFocus();
}

QList<QAction *> LatexEditorView::getBaseActions()
{
    return s_baseActions;
}

void LatexEditorView::setBaseActions(QList<QAction *> baseActions)
{
    s_baseActions = baseActions;
}
/*!
 * \brief return the line row where context menu was started
 * DefaultInputBinding only
 * \return
 */
int LatexEditorView::getLineRowforContexMenu()
{
    return s_contextMenuRow;
}
/*!
 * \brief return the line column where context menu was started
 * DefaultInputBinding only
 * \return
 */
int LatexEditorView::getLineColforContexMenu()
{
    return s_contextMenuCol;
}

void LatexEditorView::setSpellerManager(SpellerManager *manager)
{
	spellerManager = manager;
	connect(spellerManager, SIGNAL(defaultSpellerChanged()), this, SLOT(reloadSpeller()));
}
/*!
 * \brief Set new spelling language
 * No change if old and new are identical
 * The speller engine is forwarded to the syntax checker where the actual online checking is done.
 * \param name of the desired language
 * \return success of operation
 */
bool LatexEditorView::setSpeller(const QString &name, bool updateComment)
{
	if (!spellerManager) return false;

	useDefaultSpeller = (name == "<default>");

	SpellerUtility *su;
	if (spellerManager->hasSpeller(name)) {
		su = spellerManager->getSpeller(name);
		if (!su) return false;
	} else {
		su = spellerManager->getSpeller(spellerManager->defaultSpellerName());
		REQUIRE_RET(su, false);
		useDefaultSpeller = true;
	}
	if (su == speller) return true; // nothing to do

    bool dontRecheck=false;

	if (speller) {
		disconnect(speller, SIGNAL(aboutToDelete()), this, SLOT(reloadSpeller()));
		disconnect(speller, SIGNAL(ignoredWordAdded(QString)), this, SLOT(spellRemoveMarkers(QString)));
    }else {
        dontRecheck=true;
    }
	speller = su;
    if(document){
        SpellerUtility::inlineSpellChecking=config->inlineSpellChecking && config->realtimeChecking;
        SpellerUtility::hideNonTextSpellingErrors=config->hideNonTextSpellingErrors;
        document->setSpeller(speller);
    }
	connect(speller, SIGNAL(aboutToDelete()), this, SLOT(reloadSpeller()));
	connect(speller, SIGNAL(ignoredWordAdded(QString)), this, SLOT(spellRemoveMarkers(QString)));
	emit spellerChanged(name);

    if (document && updateComment) {
        document->updateMagicComment("spellcheck", speller ? speller->name() : "<none>");
    }

	// force new highlighting
    if(!dontRecheck && document){
        document->reCheckSyntax(0, document->lineCount());
    }

	return true;
}
/*!
 * \brief reload speller engine with new engine (sender())
 */
void LatexEditorView::reloadSpeller()
{
	if (useDefaultSpeller) {
		setSpeller("<default>");
		return;
	}

	SpellerUtility *su = qobject_cast<SpellerUtility *>(sender());
	if (!su) return;
	setSpeller(su->name());
}

QString LatexEditorView::getSpeller()
{
	if (useDefaultSpeller) return QString("<default>");
    if(speller==nullptr) return QString("<none>");
	return speller->name();
}

void LatexEditorView::setCompleter(LatexCompleter *newCompleter)
{
	LatexEditorView::completer = newCompleter;
}

LatexCompleter *LatexEditorView::getCompleter()
{
	return LatexEditorView::completer;
}

void LatexEditorView::updatePackageFormats()
{
	for (int i = 0; i < editor->document()->lines(); i++) {
        QDocumentLineHandle *dlh=editor->document()->line(i).handle();
        QList<QFormatRange> li = dlh->getOverlays(-1);
        QString curLineText = dlh->text();
        TokenList tl = dlh->getCookieLocked(QDocumentLine::LEXER_COOKIE).value<TokenList>();
        for (const Token &tk : tl) {
            if(tk.type != Token::package && tk.type!=Token::beamertheme && tk.type!=Token::documentclass) continue;
            QString preambel;
            if (tk.type == Token::beamertheme) { // special treatment for  \usetheme
                preambel = "beamertheme";
            }
            const QString rpck =  trimLeft(curLineText.mid(tk.start, tk.length)); // left spaces are ignored by \cite, right space not
            const QString suffix = tk.type == Token::documentclass ? ".cls" : ".sty";
            //check and highlight
            bool localPackage = false;
            if(rpck.startsWith(".")){
                // check if file exists
                LatexDocument *root=document->getRootDocument();
                QFileInfo fi=root->getFileInfo();
                QFileInfo fi_cwl=QFileInfo(fi.absolutePath(),rpck+suffix);
                localPackage=fi_cwl.exists();
            }
            if (latexPackageList->empty())
                dlh->addOverlay(QFormatRange(tk.start, tk.length, packageUndefinedFormat));
            else if ( (latexPackageList->find(preambel + rpck + suffix) != latexPackageList->end())
                      || (latexPackageList->find(preambel + rpck) != latexPackageList->end())
                     || localPackage) {
                dlh->addOverlay(QFormatRange(tk.start, tk.length, packagePresentFormat));
            } else {
                dlh->addOverlay(QFormatRange(tk.start, tk.length, packageMissingFormat));
            }
        }
	}
}

void LatexEditorView::clearLogMarks()
{
	setLogMarksVisible(false);
	logEntryToLine.clear();
	logEntryToMarkID.clear();
	lineToLogEntries.clear();
}

void LatexEditorView::addLogEntry(int logEntryNumber, int lineNumber, int markID)
{
	QDocumentLine l = editor->document()->line(lineNumber);
	lineToLogEntries.insert(l.handle(), logEntryNumber);
	logEntryToLine[logEntryNumber] = l.handle();
	logEntryToMarkID[logEntryNumber] = markID;
}

void LatexEditorView::setLogMarksVisible(bool visible)
{
	if (visible) {
		foreach (int logEntryNumber, logEntryToMarkID.keys()) {
			int markID = logEntryToMarkID[logEntryNumber];
			if (markID >= 0) {
				QDocumentLine(logEntryToLine[logEntryNumber]).addMark(markID);
			}
		}
	} else {
		int errorMarkID = QLineMarksInfoCenter::instance()->markTypeId("error");
		int warningMarkID = QLineMarksInfoCenter::instance()->markTypeId("warning");
		int badboxMarkID = QLineMarksInfoCenter::instance()->markTypeId("badbox");
		editor->document()->removeMarks(errorMarkID);
		editor->document()->removeMarks(warningMarkID);
		editor->document()->removeMarks(badboxMarkID);
	}
}

void LatexEditorView::updateCitationFormats()
{
	for (int i = 0; i < editor->document()->lines(); i++) {
		QList<QFormatRange> li = editor->document()->line(i).getOverlays();
		QString curLineText = editor->document()->line(i).text();
		for (int j = 0; j < li.size(); j++)
			if (li[j].format == citationPresentFormat || li[j].format == citationMissingFormat) {
				int newFormat = document->bibIdValid(curLineText.mid(li[j].offset, li[j].length)) ? citationPresentFormat : citationMissingFormat;
				if (newFormat != li[j].format) {
					editor->document()->line(i).removeOverlay(li[j]);
					li[j].format = newFormat;
					editor->document()->line(i).addOverlay(li[j]);
				}
			}
	}
}

int LatexEditorView::bookMarkId(int bookmarkNumber)
{
	if (bookmarkNumber == -1) return  QLineMarksInfoCenter::instance()->markTypeId("bookmark"); //unnumbered mark
    if(bookmarkNumber>9)
        bookmarkNumber=0;
    return QLineMarksInfoCenter::instance()->markTypeId("bookmark" + QString::number(bookmarkNumber));
	//return document->bookMarkId(bookmarkNumber);
}

void LatexEditorView::setLineMarkToolTip(const QString &tooltip)
{
	lineMarkPanel->setToolTipForTouchedMark(tooltip);
}

int LatexEditorView::environmentFormat, LatexEditorView::referencePresentFormat, LatexEditorView::referenceMissingFormat, LatexEditorView::referenceMultipleFormat, LatexEditorView::citationMissingFormat,
    LatexEditorView::citationPresentFormat, LatexEditorView::structureFormat, LatexEditorView::todoFormat, LatexEditorView::packageMissingFormat, LatexEditorView::packagePresentFormat, LatexEditorView::packageUndefinedFormat,
    LatexEditorView::wordRepetitionFormat, LatexEditorView::wordRepetitionLongRangeFormat, LatexEditorView::badWordFormat, LatexEditorView::grammarMistakeFormat, LatexEditorView::grammarMistakeSpecial1Format,
    LatexEditorView::grammarMistakeSpecial2Format, LatexEditorView::grammarMistakeSpecial3Format, LatexEditorView::grammarMistakeSpecial4Format,
    LatexEditorView::numbersFormat, LatexEditorView::verbatimFormat, LatexEditorView::commentFormat, LatexEditorView::pictureFormat, LatexEditorView::math_DelimiterFormat, LatexEditorView::math_KeywordFormat,
    LatexEditorView::pweaveDelimiterFormat, LatexEditorView::pweaveBlockFormat, LatexEditorView::sweaveDelimiterFormat, LatexEditorView::sweaveBlockFormat,
    LatexEditorView::asymptoteBlockFormat;
int LatexEditorView::syntaxErrorFormat, LatexEditorView::preEditFormat;
int LatexEditorView::deleteFormat, LatexEditorView::insertFormat, LatexEditorView::replaceFormat;

QList<int> LatexEditorView::grammarFormats;
QList<int> LatexEditorView::delimiterFormats;
QVector<bool> LatexEditorView::grammarFormatsDisabled;
QList<int> LatexEditorView::formatsList;

void LatexEditorView::updateSettings()
{
	lineNumberPanel->setVerboseMode(config->showlinemultiples != 10);
	editor->setFont(QFont(config->fontFamily, config->fontSize));
	editor->setLineWrapping(config->wordwrap > 0);
	editor->setSoftLimitedLineWrapping(config->wordwrap == 2);
	editor->setHardLineWrapping(config->wordwrap > 2);
	if (config->wordwrap > 1) {
		editor->setWrapAfterNumChars(config->lineWidth);
	} else {
		editor->setWrapAfterNumChars(0);
	}
	editor->setFlag(QEditor::AutoIndent, config->autoindent);
	editor->setFlag(QEditor::WeakIndent, config->weakindent);
	editor->setFlag(QEditor::ReplaceIndentTabs, config->replaceIndentTabs);
	editor->setFlag(QEditor::ReplaceTextTabs, config->replaceTextTabs);
	editor->setFlag(QEditor::RemoveTrailing, config->removeTrailingWsOnSave);
	editor->setFlag(QEditor::AllowDragAndDrop, config->allowDragAndDrop);
	editor->setFlag(QEditor::MouseWheelZoom, config->mouseWheelZoom);
	editor->setFlag(QEditor::SmoothScrolling, config->smoothScrolling);
	editor->setFlag(QEditor::VerticalOverScroll, config->verticalOverScroll);
	editor->setFlag(QEditor::AutoInsertLRM, config->autoInsertLRM);
	editor->setFlag(QEditor::BidiVisualColumnMode, config->visualColumnMode);
    editor->setFlag(QEditor::ShowIndentGuides, config->showIndentGuides);
	editor->setFlag(QEditor::OverwriteOpeningBracketFollowedByPlaceholder, config->overwriteOpeningBracketFollowedByPlaceholder);
	editor->setFlag(QEditor::OverwriteClosingBracketFollowingPlaceholder, config->overwriteClosingBracketFollowingPlaceholder);
	//TODO: parenmatch
	editor->setFlag(QEditor::AutoCloseChars, config->parenComplete);
	editor->setFlag(QEditor::ShowPlaceholders, config->showPlaceholders);
	editor->setDoubleClickSelectionType(config->doubleClickSelectionIncludeLeadingBackslash ? QDocumentCursor::WordOrCommandUnderCursor : QDocumentCursor::WordUnderCursor);
	editor->setTripleClickSelectionType((QList<QDocumentCursor::SelectionType>()
										<< QDocumentCursor::WordUnderCursor
										<< QDocumentCursor::WordOrCommandUnderCursor
										<< QDocumentCursor::ParenthesesInner
										<< QDocumentCursor::ParenthesesOuter
										<< QDocumentCursor::LineUnderCursor).at(qMax(0, qMin(4, config->tripleClickSelectionIndex))));
	editor->setIgnoreExternalChanges(!config->monitorFilesForExternalChanges);
	editor->setSilentReloadOnExternalChanges(config->silentReload);
	editor->setUseQSaveFile(config->useQSaveFile);
	editor->setHidden(false);
	editor->setCursorSurroundingLines(config->cursorSurroundLines);
	editor->setCursorBold(config->boldCursor);
	lineMarkPanelAction->setChecked((config->showlinemultiples != 0) || config->folding || config->showlinestate);
	lineNumberPanelAction->setChecked(config->showlinemultiples != 0);
	lineFoldPanelAction->setChecked(config->folding);
	lineChangePanelAction->setChecked(config->showlinestate);
	statusPanelAction->setChecked(config->showcursorstate);
	editor->setDisplayModifyTime(false);
	searchReplacePanel->setUseLineForSearch(config->useLineForSearch);
	searchReplacePanel->setSearchOnlyInSelection(config->searchOnlyInSelection);
    QDocument::WhiteSpaceMode wsMode=config->showWhitespace ? (QDocument::ShowTrailing | QDocument::ShowLeading | QDocument::ShowTabs) : QDocument::ShowNone;
    if(config->showIndentGuides){
        wsMode = wsMode | QDocument::ShowIndentGuides;
    }
    QDocument::setShowSpaces(wsMode);
	QDocument::setTabStop(config->tabStop);
	QDocument::setLineSpacingFactor(config->lineSpacingPercent / 100.0);

	editor->m_preEditFormat = preEditFormat;

	QDocument::setWorkAround(QDocument::DisableFixedPitchMode, config->hackDisableFixedPitch);
	QDocument::setWorkAround(QDocument::DisableWidthCache, config->hackDisableWidthCache);
	QDocument::setWorkAround(QDocument::DisableLineCache, config->hackDisableLineCache);

	QDocument::setWorkAround(QDocument::ForceQTextLayout, config->hackRenderingMode == 1);
	QDocument::setWorkAround(QDocument::ForceSingleCharacterDrawing, config->hackRenderingMode == 2);
	LatexDocument::syntaxErrorFormat = syntaxErrorFormat;
	if (document){
        document->setHideNonTextGrammarErrors(config->hideNonTextGrammarErrors);
        document->setGrammarFormats(grammarFormats);
        document->enableRainbowDelimiters(config->enableRainbowDelimiters);
        document->setDelimiterFormats(delimiterFormats);
		document->updateSettings();
        document->setCenterDocumentInEditor(config->centerDocumentInEditor);
    }
    rebindInputMode();
}

void LatexEditorView::updateFormatSettings()
{
	static bool formatsLoaded = false;
	if (!formatsLoaded) {
		REQUIRE(QDocument::defaultFormatScheme());

        const void *formats[] = {
            & environmentFormat , "environment" ,
            & referenceMultipleFormat , "referenceMultiple" ,
            & referencePresentFormat , "referencePresent" ,
            & referenceMissingFormat , "referenceMissing" ,
            & citationPresentFormat , "citationPresent" ,
            & citationMissingFormat , "citationMissing" ,
            & packageMissingFormat , "packageMissing" ,
            & packagePresentFormat , "packagePresent" ,
            & packageUndefinedFormat, "normal",
            & syntaxErrorFormat, "latexSyntaxMistake", //TODO: rename all to xFormat, "x"
            & structureFormat , "structure" ,
            & todoFormat , "commentTodo" ,
            & deleteFormat, "diffDelete",
            & insertFormat, "diffAdd",
            & replaceFormat, "diffReplace",
            & wordRepetitionFormat , "wordRepetition" ,
            & wordRepetitionLongRangeFormat , "wordRepetitionLongRange" ,
            & badWordFormat , "badWord" ,
            & grammarMistakeFormat , "grammarMistake" ,
            & grammarMistakeSpecial1Format , "grammarMistakeSpecial1" ,
            & grammarMistakeSpecial2Format , "grammarMistakeSpecial2" ,
            & grammarMistakeSpecial3Format , "grammarMistakeSpecial3" ,
            & grammarMistakeSpecial4Format , "grammarMistakeSpecial4" ,
            & numbersFormat , "numbers" ,
            & verbatimFormat , "verbatim" ,
            & commentFormat , "comment" ,
            & pictureFormat ,"picture" ,
            & pweaveDelimiterFormat , "pweave-delimiter" ,
            & pweaveBlockFormat , "pweave-block" ,
            & sweaveDelimiterFormat , "sweave-delimiter" ,
            & sweaveBlockFormat , "sweave-block" ,
            & math_DelimiterFormat , "math-delimiter" ,
            & math_KeywordFormat , "math-keyword" ,
            & asymptoteBlockFormat , "asymptote:block" ,
            & preEditFormat , "preedit" ,
            nullptr, nullptr
        };

		const void **temp = formats;
		while (*temp) {
			int *c = (static_cast<int *>(const_cast<void *>(*temp)));
            Q_ASSERT(c != nullptr);
			*c = QDocument::defaultFormatScheme()->id(QString(static_cast<const char *>(*(temp + 1))));
			temp += 2;
		}
		//int f=QDocument::formatFactory()->id("citationMissing");
		formatsLoaded = true;
		grammarFormats << wordRepetitionFormat << wordRepetitionLongRangeFormat << badWordFormat << grammarMistakeFormat << grammarMistakeSpecial1Format << grammarMistakeSpecial2Format << grammarMistakeSpecial3Format << grammarMistakeSpecial4Format; //don't change the order, it corresponds to GrammarErrorType
		grammarFormatsDisabled.resize(9);
		grammarFormatsDisabled.fill(false);
        formatsList << referencePresentFormat << citationPresentFormat << referenceMissingFormat;
		formatsList << referenceMultipleFormat << citationMissingFormat << packageMissingFormat << packagePresentFormat << packageUndefinedFormat << environmentFormat;
		formatsList << wordRepetitionFormat << structureFormat << todoFormat << insertFormat << deleteFormat << replaceFormat;
		LatexDocument::syntaxErrorFormat = syntaxErrorFormat;
        // delimiter colors
        delimiterFormats << QDocument::defaultFormatScheme()->id("braceLevel0") << QDocument::defaultFormatScheme()->id("braceLevel1") << QDocument::defaultFormatScheme()->id("braceLevel2") << QDocument::defaultFormatScheme()->id("braceLevel3") << QDocument::defaultFormatScheme()->id("braceLevel4") << QDocument::defaultFormatScheme()->id("braceLevel5") << QDocument::defaultFormatScheme()->id("braceLevel6") << QDocument::defaultFormatScheme()->id("braceLevel7");
	}
}

void LatexEditorView::requestCitation()
{
	QString id = editor->cursor().selectedText();
	emit needCitation(id);
}

void LatexEditorView::openExternalFile()
{
	QAction *act = qobject_cast<QAction *>(sender());
	QString name = act->data().toString();
    name.replace("\\string~",QDir::homePath());
    if(document->getStateImportedFile()){
        name+="#";
    }
	if (!name.isEmpty())
		emit openFile(name);
}

void LatexEditorView::openPackageDocumentation(QString package)
{
	QString command;
	if (package.isEmpty()) {
		QAction *act = qobject_cast<QAction *>(sender());
		if (!act) return;
		package = act->data().toString();
	}
	if (package.contains("#")) {
		int i = package.indexOf("#");
		command = package.mid(i + 1);
		package = package.left(i);
	}
	// replace some package denominations
	if (package == "latex-document" || package == "latex-dev")
		package = "latex2e";
	if (package == "class-scrartcl,scrreprt,scrbook")
		package = "scrartcl";
	if (package.startsWith("class-"))
		package = package.mid(6);
    if (!package.isEmpty() && help) {
		if (config->texdocHelpInInternalViewer) {
            QString docfile = help->packageDocFile(package);
			if (docfile.isEmpty())
				return;
			if (docfile.endsWith(".pdf"))
				emit openInternalDocViewer(docfile, command);
			else
                help->viewTexdoc(package); // fallback e.g. for dvi

		} else {
            help->viewTexdoc(package);
		}
	}
}

void LatexEditorView::emitChangeDiff()
{
	QAction *act = qobject_cast<QAction *>(sender());
	QPoint pt = act->data().toPoint();
	emit changeDiff(pt);
}

void LatexEditorView::emitGotoDefinitionFromAction()
{
	QDocumentCursor c;
	QAction *act = qobject_cast<QAction *>(sender());
	if (act) {
		c = act->data().value<QDocumentCursor>();
	}
	emit gotoDefinition(c);
}

void LatexEditorView::emitFindLabelUsagesFromAction()
{
	QAction *action = qobject_cast<QAction *>(sender());
	if (!action) return;
	QString labelText = action->data().toString();
	LatexDocument *doc = action->property("doc").value<LatexDocument *>();
	emit findLabelUsages(doc, labelText);
}

void LatexEditorView::emitFindSpecialUsagesFromAction()
{
    QAction *action = qobject_cast<QAction *>(sender());
    if (!action) return;
    QString labelText = action->data().toString();
    LatexDocument *doc = action->property("doc").value<LatexDocument *>();
    int type= action->property("type").toInt();
    emit findSpecialUsages(doc, labelText,type);
}

void LatexEditorView::emitSyncPDFFromAction()
{
	QDocumentCursor c;
	QAction *act = qobject_cast<QAction *>(sender());
	if (act) {
		c = act->data().value<QDocumentCursor>();
	}
	emit syncPDFRequested(c);
}

void LatexEditorView::lineMarkClicked(int line)
{
	QDocumentLine l = editor->document()->line(line);
	if (!l.isValid()) return;
	//remove old mark (when possible)
	for (int i = -1; i < 10; i++)
		if (l.hasMark(bookMarkId(i))) {
			l.removeMark(bookMarkId(i));
			emit bookmarkRemoved(l.handle());
			return;
		}
	// remove error marks
	if (l.hasMark(QLineMarksInfoCenter::instance()->markTypeId("error"))) {
		l.removeMark(QLineMarksInfoCenter::instance()->markTypeId("error"));
		return;
	}
	if (l.hasMark(QLineMarksInfoCenter::instance()->markTypeId("warning"))) {
		l.removeMark(QLineMarksInfoCenter::instance()->markTypeId("warning"));
		return;
	}
	if (l.hasMark(QLineMarksInfoCenter::instance()->markTypeId("badbox"))) {
		l.removeMark(QLineMarksInfoCenter::instance()->markTypeId("badbox"));
		return;
	}
	//add unused mark (1,2 .. 9,0) (when possible)
	for (int i = 1; i <= 10; i++) {
		if (editor->document()->findNextMark(bookMarkId(i % 10)) < 0) {
			l.addMark(bookMarkId(i % 10));
			emit bookmarkAdded(l.handle(), i);
			return;
		}
	}
	l.addMark(bookMarkId(-1));
	emit bookmarkAdded(l.handle(), -1);
}

void LatexEditorView::lineMarkToolTip(int line, int mark)
{
	if (line < 0 || line >= editor->document()->lines()) return;
	int errorMarkID = QLineMarksInfoCenter::instance()->markTypeId("error");
	int warningMarkID = QLineMarksInfoCenter::instance()->markTypeId("warning");
	int badboxMarkID = QLineMarksInfoCenter::instance()->markTypeId("badbox");
	if (mark != errorMarkID && mark != warningMarkID && mark != badboxMarkID) return;
	QList<int> errors = lineToLogEntries.values(editor->document()->line(line).handle());
	if (!errors.isEmpty())
		emit showMarkTooltipForLogMessage(errors);
}

void LatexEditorView::clearOverlays()
{
	for (int i = 0; i < editor->document()->lineCount(); i++) {
		QDocumentLine line = editor->document()->line(i);
		if (!line.isValid()) continue;

		//remove all overlays used for latex things, in descending frequency
		line.clearOverlays(SpellerUtility::spellcheckErrorFormat);
		line.clearOverlays(referencePresentFormat);
		line.clearOverlays(citationPresentFormat);
		line.clearOverlays(referenceMissingFormat);
		line.clearOverlays(referenceMultipleFormat);
		line.clearOverlays(citationMissingFormat);
		line.clearOverlays(environmentFormat);
		line.clearOverlays(syntaxErrorFormat);
		line.clearOverlays(structureFormat);
		line.clearOverlays(todoFormat);
		foreach (const int f, grammarFormats)
			line.clearOverlays(f);
	}
}

/*!
	Will be called from certain events, that should maybe result in opening the
	completer. Since the completer depends on the context, the caller doesn't
	have to be sure that a completer is really necassary. The context is checked
	in this function and an appropriate completer is opened if necessary.
	For example, typing a colon within a citation should start the completer.
	Therefore, typing a colon will trigger this function. It's checked in here
	if the context is a citation.
 */
void LatexEditorView::mayNeedToOpenCompleter(bool fromSingleChar)
{
	QDocumentCursor c = editor->cursor();
	QDocumentLineHandle *dlh = c.line().handle();
	if (!dlh)
		return;
	TokenStack ts = Parsing::getContext(dlh, c.columnNumber());
	Token tk;
	if (!ts.isEmpty()) {
		tk = ts.top();
        if(fromSingleChar && tk.length!=1){
            return; // only open completer on first char
        }
		if (tk.type == Token::word && tk.subtype == Token::none && ts.size() > 1) {
			// set brace type
			ts.pop();
			tk = ts.top();
		}
    }else{
        return; // no context available
    }

	Token::TokenType type = tk.type;
	if (tk.subtype != Token::none)
		type = tk.subtype;

	QList<Token::TokenType> lst;
	lst << Token::package << Token::keyValArg << Token::keyVal_val << Token::keyVal_key << Token::bibItem << Token::labelRefList;
    if(fromSingleChar){
        lst << Token::labelRef;
    }
	if (lst.contains(type))
		emit openCompleter();
    if (ts.isEmpty() || fromSingleChar)
		return;
	ts.pop();
	if (!ts.isEmpty()) { // check next level if 1. check fails (e.g. key vals are set to real value)
		tk = ts.top();
		type = tk.type;
		if (lst.contains(type))
			emit openCompleter();
	}
}

void LatexEditorView::documentContentChanged(int linenr, int count)
{
	Q_ASSERT(editor);
	QDocumentLine startline = editor->document()->line(linenr);
	if ((linenr >= 0 || count < editor->document()->lines()) && editor->cursor().isValid() &&
	        !editor->cursor().atLineStart() && editor->cursor().line().text().trimmed().length() > 0 &&
	        startline.isValid()) {
		bool add = false;
		if (changePositions.size() <= 0) add = true;
		else if (curChangePos < 1) {
			if (changePositions.first().dlh() != startline.handle()) add = true;
			else changePositions.first().setColumnNumber(editor->cursor().columnNumber());
		} else if (curChangePos >= changePositions.size() - 1) {
			if (changePositions.last().dlh() != startline.handle()) add = true;
			else changePositions.last().setColumnNumber(editor->cursor().columnNumber());
		}  else if (changePositions[curChangePos].dlh() == startline.handle()) changePositions[curChangePos].setColumnNumber(editor->cursor().columnNumber());
		else if (changePositions[curChangePos + 1].dlh() == startline.handle()) changePositions[curChangePos + 1].setColumnNumber(editor->cursor().columnNumber());
		else add = true;
		if (add) {
			curChangePos = -1;
			changePositions.insert(0, CursorPosition(editor->document()->cursor(linenr, editor->cursor().columnNumber())));
			if (changePositions.size() > 20) changePositions.removeLast();
		}
	}

	if (autoPreviewCursor.size() > 0) {
		for (int i = 0; i < autoPreviewCursor.size(); i++) {
			const QDocumentCursor &c = autoPreviewCursor[i];
			if (c.lineNumber() >= linenr && c.anchorLineNumber() < linenr + count)
				emit showPreview(c); //may modify autoPreviewCursor
		}

	}


	// checking
	if (!QDocument::defaultFormatScheme()) return;
	const LatexDocument *ldoc = qobject_cast<const LatexDocument *>(editor->document());
	bool latexLikeChecking = ldoc && ldoc->languageIsLatexLike();
	if (latexLikeChecking && config->fullCompilePreview) emit showFullPreview();
	if (!config->realtimeChecking) return; //disable all => implicit disable environment color correction (optimization)
	if (!latexLikeChecking && !config->inlineCheckNonTeXFiles) return;

    //checkGrammar(linenr,count);

    for (int i = linenr; i < linenr + count; i++) {
		QDocumentLine line = editor->document()->line(i);
        if (!line.isValid()) continue;

        //remove all overlays used for latex things, in descending frequency
        line.clearOverlays(formatsList); //faster as it avoids multiple lock/unlock operations

		bool addedOverlayReference = false;
		bool addedOverlayCitation = false;
		bool addedOverlayEnvironment = false;
		bool addedOverlayStructure = false;
		bool addedOverlayTodo = false;
		bool addedOverlayPackage = false;

		// diff presentation
		QVariant cookie = line.getCookie(QDocumentLine::DIFF_LIST_COOCKIE);
		if (!cookie.isNull()) {
			DiffList diffList = cookie.value<DiffList>();
			for (int i = 0; i < diffList.size(); i++) {
				DiffOp op = diffList.at(i);
				switch (op.type) {
				case DiffOp::Delete:
					line.addOverlay(QFormatRange(op.start, op.length, deleteFormat));
					break;
				case DiffOp::Insert:
					line.addOverlay(QFormatRange(op.start, op.length, insertFormat));
					break;
				case DiffOp::Replace:
					line.addOverlay(QFormatRange(op.start, op.length, replaceFormat));
					break;
				default:
					;
				}

			}
		}

        QDocumentLineHandle *dlh = line.handle();
        // handle % TODO
        QPair<int,int> commentStart = dlh->getCookieLocked(QDocumentLine::LEXER_COMMENTSTART_COOKIE).value<QPair<int,int> >();
        if(commentStart.second==Token::todoComment){
            int col=commentStart.first;
            QString curLine=line.text();
            QString text = curLine.mid(col);
            QString regularExpression=ConfigManagerInterface::getInstance()->getOption("Editor/todo comment regExp").toString();
            QRegularExpression rx(regularExpression);
            if (text.indexOf(rx) == 0) {
                line.addOverlay(QFormatRange(col, text.length(), todoFormat));
                addedOverlayTodo = true;
            }
        }

		// alternative context detection
        TokenList tl = dlh->getCookieLocked(QDocumentLine::LEXER_COOKIE).value<TokenList>();
		for (int tkNr = 0; tkNr < tl.length(); tkNr++) {
			Token tk = tl.at(tkNr);
			if (tk.subtype == Token::verbatim)
				continue;
			if (tk.type == Token::comment)
				break;
			if (latexLikeChecking) {
                if ((tk.subtype == Token::title || tk.subtype == Token::shorttitle) && (tk.type == Token::braces || tk.type == Token::openBrace)) {
					line.addOverlay(QFormatRange(tk.innerStart(), tk.innerLength(), structureFormat));
					addedOverlayStructure = true;
				}
				if (tk.subtype == Token::todo && (tk.type == Token::braces || tk.type == Token::openBrace)) {
					line.addOverlay(QFormatRange(tk.innerStart(), tk.innerLength(), todoFormat));
					addedOverlayTodo = true;
				}
				if (tk.type == Token::env || tk.type == Token::beginEnv) {
					line.addOverlay(QFormatRange(tk.start, tk.length, environmentFormat));
					addedOverlayEnvironment = true;
				}
				if ((tk.type == Token::package || tk.type == Token::beamertheme || tk.type == Token::documentclass) && config->inlinePackageChecking) {
					// package
					QString preambel;
					if (tk.type == Token::beamertheme) { // special treatment for  \usetheme
						preambel = "beamertheme";
					}
                    const QString text = dlh->text();
                    const QString rpck =  trimLeft(text.mid(tk.start, tk.length)); // left spaces are ignored by \cite, right space not
                    const QString suffix = tk.type == Token::documentclass ? ".cls" : ".sty";
					//check and highlight
                    bool localPackage = false;
                    if(rpck.startsWith(".")){
                        // check if file exists
                        LatexDocument *root=document->getRootDocument();
                        QFileInfo fi=root->getFileInfo();
                        QFileInfo fi_cwl=QFileInfo(fi.absolutePath(),rpck+suffix);
                        localPackage=fi_cwl.exists();
                    }
                    if (latexPackageList->empty())
						dlh->addOverlay(QFormatRange(tk.start, tk.length, packageUndefinedFormat));
                    else if ( (latexPackageList->find(preambel + rpck + suffix) != latexPackageList->end())
                              || (latexPackageList->find(preambel + rpck) != latexPackageList->end())
                              || localPackage) {
						dlh->addOverlay(QFormatRange(tk.start, tk.length, packagePresentFormat));
                    } else {
                        dlh->addOverlay(QFormatRange(tk.start, tk.length, packageMissingFormat));
                    }

					addedOverlayPackage = true;
				}
				if ((tk.type == Token::labelRef || tk.type == Token::labelRefList) && config->inlineReferenceChecking) {
					QDocumentLineHandle *dlh = tk.dlh;
					QString ref = dlh->text().mid(tk.start, tk.length);
					if (ref.contains('#')) continue;  // don't highlight refs in definitions e.g. in \newcommand*{\FigRef}[1]{figure~\ref{#1}}
					int cnt = document->countLabels(ref);
					if (cnt > 1) {
						dlh->addOverlay(QFormatRange(tk.start, tk.length, referenceMultipleFormat));
					} else if (cnt == 1) dlh->addOverlay(QFormatRange(tk.start, tk.length, referencePresentFormat));
					else dlh->addOverlay(QFormatRange(tk.start, tk.length, referenceMissingFormat));
					addedOverlayReference = true;
				}
				if (tk.type == Token::label && config->inlineReferenceChecking) {
					QDocumentLineHandle *dlh = tk.dlh;
					QString ref = dlh->text().mid(tk.start, tk.length);
					int cnt = document->countLabels(ref);
					if (cnt > 1) {
						dlh->addOverlay(QFormatRange(tk.start, tk.length, referenceMultipleFormat));
					} else dlh->addOverlay(QFormatRange(tk.start, tk.length, referencePresentFormat));
                    // look for corresponding references and adapt format respectively
                    document->updateRefsLabels(ref);
					addedOverlayReference = true;
				}
				if (tk.type == Token::bibItem && config->inlineCitationChecking) {
					QDocumentLineHandle *dlh = tk.dlh;
					QString text = dlh->text().mid(tk.start, tk.length);
					if (text.contains('#')) continue; // don't highlight cite in definitions e.g. in \newcommand*{\MyCite}[1]{see~\cite{#1}}
					QString rcit =  trimLeft(text); // left spaces are ignored by \cite, right space not
					//check and highlight
					if (document->bibIdValid(rcit))
						dlh->addOverlay(QFormatRange(tk.start, tk.length, citationPresentFormat));
					else
						dlh->addOverlay(QFormatRange(tk.start, tk.length, citationMissingFormat));
					addedOverlayCitation = true;
				}
			}// if latexLineCheking
		} // for Tokenslist

		//update wrapping if the an overlay changed the width of the text
		//TODO: should be handled by qce to be consistent (with syntax check and search)
		if (!editor->document()->getFixedPitch() && editor->flag(QEditor::LineWrap)) {
			bool updateWrapping = false;
			QFormatScheme *ff = QDocument::defaultFormatScheme();
			REQUIRE(ff);
			updateWrapping |= addedOverlayReference && (ff->format(referenceMissingFormat).widthChanging() || ff->format(referencePresentFormat).widthChanging() || ff->format(referenceMultipleFormat).widthChanging());
			updateWrapping |= addedOverlayCitation && (ff->format(citationPresentFormat).widthChanging() || ff->format(citationMissingFormat).widthChanging());
			updateWrapping |= addedOverlayPackage && (ff->format(packagePresentFormat).widthChanging() || ff->format(packageMissingFormat).widthChanging());
			updateWrapping |= addedOverlayEnvironment && ff->format(environmentFormat).widthChanging();
			updateWrapping |= addedOverlayStructure && ff->format(structureFormat).widthChanging();
			updateWrapping |= addedOverlayTodo && ff->format(todoFormat).widthChanging();
			if (updateWrapping)
				line.handle()->updateWrapAndNotifyDocument(i);

		}
	}
    editor->document()->markViewDirty();
}
/*!
 * \brief Force rechecking of syntax/spelling
 * \param linenr Starting line
 * \param count Number of lines, -1 -> until end
 */
void LatexEditorView::reCheckSyntax(int linenr, int count)
{
    document->reCheckSyntax(linenr,count);
}
/*!
 * \brief collect and prepare text for grammar check
 * \param linenr Starting line
 * \param count Number of lines
 */
void LatexEditorView::checkGrammar(int linenr, int count)
{
    if (config->inlineGrammarChecking) {
        QList<LineInfo> changedLines;
        int lookBehind = 0;
        for (; linenr - lookBehind >= 0; lookBehind++)
            if (editor->document()->line(linenr - lookBehind).firstChar() == -1) break;
        if (lookBehind > 0) lookBehind--;
        if (lookBehind > linenr) lookBehind = linenr;

        changedLines.reserve(linenr + count + lookBehind + 1);

        int truefirst = linenr - lookBehind;
        for (int i = linenr - lookBehind; i < editor->document()->lineCount(); i++) {
            QDocumentLine line = editor->document()->line(i);
            if (!line.isValid()) break;
            changedLines << LineInfo(line.handle());
            if (line.firstChar() == -1) {
                emit linesChanged(speller ? speller->name() : "<none>", document, changedLines, truefirst);
                truefirst += changedLines.size();
                changedLines.clear();
                if (i >= linenr + count) break;
            }
        }
        if (!changedLines.isEmpty())
            emit linesChanged(speller ? speller->name() : "<none>", document, changedLines, truefirst);

    }
}

void LatexEditorView::lineDeleted(QDocumentLineHandle *l,int)
{
    QMultiHash<QDocumentLineHandle *, int>::iterator it;
	while ((it = lineToLogEntries.find(l)) != lineToLogEntries.end()) {
		logEntryToLine.remove(it.value());
		lineToLogEntries.erase(it);
	}

	for (int i = changePositions.size() - 1; i >= 0; i--)
		if (changePositions[i].dlh() == l)
			changePositions.removeAt(i);

	emit lineHandleDeleted(l);
	editor->document()->markViewDirty();
}
void LatexEditorView::textReplaceFromAction()
{
	QAction *action = qobject_cast<QAction *>(QObject::sender());
	if (editor && action) {
		QString replacementText = action->data().toString();
        editor->setCursor(wordSelection);
		if (replacementText.isEmpty()) editor->cursor().removeSelectedText();
		else editor->write(replacementText);
		editor->setCursor(editor->cursor()); //to remove selection range
        wordSelection=QDocumentCursor();
	}
}
/*!
 * \brief add word to ignore file
 */
void LatexEditorView::spellCheckingAddToDict()
{
    if (speller && editor && defaultInputBinding && wordSelection.selectedText() == defaultInputBinding->lastSpellCheckedWord) {
        QString newToIgnore = wordSelection.selectedText();
        speller->addToIgnoreList(newToIgnore);
    }
}
/*!
 * \brief add word to ignore list but not into file
 * Volatile addition.
 */
void LatexEditorView::spellCheckingIgnoreAll()
{
    if (speller && editor && defaultInputBinding && wordSelection.selectedText() == defaultInputBinding->lastSpellCheckedWord) {
        QString newToIgnore = wordSelection.selectedText();
        speller->addToIgnoreList(newToIgnore,false);
    }
}

void LatexEditorView::addReplaceActions(QMenu *menu, const QStringList &replacements, bool italic)
{
	if (!menu) return;
    QAction *before = nullptr;
    if (!menu->actions().isEmpty()) before = menu->actions().constFirst();

	foreach (const QString &text, replacements) {
		QAction *replaceAction = new QAction(this);
		if (text.isEmpty()) {
			replaceAction->setText(tr("Delete"));
			QFont deleteFont;
			deleteFont.setItalic(italic);
			replaceAction->setFont(deleteFont);
		} else {
			replaceAction->setText(text);
			QFont correctionFont;
			correctionFont.setBold(true);
			correctionFont.setItalic(italic);
			replaceAction->setFont(correctionFont);
		}
		replaceAction->setData(text);
		connect(replaceAction, SIGNAL(triggered()), this, SLOT(textReplaceFromAction()));
		menu->insertAction(before, replaceAction);
	}
}

void LatexEditorView::populateSpellingMenu()
{
	QMenu *menu = qobject_cast<QMenu *>(sender());
	if (!menu) return;
	QString word = menu->property("word").toString();
	if (word.isEmpty()) return;
	addSpellingActions(menu, word, true);
}

void LatexEditorView::addSpellingActions(QMenu *menu, QString word, bool dedicatedMenu)
{
	if (menu->property("isSpellingPopulated").toBool()) return;

	QStringList suggestions = speller->suggest(word);
	addReplaceActions(menu, suggestions, false);

	QAction *act = new QAction(LatexEditorView::tr("Add to Dictionary"), menu);
    connect(act, &QAction::triggered, this, &LatexEditorView::spellCheckingAddToDict);
    QAction *act2 = new QAction(LatexEditorView::tr("Ignore all"), menu);
    connect(act2, &QAction::triggered, this, &LatexEditorView::spellCheckingIgnoreAll);
	if (dedicatedMenu) {
		menu->addSeparator();
	} else {
		QFont ignoreFont;
		ignoreFont.setItalic(true);
		act->setFont(ignoreFont);
        act2->setFont(ignoreFont);
	}
	menu->addAction(act);
    menu->addAction(act2);
	menu->setProperty("isSpellingPopulated", true);
}

void LatexEditorView::spellRemoveMarkers(const QString &newIgnoredWord)
{
	REQUIRE(editor);
	QDocument* doc = editor->document();
	if (!doc) return;
    QString newUpperIgnoredWord=newIgnoredWord; //remove upper letter start as well
    if(!newUpperIgnoredWord.isEmpty()){
        newUpperIgnoredWord[0]=newUpperIgnoredWord[0].toUpper();
    }
	//documentContentChanged(editor->cursor().lineNumber(),1);
	for (int i = 0; i < doc->lines(); i++) {
		QList<QFormatRange> li = doc->line(i).getOverlays(SpellerUtility::spellcheckErrorFormat);
		QString curLineText = doc->line(i).text();
		for (int j = 0; j < li.size(); j++)
            if (latexToPlainWord(curLineText.mid(li[j].offset, li[j].length)) == newIgnoredWord || latexToPlainWord(curLineText.mid(li[j].offset, li[j].length)) == newUpperIgnoredWord) {
				doc->line(i).removeOverlay(li[j]);
				doc->line(i).setFlag(QDocumentLine::LayoutDirty, true);
			}
	}
	editor->viewport()->update();
}

void LatexEditorView::closeCompleter()
{
	completer->close();
}

/*
 * Extracts the math formula at the given cursor position including math delimiters.
 * Current limitations: the cursor needs to be on one of the delimiters. This does
 * not work for math environments
 * Returns an empty string if there is no math formula.
 */
QString LatexEditorView::extractMath(QDocumentCursor cursor)
{
	if (cursor.line().getFormatAt(cursor.columnNumber()) != math_DelimiterFormat)
		return QString();
	int col = cursor.columnNumber();
	while (col > 0 && cursor.line().getFormatAt(col - 1) == math_DelimiterFormat) col--;
	cursor.setColumnNumber(col);
	return parenthizedTextSelection(cursor).selectedText();
}

bool LatexEditorView::moveToCommandStart (QDocumentCursor &cursor, QString commandPrefix)
{
	QString line = cursor.line().text();
	int lastOffset = cursor.columnNumber();
	if (lastOffset >= line.length()) {
		lastOffset = -1;
	}
	int foundOffset = line.lastIndexOf(commandPrefix, lastOffset);
	if (foundOffset == -1) {
		return false;
	}
	cursor.moveTo(cursor.lineNumber(), foundOffset);
	return true;
}

QString LatexEditorView::findEnclosedMathText(QDocumentCursor cursor, QString command){
    QString text;
    QFormatRange fr = cursor.line().getOverlayAt(cursor.columnNumber(), numbersFormat);
    if(fr.isValid()){
        // end found
        // test if start is in the same line
        if(fr.offset>0){
            // yes
            text=cursor.line().text().mid(fr.offset,fr.length);
        }else{
            //start in previous lines
            StackEnvironment env;
            document->getEnv(cursor.lineNumber(), env);
            if(!env.isEmpty() && env.top().name=="math"){
                cursor.moveTo(document->indexOf(env.top().dlh,cursor.lineNumber()),env.top().startingColumn,QDocumentCursor::KeepAnchor);
                text=cursor.selectedText();
            }
        }
    }else{
        // try again at end of command ($/$$)
        fr = cursor.line().getOverlayAt(cursor.columnNumber()+command.length(), numbersFormat);
        if(fr.isValid()){
            if(fr.offset+fr.length<cursor.line().length()){
                // within current line
                text=cursor.line().text().mid(fr.offset,fr.length);
            }else{
                // exceeds current line
                cursor.movePosition(command.length());
                int ln=cursor.lineNumber();
                StackEnvironment env;
                document->getEnv(ln+1, env);
                while(true){
                    ++ln;
                    QDocumentLine dln=document->line(ln);
                    if(dln.isValid()){
                        StackEnvironment envNext;
                        document->getEnv(ln+1, envNext);
                        if(env==envNext)
                            continue;
                        QFormatRange fr2 = dln.getOverlayAt(0, numbersFormat);
                        cursor.moveTo(ln,fr2.length,QDocumentCursor::KeepAnchor);
                        text=cursor.selectedText();
                        break;
                    }else{
                        QDocumentLine dln=document->line(ln-1);
                        cursor.moveTo(ln-1,dln.length(),QDocumentCursor::KeepAnchor);
                        text=cursor.selectedText();
                        break;
                    }
                }
            }
        }
    }
    return text;
}

bool LatexEditorView::showMathEnvPreview(QDocumentCursor cursor, QString command, QString environment, QPoint pos)
{
    QStringList envAliases = document->lp->environmentAliases.values(environment);
	bool found;
    QString text;
    if (((command == "\\begin" || command == "\\end") && envAliases.contains("math")) || command == "\\[" || command == "\\]" || command == "\\(" || command == "\\)") {
		found = moveToCommandStart(cursor, "\\");
	} else if (command == "$" || command == "$$") {
		found = moveToCommandStart(cursor, command);
        // special treatment for $/$$ as it is handled in syntax checker
        text="$"+findEnclosedMathText(cursor,command)+"$";
	} else {
		found = false;
	}
	if (!found) {
		QToolTip::hideText();
		return false;
	}
    text = text.isEmpty() ? parenthizedTextSelection(cursor).selectedText() : text;
	if (text.isEmpty()) {
		QToolTip::hideText();
		return false;
	}
	m_point = editor->mapToGlobal(editor->mapFromFrame(pos));
	emit showPreview(text);
	return true;
}

void LatexEditorView::mouseHovered(QPoint pos)
{
	// reimplement to what is necessary

	if (pos.x() < 0) return; // hover event on panel
	QDocumentCursor cursor;
    cursor = editor->cursorForPosition(editor->mapToContents(pos),true);
	QString line = cursor.line().text();
	QDocumentLine l = cursor.line();

	QFormatRange fr = cursor.line().getOverlayAt(cursor.columnNumber(), replaceFormat);
	if (fr.length > 0 && fr.format == replaceFormat) {
		QVariant var = l.getCookie(QDocumentLine::DIFF_LIST_COOCKIE);
		if (var.isValid()) {
			DiffList diffList = var.value<DiffList>();
			DiffOp op;
			op.start = -1;
			foreach (op, diffList) {
				if (op.start <= cursor.columnNumber() && op.start + op.length >= cursor.columnNumber()) {
					break;
				}
				op.start = -1;
			}

			if (op.start >= 0 && !op.text.isEmpty()) {
				QString message = op.text;
				QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), message);
				return;
			}
		}
	}
	foreach (const int f, grammarFormats) {
		fr = cursor.line().getOverlayAt(cursor.columnNumber(), f);
		if (fr.length > 0 && fr.format == f) {
			QVariant var = l.getCookie(QDocumentLine::GRAMMAR_ERROR_COOKIE);
			if (var.isValid()) {
				const QList<GrammarError> &errors = var.value<QList<GrammarError> >();
				for (int i = 0; i < errors.size(); i++)
					if (errors[i].offset <= cursor.columnNumber() && errors[i].offset + errors[i].length >= cursor.columnNumber()) {
						QString message = errors[i].message;
						QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), message);
						return;
					}
			}
		}
	}
	// check for latex error
	//syntax checking
	fr = cursor.line().getOverlayAt(cursor.columnNumber(), syntaxErrorFormat);
	if (fr.length > 0 && fr.format == syntaxErrorFormat) {
		StackEnvironment env;
		document->getEnv(cursor.lineNumber(), env);
		TokenStack remainder;
		int i = cursor.lineNumber();
		if (document->line(i - 1).handle())
			remainder = document->line(i - 1).handle()->getCookieLocked(QDocumentLine::LEXER_REMAINDER_COOKIE).value<TokenStack >();
		QString text = l.text();
		if (!text.isEmpty()) {
			QString message = document->getErrorAt(l.handle(), cursor.columnNumber(), env, remainder);
			QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), message);
			return;
		}
	}
	// new way
	QDocumentLineHandle *dlh = cursor.line().handle();

	TokenList tl = dlh ? dlh->getCookieLocked(QDocumentLine::LEXER_COOKIE).value<TokenList>() : TokenList();

	//Tokens tk=getTokenAtCol(dlh,cursor.columnNumber());
	TokenStack ts = Parsing::getContext(dlh, cursor.columnNumber());
	Token tk;
	if (!ts.isEmpty()) {
		tk = ts.top();
	}

	LatexParser &lp = LatexParser::getInstance();
	QString command, value;
	bool handled = false;
	if (tk.type != Token::none) {
		int tkPos = tl.indexOf(tk);
		if (tk.type == Token::command || tk.type == Token::commandUnknown) {
			handled = true;
			command = line.mid(tk.start, tk.length);
			CommandDescription cd = lp.commandDefs.value(command);
            if (cd.args() > 0)
				value = Parsing::getArg(tl.mid(tkPos + 1), dlh, 0, ArgumentList::Mandatory);
			if (config->toolTipPreview && showMathEnvPreview(cursor, command, value, pos)) {
                // action is already performed as a side effect
			} else if (config->toolTipHelp && completer->getLatexReference()) {
				QString topic = completer->getLatexReference()->getTextForTooltip(command);
				if (!topic.isEmpty()) QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), topic);
			}
		}
		value = tk.getText();
		if (tk.type == Token::env || tk.type == Token::beginEnv) {
			handled = true;
			if (config->toolTipPreview && showMathEnvPreview(cursor, "\\begin", value, pos)) {
                // action is already performed as a side effect
			} else if (config->toolTipHelp && completer->getLatexReference()) {
				QString topic = completer->getLatexReference()->getTextForTooltip("\\begin{" + value);
				if (!topic.isEmpty()) QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), topic);
			}
		}
		if (tk.type == Token::labelRef || tk.type == Token::labelRefList) {
			handled = true;
			int cnt = document->countLabels(value);
			QString mText = "";
			if (cnt == 0) {
				mText = tr("label missing!");
			} else if (cnt > 1) {
				mText = tr("label defined multiple times!");
			} else {
				QMultiHash<QDocumentLineHandle *, int> result = document->getLabels(value);
                if(!result.isEmpty()){
                    QDocumentLineHandle *mLine = result.keys().constFirst();
                    int l = mLine->document()->indexOf(mLine);
                    LatexDocument *doc = qobject_cast<LatexDocument *> (editor->document());
                    if (mLine->document() != editor->document()) {
                        doc = document->parent->findDocument(mLine->document());
                        if (doc) mText = tr("<p style='white-space:pre'><b>Filename: %1</b>\n").arg(doc->getFileName());
                    }
                    if (doc)
                        mText += doc->exportAsHtml(doc->cursor(qMax(0, l - 2), 0, l + 2), true, true, 60);
                }
			}
			QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), mText);
		}
		if (tk.type == Token::label) {
			handled = true;
			if (document->countLabels(value) > 1) {
				QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), tr("label defined multiple times!"));
			} else {
				int cnt = document->countRefs(value);
				QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), tr("%n reference(s) to this label", "", cnt));
			}
		}
		if (tk.type == Token::package || tk.type == Token::beamertheme || tk.type == Token::documentclass) {
			handled = true;
			QString type = (tk.type == Token::documentclass) ? tr("Class") : tr("Package");
			QString preambel;
			if (tk.type == Token::beamertheme) { // special treatment for  \usetheme
				preambel = "beamertheme";
				type = tr("Beamer Theme");
				type.replace(' ', "&nbsp;");
			}
            QString text = QString("%1:&nbsp;<b>%2</b>").arg(type,value);
            const QString suffix = tk.type == Token::documentclass ? ".cls" : ".sty";
            if (latexPackageList->find(preambel + value + suffix) != latexPackageList->end()
                || latexPackageList->find(preambel + value) != latexPackageList->end()
                || value.startsWith(".")) { // don't check relative paths,i.e. local packages
				QString description = LatexRepository::instance()->shortDescription(value);
				if (!description.isEmpty()) text += "<br>" + description;
				QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), text);
			} else {
				text += "<br><b>(" + tr("not found") + ")";
				QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), text);
			}
		}
		if (config->imageToolTip && tk.subtype == Token::color) {
            handled=true;
            QString text;
            if (ts.size() > 1 && tk.type==Token::word) {
                ts.pop();
                tk = ts.top();
            }
            text = QString("\\noindent{\\color%1 \\rule{1cm}{1cm} }").arg(tk.getText());
			m_point = editor->mapToGlobal(editor->mapFromFrame(pos));
			emit showPreview(text);
		}
		if (tk.type == Token::bibItem) {
			handled = true;
			QString tooltip(tr("Citation correct (reading ...)"));
			QString bibID;

			bibID = value;

			if (!document->bibIdValid(bibID)) {
				tooltip = "<b>" + tr("Citation missing") + ":</b> " + bibID;

				if (!bibID.isEmpty() && bibID[bibID.length() - 1].isSpace()) {
					tooltip.append("<br><br><i>" + tr("Warning:") + "</i> " + tr("BibTeX ID ends with space. Trailing spaces are not ignored by BibTeX."));
				}
			} else {
				if (document->isBibItem(bibID)) {
					// by bibitem defined citation
					tooltip.clear();
					QMultiHash<QDocumentLineHandle *, int> result = document->getBibItems(bibID);
                    if (result.isEmpty())
						return;
                    QDocumentLineHandle *mLine = result.keys().constFirst();
					if (!mLine)
						return;
					int l = mLine->document()->indexOf(mLine);
					LatexDocument *doc = qobject_cast<LatexDocument *> (editor->document());
					if (mLine->document() != editor->document()) {
						doc = document->parent->findDocument(mLine->document());
						if (doc) tooltip = tr("<p style='white-space:pre'><b>Filename: %1</b>\n").arg(doc->getFileName());
					}
					if (doc)
						tooltip += doc->exportAsHtml(doc->cursor(l, 0, l + 4), true, true, 60);
				} else {
					// read entry in bibtex file
					if (!bibReader) {
						bibReader = new bibtexReader(this);
						connect(bibReader, SIGNAL(sectionFound(QString)), this, SLOT(bibtexSectionFound(QString)));
                        connect(this, SIGNAL(searchBibtexSection(QString,QString)), bibReader, SLOT(searchSection(QString,QString)));
						bibReader->start(); //The thread is started, but it is doing absolutely nothing! Signals/slots called in the thread object are execute in the emitting thread, not the thread itself.  TODO: fix
					}
					QString file = document->findFileFromBibId(bibID);
					lastPos = pos;
					if (!file.isEmpty())
						emit searchBibtexSection(file, bibID);
					return;
				}
			}
			QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), tooltip);
		}
		if (tk.type == Token::imagefile && config->imageToolTip) {
			handled = true;
			QStringList imageExtensions = QStringList() << "" << "png" << "pdf" << "jpg" << "jpeg";
			QString fname;
			QFileInfo fi;
			QStringList imagePaths = ConfigManagerInterface::getInstance()->getOption("Files/Image Paths").toString().split(getPathListSeparator());
			foreach (const QString &ext, imageExtensions) {
				fname = getDocument()->getAbsoluteFilePath(value, ext, imagePaths);
				fi.setFile(fname);
				if (fi.exists()) break;
			}
			if (!fi.exists()) return;
			m_point = editor->mapToGlobal(editor->mapFromFrame(pos));
			emit showImgPreview(fname);
		}
        if(tk.type>=Token::specialArg){
            QString mText;
            LatexDocument *doc = qobject_cast<LatexDocument *> (editor->document());
            QString def=doc->lp->mapSpecialArgs.value(tk.type-Token::specialArg);
            QDocumentLineHandle *target = doc->findCommandDefinition(def+"%"+tk.getText());
            if (target) {
                int l = target->document()->indexOf(target);
                if (target->document() != editor->document()) {
                    doc = document->parent->findDocument(target->document());
                    if (doc) mText = tr("<p style='white-space:pre'><b>Filename: %1</b>\n").arg(doc->getFileName());
                }
                if (doc)
                    mText += doc->exportAsHtml(doc->cursor(qMax(0, l - 2), 0, l + 2), true, true, 60);
                QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(pos)), mText);
                handled=true;
            }
        }

	}//if tk
	if (handled)
		return;

    QToolTip::hideText();
}

bool LatexEditorView::closeElement()
{
    if (vimPromptPanel && vimPromptPanel->isVisible()) {
        vimPromptPanel->closePrompt();
        return true;
    }
	if (completer->close()) return true;
	if (gotoLinePanel->isVisible()) {
		gotoLinePanel->hide();
		editor->setFocus();
		return true;
	}
	if (searchReplacePanel->isVisible()) {
		searchReplacePanel->closeElement(config->closeSearchAndReplace);
		return true;
	}
    if (config->editingMode == LatexEditorViewConfig::VimEditing && editor) {
        QWidget *focus = QApplication::focusWidget();
        const bool editorFocused = editor->hasFocus() || focus == editor || (focus && editor->isAncestorOf(focus));
        if (editorFocused) {
            if (vimInputBinding && vimInputBinding->handleEscapeShortcut(editor))
                return true;
            return true;
        }
	}
	return false;
}

namespace {
static void skipVimSpaces(const QString &command, int &pos)
{
    while (pos < command.size() && command.at(pos).isSpace())
        ++pos;
}

static bool parseVimLineAddress(const LatexEditorView *view, const QString &command, int &pos, int &line)
{
    if (!view || !view->editor || !view->document || pos >= command.size())
        return false;

    const int lastLine = qMax(0, view->document->lineCount() - 1);
    if (command.at(pos) == QLatin1Char('.')) {
        line = view->editor->cursor().lineNumber();
        ++pos;
        return true;
    }
    if (command.at(pos) == QLatin1Char('$')) {
        line = lastLine;
        ++pos;
        return true;
    }
    if (!command.at(pos).isDigit())
        return false;

    const int start = pos;
    while (pos < command.size() && command.at(pos).isDigit())
        ++pos;

    bool ok = false;
    const int parsedLine = command.mid(start, pos - start).toInt(&ok);
    if (!ok)
        return false;

    line = qMax(0, qMin(lastLine, parsedLine - 1));
    return true;
}

static bool parseDelimitedVimSubstitutePart(const QString &command, int &pos, QChar delimiter, QString &part)
{
    part.clear();
    while (pos < command.size()) {
        const QChar ch = command.at(pos++);
        if (ch == QLatin1Char('\\') && pos < command.size()) {
            const QChar escaped = command.at(pos++);
            if (escaped == delimiter) {
                part += delimiter;
            } else {
                part += QLatin1Char('\\');
                part += escaped;
            }
            continue;
        }
        if (ch == delimiter)
            return true;
        part += ch;
    }
    return false;
}

static QString translateVimReplacement(const QString &replacement)
{
    QString translated;
    translated.reserve(replacement.size() + 4);

    bool escaped = false;
    for (const QChar ch : replacement) {
        if (!escaped && ch == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (escaped) {
            if (ch == QLatin1Char('&')) {
                translated += QLatin1Char('&');
            } else {
                translated += QLatin1Char('\\');
                translated += ch;
            }
            escaped = false;
            continue;
        }
        if (ch == QLatin1Char('&'))
            translated += QStringLiteral("\\0");
        else
            translated += ch;
    }

    if (escaped)
        translated += QLatin1Char('\\');
    return translated;
}

static VimSubstituteParseResult parseVimSubstituteCommand(const LatexEditorView *view, const QString &command, VimSubstituteCommand &result, QString &error)
{
    if (!view || !view->editor || !view->document)
        return VimSubstituteParseResult::NotSubstitute;

    int pos = 0;
    skipVimSpaces(command, pos);

    const int currentLine = view->editor->cursor().lineNumber();
    result.startLine = currentLine;
    result.endLine = currentLine;

    if (pos < command.size() && command.at(pos) == QLatin1Char('%')) {
        result.startLine = 0;
        result.endLine = qMax(0, view->document->lineCount() - 1);
        ++pos;
    } else {
        const int savedPos = pos;
        int firstLine = -1;
        if (parseVimLineAddress(view, command, pos, firstLine)) {
            int rangePos = pos;
            skipVimSpaces(command, rangePos);
            if (rangePos < command.size() && (command.at(rangePos) == QLatin1Char(',') || command.at(rangePos) == QLatin1Char(';'))) {
                ++rangePos;
                skipVimSpaces(command, rangePos);
                int secondLine = -1;
                if (!parseVimLineAddress(view, command, rangePos, secondLine)) {
                    error = QCoreApplication::translate("LatexEditorView", "Invalid Vim range.");
                    return VimSubstituteParseResult::Invalid;
                }
                result.startLine = firstLine;
                result.endLine = secondLine;
                pos = rangePos;
            } else if (rangePos < command.size() && command.at(rangePos) == QLatin1Char('s')) {
                result.startLine = firstLine;
                result.endLine = firstLine;
                pos = rangePos;
            } else {
                pos = savedPos;
            }
        }
    }

    skipVimSpaces(command, pos);
    if (pos >= command.size() || command.at(pos) != QLatin1Char('s'))
        return VimSubstituteParseResult::NotSubstitute;

    ++pos;
    if (pos >= command.size()) {
        error = QCoreApplication::translate("LatexEditorView", "Incomplete Vim substitute command.");
        return VimSubstituteParseResult::Invalid;
    }

    const QChar delimiter = command.at(pos++);
    if (delimiter.isSpace()) {
        error = QCoreApplication::translate("LatexEditorView", "Invalid Vim substitute delimiter.");
        return VimSubstituteParseResult::Invalid;
    }

    QString pattern;
    if (!parseDelimitedVimSubstitutePart(command, pos, delimiter, pattern)) {
        error = QCoreApplication::translate("LatexEditorView", "Unterminated Vim substitute pattern.");
        return VimSubstituteParseResult::Invalid;
    }

    QString replacement;
    if (!parseDelimitedVimSubstitutePart(command, pos, delimiter, replacement)) {
        error = QCoreApplication::translate("LatexEditorView", "Unterminated Vim substitute replacement.");
        return VimSubstituteParseResult::Invalid;
    }

    while (pos < command.size()) {
        const QChar flag = command.at(pos++);
        if (flag.isSpace()) {
            skipVimSpaces(command, pos);
            if (pos == command.size())
                break;
            error = QCoreApplication::translate("LatexEditorView", "Unexpected trailing text in Vim substitute command.");
            return VimSubstituteParseResult::Invalid;
        }

        switch (flag.unicode()) {
        case 'g':
            result.global = true;
            break;
        case 'c':
            result.confirm = true;
            break;
        case 'i':
            result.caseSensitive = false;
            break;
        case 'I':
            result.caseSensitive = true;
            break;
        default:
            error = QCoreApplication::translate("LatexEditorView", "Unsupported Vim substitute flag: %1").arg(flag);
            return VimSubstituteParseResult::Invalid;
        }
    }

    result.pattern = pattern;
    if (result.pattern.isEmpty())
        result.pattern = view->lastVimSearchText();
    if (result.pattern.isEmpty()) {
        error = QCoreApplication::translate("LatexEditorView", "No previous search pattern for Vim substitute.");
        return VimSubstituteParseResult::Invalid;
    }

    result.replacement = translateVimReplacement(replacement);
    return VimSubstituteParseResult::Parsed;
}

static bool executeVimSubstituteCommand(LatexEditorView *view, const VimSubstituteCommand &command, QString &error)
{
    if (!view || !view->editor || !view->document)
        return false;

    const int firstLine = qMax(0, qMin(command.startLine, command.endLine));
    const int lastLine = qMax(0, qMax(command.startLine, command.endLine));

    QDocumentSearch::Options options = QDocumentSearch::RegExp | QDocumentSearch::Replace | QDocumentSearch::Silent;
    if (command.caseSensitive)
        options |= QDocumentSearch::CaseSensitive;
    if (command.confirm)
        options |= QDocumentSearch::Prompt;

    QDocumentSearch search(view->editor, command.pattern, options, command.replacement);
    QDocumentCursor lastReplacement;
    int replacedCount = 0;

    for (int line = firstLine; line <= lastLine; ++line) {
        const int lineLength = view->document->line(line).length();
        if (lineLength == 0)
            continue;
        QDocumentCursor scope(view->document, line, 0, line, lineLength);
        search.setScope(scope);
        search.setCursor(QDocumentCursor());

        const int lineReplacements = search.next(false, command.global, false, false);
        replacedCount += lineReplacements;
        if (lineReplacements > 0 && search.lastReplacedPosition().isValid())
            lastReplacement = search.lastReplacedPosition().selectionStart();
    }

    if (replacedCount <= 0) {
        error = QCoreApplication::translate("LatexEditorView", "Pattern not found: %1").arg(command.pattern);
        return false;
    }

    view->recordVimSearchState(command.pattern, false);

    if (lastReplacement.isValid()) {
        lastReplacement.clearSelection();
        view->editor->setCursor(lastReplacement);
        view->editor->ensureCursorVisible(QEditor::Navigation);
    }
    return true;
}
}

bool LatexEditorView::executeVimExCommand(const QString &command)
{
    QString trimmed = command.trimmed();
    if (trimmed.startsWith(QLatin1Char(':')))
        trimmed.remove(0, 1);
    trimmed = trimmed.trimmed();
    const QString normalized = trimmed.toLower();

    if (trimmed.isEmpty())
        return true;

    bool ok = false;
    const int lineNumber = trimmed.toInt(&ok);
    if (ok) {
        QDocumentCursor cursor(document, qMax(0, qMin(document->lineCount() - 1, lineNumber - 1)), 0);
        editor->setCursor(cursor);
        editor->ensureCursorVisible(QEditor::Navigation);
        return true;
    }

    if (normalized == QLatin1String("noh") || normalized == QLatin1String("nohlsearch")) {
        clearVimSearchHighlight();
        return true;
    }

    VimSubstituteCommand substituteCommand;
    QString substituteError;
    switch (parseVimSubstituteCommand(this, trimmed, substituteCommand, substituteError)) {
    case VimSubstituteParseResult::Parsed:
        if (executeVimSubstituteCommand(this, substituteCommand, substituteError))
            return true;
        if (vimPromptPanel)
            vimPromptPanel->showError(substituteError);
        return false;
    case VimSubstituteParseResult::Invalid:
        if (vimPromptPanel)
            vimPromptPanel->showError(substituteError);
        return false;
    case VimSubstituteParseResult::NotSubstitute:
        break;
    }

    if (normalized == QLatin1String("w") || normalized == QLatin1String("write") ||
        normalized == QLatin1String("q") || normalized == QLatin1String("quit") ||
        normalized == QLatin1String("wq") || normalized == QLatin1String("x")) {
        QString dispatched = normalized;
        if (dispatched == QLatin1String("write"))
            dispatched = QStringLiteral("w");
        else if (dispatched == QLatin1String("quit"))
            dispatched = QStringLiteral("q");
        emit vimCommandRequested(dispatched);
        return true;
    }

    if (vimPromptPanel)
        vimPromptPanel->showError(tr("Unsupported Vim command: %1").arg(command));
    return false;
}

void LatexEditorView::executeVimSearch(const QString &text, bool backward)
{
    QString query = text;
    if (query.isEmpty() && vimInputBinding)
        query = vimInputBinding->lastSearchText();
    if (query.isEmpty())
        return;

    searchReplacePanel->find(query, backward, true, false, false, true, true, false);
    searchReplacePanel->hide();
    if (searchReplacePanelAction) {
        const bool wasBlocked = searchReplacePanelAction->blockSignals(true);
        searchReplacePanelAction->setChecked(false);
        searchReplacePanelAction->blockSignals(wasBlocked);
    }
    if (vimInputBinding)
        vimInputBinding->recordSearch(query, backward);
    if (config->editingMode == LatexEditorViewConfig::VimEditing)
        editor->setInputModeLabel(QStringLiteral("NORMAL"));
    editor->setFocus();
}

QString LatexEditorView::lastVimSearchText() const
{
    if (vimInputBinding && !vimInputBinding->lastSearchText().isEmpty())
        return vimInputBinding->lastSearchText();
    return searchReplacePanel ? searchReplacePanel->getSearchText() : QString();
}

void LatexEditorView::recordVimSearchState(const QString &text, bool backward)
{
    if (vimInputBinding)
        vimInputBinding->recordSearch(text, backward);
}

void LatexEditorView::clearVimSearchHighlight()
{
    if (searchReplacePanel && searchReplacePanel->search())
        searchReplacePanel->search()->setOption(QDocumentSearch::HighlightAll, false);
}

void LatexEditorView::rebindInputMode()
{
    if (!editor || !defaultInputBinding || !vimInputBinding)
        return;

    if (config->editingMode == LatexEditorViewConfig::VimEditing) {
        editor->setInputBinding(vimInputBinding);
        vimInputBinding->resetForEditor(editor);
    } else {
        if (vimPromptPanel)
            vimPromptPanel->closePrompt();
        editor->setInputBinding(defaultInputBinding);
        editor->setCursorStyle(QDocument::AutoCursorStyle);
        editor->setInputModeLabel(QString());
    }
}

void LatexEditorView::setVimPromptVisible(bool visible)
{
    if (vimPromptPanelAction) {
        vimPromptPanelAction->setChecked(visible);
    } else if (vimPromptPanel) {
        vimPromptPanel->setVisible(visible);
    }
}

void LatexEditorView::insertHardLineBreaks(int newLength, bool smartScopeSelection, bool joinLines)
{
    QRegularExpression breakChars("[ \t\n\r]");
	QDocumentCursor cur = editor->cursor();
	QDocument *doc = editor->document();
	int startLine = 0;
	int endLine = doc->lines() - 1;

	if (cur.isValid()) {
		if (cur.hasSelection()) {
			startLine = cur.selectionStart().lineNumber();
			endLine = cur.selectionEnd().lineNumber();
			if (cur.selectionEnd().columnNumber() == 0 && startLine < endLine) endLine--;
		} else if (smartScopeSelection) {
			QDocumentCursor currentCur = cur;
			QDocumentCursor lineCursor = currentCur;
			do {
				QString lineString  = lineCursor.line().text().trimmed();
				if ((lineString == QLatin1String("")) ||
				        (lineString.contains("\\begin")) ||
				        (lineString.contains("\\end")) ||
				        (lineString.contains("$$")) ||
				        (lineString.contains("\\[")) ||
				        (lineString.contains("\\]"))) {
					//qDebug() << lineString;
					break;
				}
			} while (lineCursor.movePosition(1, QDocumentCursor::Up, QDocumentCursor::MoveAnchor));
			startLine = lineCursor.lineNumber();
			if (lineCursor.atStart()) startLine--;

			lineCursor = currentCur;
			do {
				QString lineString  = lineCursor.line().text().trimmed();
				if ((lineString == QLatin1String("")) ||
				        (lineString.contains("\\begin")) ||
				        (lineString.contains("\\end")) ||
				        (lineString.contains("$$")) ||
				        (lineString.contains("\\[")) ||
				        (lineString.contains("\\]"))) {
					//qDebug() << lineString;
					break;
				}
			} while (lineCursor.movePosition(1, QDocumentCursor::Down, QDocumentCursor::MoveAnchor));
			endLine = lineCursor.lineNumber();
			if (lineCursor.atEnd()) endLine++	;

			if ((endLine - startLine) < 2) { // lines near, therefore no need to line break
				return ;
			}

			startLine++;
			endLine--;
		}
	}
	if (joinLines) { // start of smart formatting, similar to what emacs (AucTeX) can do, but much simple
		QStringList lines;
		for (int i = startLine; i <= endLine; i++)
			lines << doc->line(i).text();
		lines = joinLinesExceptCommentsAndEmptyLines(lines);
		lines = splitLines(lines, newLength, breakChars);

		QDocumentCursor vCur = doc->cursor(startLine, 0, endLine, doc->line(endLine).length());
		editor->insertText(vCur, lines.join("\n"));
		editor->setCursor(cur);
		return;
	}

	bool areThereLinesToBreak = false;
	for (int i = startLine; i <= endLine; i++)
		if (doc->line(i).length() > newLength) {
			areThereLinesToBreak = true;
			break;
		}
	if (!areThereLinesToBreak) return;
	//remove all lines and reinsert them wrapped
	if (endLine + 1 < doc->lines())
		cur = doc->cursor(startLine, 0, endLine + 1, 0); //+1,0);
	else
		cur = doc->cursor(startLine, 0, endLine, doc->line(endLine).length()); //+1,0);
	QStringList lines;
	for (int i = startLine; i <= endLine; i++)
		lines << doc->line(i).text();
	QString insertBlock;
	for (int i = 0; i < lines.count(); i++) {
		QString line = lines[i];
        int commentStartPos = commentStart(line);
        if (commentStartPos == -1) commentStartPos = line.length();
		while (line.length() > newLength) {
			int breakAt = line.lastIndexOf(breakChars, newLength);
			if (breakAt < 0) breakAt = line.indexOf(breakChars, newLength);
			if (breakAt < 0) break;
            if (breakAt >= commentStartPos && breakAt + 1 > newLength) {
				int newBreakAt = line.indexOf(breakChars, breakAt - 1);
				if (newBreakAt > -1) breakAt = newBreakAt;
			}
			insertBlock += line.left(breakAt) + "\n";
            if (breakAt < commentStartPos) {
				line = line.mid(breakAt + 1);
                commentStartPos -= breakAt + 1;
			} else {
				line = "%" + line.mid(breakAt + 1);
                commentStartPos = 0;
			}
		}
		insertBlock += line + "\n";
	}
	editor->insertText(cur, insertBlock);

	editor->setCursor(cur);
}

void LatexEditorView::sortSelectedLines(LineSorting sorting, Qt::CaseSensitivity caseSensitivity, bool completeLines, bool removeDuplicates){
	if (completeLines){
		editor->selectExpand(QDocumentCursor::LineUnderCursor);
	}
	QList<QDocumentCursor> cursors = editor->cursors();
	std::sort(cursors.begin(), cursors.end());
	for (int i=0; i < cursors.length(); i++)
		cursors[i].setAutoUpdated(true); //auto updating is not enabled by default (but it is supposed to be, isn't it?)

	QList<int> spannedLines;
	QStringList text;
	foreach (const QDocumentCursor& c, cursors) {
		QStringList selectedTextLines = c.selectedText().split('\n');
		spannedLines << selectedTextLines.length();
		text << selectedTextLines;
	}
	if (text.isEmpty()) return;
	bool additionalEmptyLine = text.last().isEmpty();
	if (additionalEmptyLine) text.removeLast();

	if (sorting == SortAscending || sorting == SortDescending)
		text.sort(caseSensitivity);
    else if (sorting == SortRandomShuffle){
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(text.begin(), text.end(),g);
    }
	if (sorting == SortDescending)
		std::reverse(text.begin(), text.end());
	if (removeDuplicates)
		text.removeDuplicates();

	if (additionalEmptyLine) text.append("");

	editor->document()->beginMacro();
	for (int i=0; i < cursors.length() - 1; i++){
		QStringList lines;
		for (int j=0; j < qMin(spannedLines[i], text.length()); j++)
			lines << text.takeFirst();
		cursors[i].replaceSelectedText(lines.join('\n'));
	}
	cursors.last().replaceSelectedText(text.join('\n'));
	editor->document()->endMacro();
}

QString LatexEditorViewConfig::translateEditOperation(int key)
{
    return QEditor::translateEditOperation(static_cast<QEditor::EditOperation>(key));
}

QList<int> LatexEditorViewConfig::possibleEditOperations()
{
	int  temp[] = {
		QEditor::NoOperation,
		QEditor::Invalid,

		QEditor::CursorUp,
		QEditor::CursorDown,
		QEditor::CursorLeft,
		QEditor::CursorRight,
		QEditor::CursorWordLeft,
		QEditor::CursorWordRight,
		QEditor::CursorStartOfLine,
        QEditor::CursorStartOfLineText,
		QEditor::CursorEndOfLine,
		QEditor::CursorStartOfDocument,
		QEditor::CursorEndOfDocument,

		QEditor::CursorPageUp,
		QEditor::CursorPageDown,

		QEditor::SelectCursorUp,
		QEditor::SelectCursorDown,
		QEditor::SelectCursorLeft,
		QEditor::SelectCursorRight,
		QEditor::SelectCursorWordLeft,
		QEditor::SelectCursorWordRight,
		QEditor::SelectCursorStartOfLine,
        QEditor::SelectCursorStartOfLineText,
		QEditor::SelectCursorEndOfLine,
		QEditor::SelectCursorStartOfDocument,
		QEditor::SelectCursorEndOfDocument,

		QEditor::SelectPageUp,
		QEditor::SelectPageDown,

		QEditor::EnumForCursorEnd,

		QEditor::DeleteLeft,
		QEditor::DeleteRight,
		QEditor::DeleteLeftWord,
		QEditor::DeleteRightWord,
		QEditor::NewLine,

		QEditor::ChangeOverwrite,
		QEditor::Undo,
		QEditor::Redo,
		QEditor::Copy,
		QEditor::Paste,
		QEditor::Cut,
		QEditor::Print,
		QEditor::SelectAll,
		QEditor::Find,
		QEditor::FindNext,
		QEditor::FindPrevious,
		QEditor::Replace,

		QEditor::CreateMirrorUp,
		QEditor::CreateMirrorDown,
		QEditor::NextPlaceHolder,
		QEditor::PreviousPlaceHolder,
		QEditor::NextPlaceHolderOrWord,
		QEditor::PreviousPlaceHolderOrWord,
		QEditor::NextPlaceHolderOrChar,
		QEditor::PreviousPlaceHolderOrChar,
		QEditor::TabOrIndentSelection,
		QEditor::IndentSelection,
		QEditor::UnindentSelection
	};
	QList<int> res;
    int operationCount = static_cast<int>(sizeof(temp) / sizeof(int)); //sizeof(array) is possible with c-arrays
	for (int i = 0; i < operationCount; i++)
		res << temp[i];
	return res;
}

/*
 * If the cursor is at the border of a parenthesis, this returns a QDocumentCursor with a selection of the parenthized text.
 * Otherwise, a default QDocumentCursor is returned.
 */
QDocumentCursor LatexEditorView::parenthizedTextSelection(const QDocumentCursor &cursor, bool includeParentheses)
{
	QDocumentCursor from, to;
	cursor.getMatchingPair(from, to, includeParentheses);
	if (!from.hasSelection() || !to.hasSelection()) return QDocumentCursor();
	QDocumentCursor::sort(from, to);
	return QDocumentCursor(from.selectionStart(), to.selectionEnd());
}

/*
 * finds the beginning of the specified allowedFormats
 * additional formats can be allowed at the line end (e.g. comments)
 */
QDocumentCursor LatexEditorView::findFormatsBegin(const QDocumentCursor &cursor, QSet<int> allowedFormats, QSet<int> )
{
	QDocumentCursor c(cursor);

	QVector<int> lineFormats = c.line().getFormats();
	int col = c.columnNumber();
    QFormatRange rng=c.line().getOverlayAt(col);
    if (col >= 0 && allowedFormats.contains(rng.format) ) {
		while (true) {
            while (col > 0) {
                rng=c.line().getOverlayAt(col-1);
                if(allowedFormats.contains(rng.format)){
                    col--;
                }else{
                    break;
                }
            }
			if (col > 0) break;
			// continue on previous line
			c.movePosition(1, QDocumentCursor::PreviousLine);
			c.movePosition(1, QDocumentCursor::EndOfLine);
			lineFormats = c.line().getFormats();
			col = c.columnNumber();
            while (col > 0) {
                rng=c.line().getOverlayAt(col-1);
                if(allowedFormats.contains(rng.format)){
                    col--;
                }else{
                    break;
                }
            }
		}
		c.setColumnNumber(col);
		return c;
	}
	return QDocumentCursor();
}

void LatexEditorView::triggeredThesaurus()
{
	QAction *act = qobject_cast<QAction *>(sender());
	QPoint pt = act->data().toPoint();
	emit thesaurus(pt.x(), pt.y());
}

void LatexEditorView::changeSpellingDict(const QString &name)
{
	QString similarName;
	if (spellerManager->hasSpeller(name)) {
		setSpeller(name);
	} else if (spellerManager->hasSimilarSpeller(name, similarName)) {
		setSpeller(similarName);
	}
}

void LatexEditorView::copyImageFromAction()
{
	QAction *act = qobject_cast<QAction *>(sender());
	if (!act) return;

	QPixmap pm = act->data().value<QPixmap>();
	if (!pm.isNull()) {
		QApplication::clipboard()->setImage(pm.toImage());
	}
}

void LatexEditorView::saveImageFromAction()
{
	static QString lastSaveDir;

	QAction *act = qobject_cast<QAction *>(sender());
	if (!act) return;

	QPixmap pm = act->data().value<QPixmap>();

	QString fname = FileDialog::getSaveFileName(this , tr("Save Preview Image"), lastSaveDir, tr("Images") + " (*.png *.jpg *.jpeg)");
	if (fname.isEmpty()) return;

	QFileInfo fi(fname);
	lastSaveDir = fi.absolutePath();
	pm.save(fname);
}


void LatexEditorViewConfig::settingsChanged()
{
	if (!hackAutoChoose) {
		lastFontFamily = "";
		return;
	}
	if (lastFontFamily == fontFamily && lastFontSize == fontSize) return;

	QFont f(fontFamily, fontSize);
#if (QT_VERSION>=QT_VERSION_CHECK(6,0,0))
    f.setStyleHint(QFont::Courier);
#else
	f.setStyleHint(QFont::Courier, QFont::ForceIntegerMetrics);
#endif

	f.setKerning(false);

    QList<QFontMetrics> fms; // QFontMetric should be okay as it is just used to check for monospace font.
	for (int b = 0; b < 2; b++) for (int i = 0; i < 2; i++) {
			QFont ft(f);
			ft.setBold(b);
			ft.setItalic(i);
            fms << QFontMetrics(ft);
		}

	bool lettersHaveDifferentWidth = false, sameLettersHaveDifferentWidth = false;
	int letterWidth = UtilsUi::getFmWidth(fms.first(), 'a');

	const QString lettersToCheck("abcdefghijklmnoqrstuvwxyzABCDEFHIJKLMNOQRSTUVWXYZ_+ 123/()=.,;#");
	QVector<QMap<QChar, int> > widths;
	widths.resize(fms.size());

	foreach (const QChar &c, lettersToCheck) {
		for (int fmi = 0; fmi < fms.size(); fmi++) {
			const QFontMetrics &fm = fms[fmi];
			int currentWidth = UtilsUi::getFmWidth(fm, c);
			widths[fmi].insert(c, currentWidth);
			if (currentWidth != letterWidth) lettersHaveDifferentWidth = true;
			QString testString;
			for (int i = 1; i < 10; i++) {
				testString += c;
				int stringWidth = UtilsUi::getFmWidth(fm, testString);
				if (stringWidth % i != 0) sameLettersHaveDifferentWidth = true;
				if (currentWidth != stringWidth / i) sameLettersHaveDifferentWidth = true;
			}
			if (lettersHaveDifferentWidth && sameLettersHaveDifferentWidth) break;
		}
		if (lettersHaveDifferentWidth && sameLettersHaveDifferentWidth) break;
	}
	const QString ligatures[2] = {"aftt", "afit"};
	for (int l = 0; l < 2 && !sameLettersHaveDifferentWidth; l++) {
		for (int fmi = 0; fmi < fms.size(); fmi++) {
			int expectedWidth = 0;
			for (int i = 0; i < ligatures[l].size() && !sameLettersHaveDifferentWidth; i++) {
				expectedWidth += widths[fmi].value(ligatures[l][i]);
				if (expectedWidth != UtilsUi::getFmWidth(fms[fmi], ligatures[l].left(i + 1))) sameLettersHaveDifferentWidth = true;
			}
		}
	}

	if (!QFontInfo(f).fixedPitch()) hackDisableFixedPitch = false; //won't be enabled anyways
	else hackDisableFixedPitch = lettersHaveDifferentWidth || sameLettersHaveDifferentWidth;
	hackDisableWidthCache = sameLettersHaveDifferentWidth;

#if defined( Q_OS_LINUX ) || defined( Q_OS_WIN )
	hackDisableLineCache = true;
#else
	hackDisableLineCache = false;
	//hackDisableLineCache = isRetinaMac();
#endif
	hackRenderingMode = 0; //always use qce, best tested

	lastFontFamily = fontFamily;
	lastFontSize = fontSize;
}


QString BracketInvertAffector::affect(const QKeyEvent *, const QString &base, int, int) const
{
	static const QString &brackets = "<>()[]{}";
	QString after;
	for (int i = 0; i < base.length(); i++)
		if (brackets.indexOf(base[i]) >= 0)
			after += brackets[brackets.indexOf(base[i]) + 1 - 2 * (brackets.indexOf(base[i]) & 1) ];
		else if (base[i] == '\\') {
			if (base.mid(i, 7) == "\\begin{") {
				after += "\\end{" + base.mid(i + 7);
				return after;
			} else if (base.mid(i, 5) == "\\end{") {
				after += "\\begin{" + base.mid(i + 5);
				return after;
			} else if (base.mid(i, 5) == "\\left") {
				after += "\\right";
				i += 4;
			} else if (base.mid(i, 6) == "\\right") {
				after += "\\left";
				i += 5;
			} else after += '\\';
		} else after += base[i];
	return after;
}

BracketInvertAffector *inverterSingleton = nullptr;

BracketInvertAffector *BracketInvertAffector::instance()
{
	if (!inverterSingleton) inverterSingleton = new BracketInvertAffector();
	return inverterSingleton;
}

void LatexEditorView::bibtexSectionFound(QString content)
{
	QToolTip::showText(editor->mapToGlobal(editor->mapFromFrame(lastPos)), content);
}

void LatexEditorView::lineMarkContextMenuRequested(int lineNumber, QPoint globalPos)
{
	if (!document) return;

	QDocumentLine line(document->line(lineNumber));
	QMenu menu(this);

	for (int i = -1; i < 10; i++) {
		int rmid = bookMarkId(i);
		if (line.hasMark(rmid)) {
			QAction *act =  new QAction(tr("Remove Bookmark"), &menu);
			act->setData(-2);
			menu.addAction(act);
			menu.addSeparator();
			break;
		}
	}

	QAction *act = new QAction(getRealIconCached("lbook"), tr("Unnamed Bookmark"), &menu);
	act->setData(-1);
	menu.addAction(act);

	for (int i = 1; i < 11; i++) {
		int modi = i % 10;
		QAction *act = new QAction(getRealIconCached(QString("lbook%1").arg(modi)), tr("Bookmark") + QString(" %1").arg(modi), &menu);
		act->setData(modi);
		menu.addAction(act);
	}

	act = menu.exec(globalPos);
	if (act) {
		int bookmarkNumber = act->data().toInt();
		if (bookmarkNumber == -2) {
			for (int i = -1; i < 10; i++) {
				int rmid = bookMarkId(i);
				if (line.hasMark(rmid)) {
					removeBookmark(line.handle(), i);
					return;
				}
			}
		} else {
			toggleBookmark(bookmarkNumber, line);
		}
	}
}

void LatexEditorView::foldContextMenuRequested(int lineNumber, QPoint globalPos)
{
	Q_UNUSED(lineNumber)

	QMenu menu;
	QAction *act = new QAction(tr("Collapse All"), &menu);
	act->setData(-5);
	menu.addAction(act);
	for (int i = 1; i <= 4; i++) {
		act = new QAction(QString(tr("Collapse Level %1")).arg(i), &menu);
		act->setData(-i);
		menu.addAction(act);
	}
	menu.addSeparator();
	act = new QAction(tr("Expand All"), &menu);
	act->setData(5);
	menu.addAction(act);
	for (int i = 1; i <= 4; i++) {
		act = new QAction(QString(tr("Expand Level %1")).arg(i), &menu);
		act->setData(i);
		menu.addAction(act);
	}

	act = menu.exec(globalPos);
	if (act) {
		int level = act->data().toInt();
		if (qAbs(level) < 5) {
			foldLevel(level > 0, qAbs(level));
		} else {
			foldEverything(level > 0);
		}
	}
}

LinkOverlay::LinkOverlay(const LinkOverlay &o)
{
	type = o.type;
	if (o.isValid()) {
		docLine = o.docLine;
		formatRange = o.formatRange;
        m_link = o.m_link;
	}
}

LinkOverlay::LinkOverlay(const Token &token, LinkOverlay::LinkOverlayType ltype) :
	type(ltype)
{
	if (type == Invalid) return;

	int from = token.start;
	int to = from + token.length - 1;
	if (from < 0 || to < 0 || to <= from)
		return;

	REQUIRE(QDocument::defaultFormatScheme());
	formatRange = QFormatRange(from, to - from + 1, QDocument::defaultFormatScheme()->id("link"));
	docLine = QDocumentLine(token.dlh);
}

QString LinkOverlay::text() const
{
	if (!isValid()) return QString();
	return docLine.text().mid(formatRange.offset, formatRange.length);
}

QString LatexEditorView::getSearchText()
{
	return searchReplacePanel->getSearchText();
}

QString LatexEditorView::getReplaceText()
{
	return searchReplacePanel->getReplaceText();
}

bool LatexEditorView::getSearchIsWords()
{
	return searchReplacePanel->getSearchIsWords();
}

bool LatexEditorView::getSearchIsCase()
{
	return searchReplacePanel->getSearchIsCase();
}

bool LatexEditorView::getSearchIsRegExp()
{
	return searchReplacePanel->getSearchIsRegExp();
}

bool LatexEditorView::isInMathHighlighting(const QDocumentCursor &cursor )
{
	const QDocumentLine &line = cursor.line();
	if (!line.handle()) return false;

    int col = cursor.columnNumber();

    const QFormatRange &format = line.handle()->getOverlayAt(col,numbersFormat);

    return format.isValid();
}

void LatexEditorView::checkRTLLTRLanguageSwitching()
{
#if defined( Q_OS_WIN ) || defined( Q_OS_LINUX ) || ( defined( Q_OS_UNIX ) && !defined( Q_OS_MAC ) )
	QDocumentCursor cursor = editor->cursor();
	QDocumentLine line = cursor.line();
	InputLanguage language = IL_UNCERTAIN;
	if (line.firstChar() >= 0) { //whitespace lines have no language information
		if (config->switchLanguagesMath) {
			if (isInMathHighlighting(cursor)) language = IL_LTR;
			else language = IL_RTL;
		}

		if (config->switchLanguagesDirection && language != IL_LTR) {
			if (line.hasFlag(QDocumentLine::LayoutDirty))
				if (line.isRTLByLayout() || line.isRTLByText() ) {
					line.handle()->lockForWrite();
					line.handle()->layout(cursor.lineNumber());
					line.handle()->unlock();
				}
			if (!line.isRTLByLayout())
				language = IL_LTR;
			else {
				int c = cursor.columnNumber();
				int dir = line.rightCursorPosition(c) - c;
				if (dir < 0) language = IL_RTL;
				else if (dir > 0) language = IL_LTR;
			}
		}
	}
	setInputLanguage(language);
#endif
}
