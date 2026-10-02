// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VIM_INPUT_BINDING_H
#define VIM_INPUT_BINDING_H

#include "qeditorinputbindinginterface.h"
#include "qpanel.h"

class LatexEditorView;

// Small integration surface: TeXstudio owns the editor, completion and panels;
// the Vim module owns modal command state and dispatch.
class VimInputBinding : public QEditorInputBindingInterface
{
public:
    virtual void resetForEditor(QEditor *editor) = 0;
    virtual void promptClosed(QEditor *editor) = 0;
    virtual void recordSearch(const QString &text, bool backward) = 0;
    virtual QString lastSearchText() const = 0;
    virtual bool shouldOverrideShortcut(const QKeyEvent *event) const = 0;
    virtual bool handleEscapeShortcut(QEditor *editor) = 0;
};

class VimPromptPanel : public QPanel
{
public:
    enum PromptKind { NoPrompt, CommandPrompt, SearchForwardPrompt, SearchBackwardPrompt };
    explicit VimPromptPanel(QWidget *parent) : QPanel(parent) {}
    virtual void openPrompt(PromptKind kind) = 0;
    virtual void closePrompt() = 0;
    virtual PromptKind promptKind() const = 0;
    virtual void showError(const QString &message) = 0;
};

VimInputBinding *createVimInputBinding(LatexEditorView *view, QEditorInputBindingInterface *defaultBinding);
VimPromptPanel *createVimPromptPanel(LatexEditorView *view);

#endif
