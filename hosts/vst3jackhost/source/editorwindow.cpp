// EditorWindow implementation. See editorwindow.h for the threading contract.

#include "editorwindow.h"

#include "pluginterfaces/base/funknown.h"

#include <View.h>

#include <cstdio>

using namespace Steinberg;

//------------------------------------------------------------------------
// PlugFrame
//------------------------------------------------------------------------

tresult PLUGIN_API PlugFrame::resizeView(IPlugView *view, ViewRect *newSize)
{
    if (!view || !newSize || !fWindow)
        return kInvalidArgument;
    return fWindow->ResizeFromPlugView(newSize);
}

tresult PLUGIN_API PlugFrame::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IPlugFrame *>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

//------------------------------------------------------------------------
// ComponentHandler
//------------------------------------------------------------------------

tresult PLUGIN_API ComponentHandler::restartComponent(int32 flags)
{
    // The interesting flag for a REPL host is kParamValuesChanged: the
    // controller already carries the new values, so there is nothing to
    // re-sync beyond what `params` prints on demand.
    printf("[host] restartComponent(0x%x)\n", (unsigned)flags);
    return kResultOk;
}

tresult PLUGIN_API ComponentHandler::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<Vst::IComponentHandler *>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

//------------------------------------------------------------------------
// EditorWindow
//------------------------------------------------------------------------

EditorWindow::EditorWindow(const char *title, IPlugView *view, Vst::IEditController *controller,
                           Vst::ParameterChangeTransfer *outputTransfer,
                           std::atomic<bool> *aliveFlag)
    : BWindow(BRect(80, 80, 480, 380), title, B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fPlugView(owned(view)), fController(controller), fOutputTransfer(outputTransfer),
      fAliveFlag(aliveFlag)
{
    ViewRect size{};
    if (fPlugView->getSize(&size) != kResultTrue || size.getWidth() <= 0 || size.getHeight() <= 0) {
        size.right = size.left + 400;
        size.bottom = size.top + 300;
    }
    ResizeTo(size.getWidth() - 1, size.getHeight() - 1);

    fParent = new BView(Bounds(), "plugin-parent", B_FOLLOW_ALL, 0);
    fParent->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    AddChild(fParent);

    fPlugFrame = new PlugFrame(this);
    fPlugView->setFrame(fPlugFrame);

    // A fresh BWindow is locked by the creating thread until Show(), so the
    // attach happens under the looper lock as the platform contract requires.
    if (fPlugView->attached(fParent, kPlatformTypeHaikuBView) != kResultOk)
        fprintf(stderr, "editor: IPlugView::attached() failed\n");

    BMessage poll(kMsgPoll);
    fPoll = new BMessageRunner(BMessenger(this), &poll, 33000); // ~30 Hz
    if (fPoll->InitCheck() != B_OK)
        fprintf(stderr, "editor: parameter poll runner failed to start\n");
}

EditorWindow::~EditorWindow()
{
    delete fPoll;
    DetachPlugView();
    delete fPlugFrame;
    if (fAliveFlag)
        fAliveFlag->store(false);
}

void EditorWindow::DetachPlugView()
{
    if (fDetached || !fPlugView)
        return;
    fDetached = true;
    fPlugView->setFrame(nullptr);
    if (fPlugView->removed() != kResultOk)
        fprintf(stderr, "editor: IPlugView::removed() failed\n");
    fPlugView = nullptr; // releases the view (EditorView notifies its controller)
}

bool EditorWindow::QuitRequested()
{
    // Runs on the window looper thread with the looper locked — detach here
    // so the plug-in view is gone before the BWindow (and parent) die.
    delete fPoll;
    fPoll = nullptr;
    DetachPlugView();
    return true;
}

Steinberg::tresult EditorWindow::ResizeFromPlugView(ViewRect *newSize)
{
    ResizeTo(newSize->getWidth() - 1, newSize->getHeight() - 1);
    if (fPlugView)
        return fPlugView->onSize(newSize);
    return kResultFalse;
}

void EditorWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case kMsgPoll: {
            if (!fController || !fOutputTransfer)
                break;
            Vst::ParamID id;
            Vst::ParamValue value;
            int32 sampleOffset;
            while (fOutputTransfer->getNextChange(id, value, sampleOffset))
                fController->setParamNormalized(id, value);
            break;
        }
        case kMsgExec: {
            const void *ptr = nullptr;
            if (message->FindPointer("fn", const_cast<void **>(&ptr)) == B_OK && ptr) {
                const auto *fn = static_cast<const std::function<void()> *>(ptr);
                (*fn)();
            }
            BMessage reply(B_REPLY);
            message->SendReply(&reply);
            break;
        }
        default:
            BWindow::MessageReceived(message);
            break;
    }
}
