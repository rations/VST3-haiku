// HaikuPlugView — reusable base class for native Haiku VST3 plug-in editors.
//
// Implements the kPlatformTypeHaikuBView contract from pluginterfaces/gui/iplugview.h:
// the host passes a BView* (attached to a BWindow, looper locked) to IPlugView::attached();
// the plug-in adds one child BView to it and from then on lives on that window's looper
// thread. There is no IRunLoop on Haiku — use BMessageRunner for timers.
//
// This header lives OUTSIDE the vendored SDK tree on purpose: the upstream diff stays a
// single constant in iplugview.h, while plug-ins share this behavior via
//   target_include_directories(<plugin> PRIVATE "${VST3_SDK_DIR}/../haiku")
//
// Subclasses implement createHaikuView() and must do ALL app_server work (BView
// construction, BBitmap loading) there — never in the constructor — so that createView()
// remains harmless in headless hosts (validator, REPL hosts without an open editor).
//
// Threading: attachedToParent()/removedFromParent() run with the parent window's looper
// locked (host contract; locking here is defensive and recursion-safe). The controller
// hooks editorAttached()/editorRemoved() fire on the same thread — after the child view
// exists and before it is deleted, respectively.

#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"

#include <Looper.h>
#include <View.h>

#include <cstring>

namespace Steinberg
{

//------------------------------------------------------------------------
class HaikuPlugView : public Vst::EditorView
{
public:
    HaikuPlugView(Vst::EditController *controller, ViewRect *size = nullptr)
        : Vst::EditorView(controller, size)
    {
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE
    {
        if (type && std::strcmp(type, kPlatformTypeHaikuBView) == 0)
            return kResultTrue;
        return kResultFalse;
    }

    // v1 contract: fixed-size editors (CPluginView already answers canResize() with
    // kResultFalse; stated here as documentation of intent).

    void attachedToParent() SMTG_OVERRIDE
    {
        BView *parent = static_cast<BView *>(systemWindow);
        if (!parent)
            return;
        // Host holds the window looper lock during attached(); BLooper locks are
        // recursive, so this extra lock is safe and covers non-conforming hosts.
        bool locked = parent->LockLooper();
        if (!fView) {
            fView = createHaikuView(BRect(0, 0, rect.getWidth() - 1, rect.getHeight() - 1));
            if (fView)
                parent->AddChild(fView);
        }
        if (locked)
            parent->UnlockLooper();
        // Notify the controller (EditorView::attachedToParent -> editorAttached) only
        // once the child view exists, so the controller may immediately push values.
        Vst::EditorView::attachedToParent();
    }

    void removedFromParent() SMTG_OVERRIDE
    {
        // Controller first (editorRemoved), so it stops touching the view before the
        // view is destroyed.
        Vst::EditorView::removedFromParent();
        if (fView) {
            // Keep the looper pointer: after RemoveSelf() the view has none to unlock.
            BLooper *looper = fView->Looper();
            bool locked = looper ? looper->Lock() : false;
            fView->RemoveSelf();
            if (locked)
                looper->Unlock();
            delete fView;
            fView = nullptr;
        }
    }

protected:
    // Build the plug-in's view, sized to `frame` (B_FOLLOW_NONE is fine; the parent is
    // sized by the host from getSize()). Called with the parent's looper locked.
    virtual BView *createHaikuView(BRect frame) = 0;

    BView *fView{nullptr};
};

//------------------------------------------------------------------------
} // namespace Steinberg
