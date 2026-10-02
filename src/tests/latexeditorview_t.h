#ifndef Header_Latex_Editor_View_T
#define Header_Latex_Editor_View_T
#ifndef QT_NO_DEBUG
#include "mostQtHeaders.h"
#include <QtTest/QtTest>

class LatexEditorView;
class LatexEditorViewTest: public QObject{
	Q_OBJECT
	public:
		LatexEditorViewTest(LatexEditorView* view);
	private:
		LatexEditorView *edView;
	private slots:
		void insertHardLineBreaks_data();
		void insertHardLineBreaks();
		void inMathEnvironment_data();
        void inMathEnvironment();
        void vimEditingModeSwitches();
        void vimCursorStyles();
        void vimCtrlClickNavigation();
        void vimInsertEscape();
        void vimVisualLineStaysOnCurrentLine();
        void vimVisualBlockCtrlV();
        void vimVisualBlockDeleteAffectsAllRows();
        void vimVisualBlockInsertAtStart();
        void vimCloseElementEscapesInsertMode();
        void vimCloseElementIsConsumedInNormalMode();
        void vimPromptEnterDoesNotInsertNewline();
        void vimPromptHistory();
        void vimMarks();
        void vimDeleteLastLineMovesToPreviousLine();
        void vimLinewisePasteKeepsCursorOnInsertedText();
        void vimNormalModeConsumesUnhandledPrintableKeys();
        void vimExSubstituteCommands();
        void vimExCommands();
        void vimRegisterCommands_data();
        void vimRegisterCommands();
        void vimPhysicalModifierEvents();
        void vimRegisterStore();
        void vimRegistersSharedAcrossViews();
        void vimClipboardRegisters();
        void vimDesktopClipboard();
        void vimDocumentedModes_data();
        void vimDocumentedModes();
        void vimSubstituteFlags_data();
        void vimSubstituteFlags();
        void vimSearchNavigation();
        void vimSubstituteConfirmation();
        void vimSubstitutePrompt();
        void vimDocumentedMotions_data();
        void vimDocumentedMotions();
};

#endif
#endif
