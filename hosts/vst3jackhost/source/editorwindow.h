// EditorWindow — native Haiku window hosting a VST3 plug-in editor (IPlugView).
//
// Implements the host side of the kPlatformTypeHaikuBView contract from
// pluginterfaces/gui/iplugview.h: the window owns a plain parent BView; the
// plug-in view is attached while the window looper is locked (a fresh BWindow
// is locked by its creating thread until Show(), so attaching from the ctor
// satisfies the contract) and detached from QuitRequested() on the window's
// looper thread.
//
// Threading: once a window is open, the IEditController must only be touched
// from this window's looper thread. The REPL marshals controller-touching
// commands here via kMsgExec (synchronous; see runOnUiThread in main.cpp).
// A ~30 Hz BMessageRunner drains the AudioClient's output parameter transfer
// (RT -> UI, lock-free) into IEditController::setParamNormalized.

#pragma once

#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <MessageRunner.h>
#include <Window.h>

#include <atomic>
#include <functional>

class EditorWindow;

// Host IPlugFrame: the plug-in requests a resize; we resize the BWindow and
// confirm with onSize() in the same call stack (iplugview.h resize protocol).
// Lives and dies with its window, so FUnknown ref counting is inert.
class PlugFrame : public Steinberg::IPlugFrame
{
public:
    explicit PlugFrame(EditorWindow *window) : fWindow(window)
    {
    }

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view,
                                             Steinberg::ViewRect *newSize) override;

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override
    {
        return 100;
    }
    Steinberg::uint32 PLUGIN_API release() override
    {
        return 100;
    }

private:
    EditorWindow *fWindow;
};

// Host IComponentHandler: receives parameter edits from the plug-in's own
// editor and forwards them to the RT thread through the AudioClient's
// lock-free transfer (the plug-in already updated its controller value
// before calling performEdit). Owned by main(), outlives the window.
class ComponentHandler : public Steinberg::Vst::IComponentHandler
{
public:
    // setParam pushes one normalized value to the RT thread (no controller call).
    using SetParamFunc = std::function<void(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue)>;

    explicit ComponentHandler(SetParamFunc setParam) : fSetParam(std::move(setParam))
    {
    }

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                              Steinberg::Vst::ParamValue valueNormalized) override
    {
        fSetParam(id, valueNormalized);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override
    {
        return 100;
    }
    Steinberg::uint32 PLUGIN_API release() override
    {
        return 100;
    }

private:
    SetParamFunc fSetParam;
};

class EditorWindow : public BWindow
{
public:
    static const uint32 kMsgExec = 'jhEx'; // "fn" pointer: const std::function<void()>*
    static const uint32 kMsgPoll = 'jhPl';

    // Takes ownership of `view` (already checked for kPlatformTypeHaikuBView
    // support, not yet attached). `controller` and `outputTransfer` must
    // outlive the window; `aliveFlag` is cleared in the destructor so the
    // REPL thread can wait for teardown.
    EditorWindow(const char *title, Steinberg::IPlugView *view,
                 Steinberg::Vst::IEditController *controller,
                 Steinberg::Vst::ParameterChangeTransfer *outputTransfer,
                 std::atomic<bool> *aliveFlag);
    ~EditorWindow() override;

    bool QuitRequested() override;
    void MessageReceived(BMessage *message) override;

    // Called by PlugFrame::resizeView on the window looper thread.
    Steinberg::tresult ResizeFromPlugView(Steinberg::ViewRect *newSize);

private:
    void DetachPlugView();

    Steinberg::IPtr<Steinberg::IPlugView> fPlugView;
    Steinberg::Vst::IEditController *fController;
    Steinberg::Vst::ParameterChangeTransfer *fOutputTransfer;
    std::atomic<bool> *fAliveFlag;
    PlugFrame *fPlugFrame{nullptr};
    BView *fParent{nullptr};
    BMessageRunner *fPoll{nullptr};
    bool fDetached{false};
};
