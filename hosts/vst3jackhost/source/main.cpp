// vst3jackhost — VST3 host for Haiku on JACK, with a stdin parameter REPL and an
// optional native editor window (IPlugView, kPlatformTypeHaikuBView).
//
// Loads one VST3 audio-effect class from a .vst3 bundle, wires it between JACK
// ports (reusing the SDK audiohost sample's AudioClient/JackClient), and lets the
// user inspect and change parameters from the terminal. Parameter changes reach
// the real-time thread only through AudioClient's lock-free ParameterChangeTransfer.
//
// Threading: main() owns the BApplication and runs its message loop; the REPL
// runs on a separate thread. While no editor window is open, the REPL touches
// the IEditController directly; once one is open, every controller-touching
// command is marshaled to the window's looper thread (EditorWindow::kMsgExec,
// synchronous), so the controller is only ever driven from one thread at a time.
//
// Usage:
//   vst3jackhost <plugin.vst3> [--uid <class-UID>]
//   vst3jackhost <plugin.vst3> --list
//
// REPL commands: help, list, params, get <id>, set <id> <normalized 0..1>,
//                editor, editor close, savestate <file>, loadstate <file>, quit

#include "public.sdk/samples/vst-hosting/audiohost/source/media/audioclient.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include "editorwindow.h"
#include "inamfileloader.h"

#include <Application.h>
#include <Messenger.h>
#include <OS.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;

namespace
{

void printClasses(const VST3::Hosting::PluginFactory &factory)
{
    printf("classes in module:\n");
    for (auto &classInfo : factory.classInfos()) {
        printf("  name: %-32s category: %-24s uid: %s\n", classInfo.name().c_str(),
               classInfo.category().c_str(), classInfo.ID().toString().c_str());
    }
}

std::string paramTitle(Vst::IEditController *controller, const Vst::ParameterInfo &info)
{
    return Vst::StringConvert::convert(info.title);
}

void printParams(Vst::IEditController *controller)
{
    int32 count = controller->getParameterCount();
    printf("%d parameters:\n", count);
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info{};
        if (controller->getParameterInfo(i, info) != kResultOk)
            continue;
        Vst::ParamValue norm = controller->getParamNormalized(info.id);
        Vst::String128 display{};
        std::string displayStr;
        if (controller->getParamStringByValue(info.id, norm, display) == kResultOk)
            displayStr = Vst::StringConvert::convert(display);
        std::string units = Vst::StringConvert::convert(info.units);
        const char *flagsNote = (info.flags & Vst::ParameterInfo::kIsReadOnly) ? " [read-only]"
                                : (info.flags & Vst::ParameterInfo::kIsBypass) ? " [bypass]"
                                                                               : "";
        printf("  id %6u  %-28s = %.4f  (%s %s)%s\n", info.id, paramTitle(controller, info).c_str(),
               norm, displayStr.c_str(), units.c_str(), flagsNote);
    }
}

void printParam(Vst::IEditController *controller, Vst::ParamID id)
{
    int32 count = controller->getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info{};
        if (controller->getParameterInfo(i, info) != kResultOk || info.id != id)
            continue;
        Vst::ParamValue norm = controller->getParamNormalized(id);
        Vst::String128 display{};
        std::string displayStr;
        if (controller->getParamStringByValue(id, norm, display) == kResultOk)
            displayStr = Vst::StringConvert::convert(display);
        printf("  id %6u  %-28s = %.4f  (%s %s)\n", info.id, paramTitle(controller, info).c_str(),
               norm, displayStr.c_str(), Vst::StringConvert::convert(info.units).c_str());
        return;
    }
    printf("no parameter with id %u\n", id);
}

void printHelp(bool hasFileLoader)
{
    printf("commands:\n"
           "  params            list all parameters with current values\n"
           "  get <id>          show one parameter\n"
           "  set <id> <value>  set parameter (normalized 0..1)\n"
           "  editor            open the plug-in's native editor window\n"
           "  editor close      close the editor window\n"
           "  list              list classes in the module\n"
           "  savestate <file>  save the plug-in state to a file\n"
           "  loadstate <file>  restore the plug-in state from a file\n"
           "  help              this help\n"
           "  quit              exit\n");
    if (hasFileLoader)
        printf("file loading (plug-in supports it):\n"
               "  loadmodel <path>  load a .nam model file\n"
               "  loadir <path>     load a .wav impulse response\n"
               "  clearmodel        unload the model\n"
               "  clearir           unload the impulse response\n"
               "  files             show currently loaded files\n");
}

// PlugProvider whose component<->controller connection works from any
// non-RT thread. The SDK's ConnectionProxy discards messages sent off the
// thread it was created on ("UI main thread"); Haiku has no such single UI
// thread — the plug-in's editor lives on its BWindow's looper thread — so the
// proxies would silently drop every editor-originated message (file loads,
// MIDI-learn arming). A direct IConnectionPoint connection has no thread
// guard; plug-in notify() implementations must be message-thread-safe, which
// the VST3 threading model requires of them anyway.
class DirectPlugProvider : public Vst::PlugProvider
{
public:
    using Vst::PlugProvider::PlugProvider;

    bool connectDirect()
    {
        if (!component || !controller)
            return false;
        disconnectComponents(); // drop the thread-affine proxies
        auto compICP = U::cast<Vst::IConnectionPoint>(component);
        auto contrICP = U::cast<Vst::IConnectionPoint>(controller);
        if (!compICP || !contrICP)
            return false;
        return compICP->connect(contrICP) == kResultTrue &&
               contrICP->connect(compICP) == kResultTrue;
    }

    void disconnectDirect()
    {
        auto compICP = U::cast<Vst::IConnectionPoint>(component);
        auto contrICP = U::cast<Vst::IConnectionPoint>(controller);
        if (compICP && contrICP) {
            compICP->disconnect(contrICP);
            contrICP->disconnect(compICP);
        }
    }
};

} // namespace

int main(int argc, char *argv[])
{
    std::string path;
    std::string uidString;
    bool listOnly = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--uid" && i + 1 < argc)
            uidString = argv[++i];
        else if (arg == "--list")
            listOnly = true;
        else if (arg == "--help" || arg == "-h") {
            printf("usage: vst3jackhost <plugin.vst3> [--uid <class-UID>] [--list]\n");
            return 0;
        } else
            path = arg;
    }
    if (path.empty()) {
        fprintf(stderr, "usage: vst3jackhost <plugin.vst3> [--uid <class-UID>] [--list]\n");
        return 1;
    }

    // The BApplication must exist before any window/BMessageRunner is created;
    // its message loop runs on this (main) thread at the bottom of main().
    BApplication app("application/x-vnd.VST3-haiku-vst3jackhost");

    // The host context must be in place before any plug-in code runs.
    Vst::PluginContextFactory::instance().setPluginContext(new Vst::HostApplication());

    std::string error;
    auto module = VST3::Hosting::Module::create(path, error);
    if (!module) {
        fprintf(stderr, "could not load module '%s': %s\n", path.c_str(), error.c_str());
        return 1;
    }

    auto factory = module->getFactory();
    if (listOnly) {
        printClasses(factory);
        return 0;
    }

    VST3::Optional<VST3::UID> effectID;
    if (!uidString.empty()) {
        if (auto uid = VST3::UID::fromString(uidString))
            effectID = std::move(*uid);
        else {
            fprintf(stderr, "invalid UID '%s'\n", uidString.c_str());
            return 1;
        }
    }

    IPtr<DirectPlugProvider> plugProvider;
    std::string className;
    for (auto &classInfo : factory.classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass)
            continue;
        if (effectID && *effectID != classInfo.ID())
            continue;
        plugProvider = owned(new DirectPlugProvider(factory, classInfo, true));
        className = classInfo.name();
        break;
    }
    if (!plugProvider || !plugProvider->initialize()) {
        fprintf(stderr, "no matching audio-effect class in '%s' (try --list)\n", path.c_str());
        return 1;
    }
    if (!plugProvider->connectDirect())
        fprintf(stderr, "warning: direct component/controller connection failed\n");

    OPtr<Vst::IComponent> component = plugProvider->getComponent();
    OPtr<Vst::IEditController> controller = plugProvider->getController();
    if (!component || !controller) {
        fprintf(stderr, "plug-in provides no component/controller pair\n");
        return 1;
    }
    auto midiMapping = U::cast<Vst::IMidiMapping>(controller);

    auto audioClient = Vst::AudioClient::create(className, component, midiMapping);
    if (!audioClient) {
        fprintf(stderr, "could not create the JACK audio client (is jackd running?)\n");
        return 1;
    }

    // Route the editor's parameter edits (IComponentHandler::performEdit) to
    // the RT thread; PlugProvider does not install any handler on its own.
    ComponentHandler componentHandler([&audioClient](Vst::ParamID id, Vst::ParamValue value) {
        audioClient->setParameter(id, value, 0);
    });
    if (controller->setComponentHandler(&componentHandler) != kResultOk)
        fprintf(stderr, "warning: plug-in refused the component handler\n");

    // Optional file-loading extension (NAMku and friends): discovered purely
    // via queryInterface, so unknown plug-ins are unaffected.
    IPtr<NAMku::INamFileLoader> fileLoader;
    {
        NAMku::INamFileLoader *fl = nullptr;
        if (controller->queryInterface(NAMku::INamFileLoader_iid, (void **)&fl) == kResultOk && fl)
            fileLoader = owned(fl);
    }

    printf("loaded '%s' from %s\n", className.c_str(), path.c_str());
    if (fileLoader)
        printf("plug-in supports file loading (loadmodel/loadir)\n");
    printf("JACK client '%s' is running; type 'help' for commands.\n", className.c_str());

    // Editor window state, owned by the REPL thread. `editorAlive` is cleared
    // by the window's destructor (window looper thread).
    std::atomic<bool> editorAlive{false};
    BMessenger editorMessenger;

    // Run `fn` on the window looper thread when an editor is open (synchronous
    // round trip), directly otherwise. Falls back to a direct call if the
    // window dies mid-send.
    auto onUiThread = [&](const std::function<void()> &fn) {
        if (editorAlive.load()) {
            BMessage msg(EditorWindow::kMsgExec);
            BMessage reply;
            msg.AddPointer("fn", &fn);
            if (editorMessenger.SendMessage(&msg, &reply) == B_OK)
                return;
        }
        fn();
    };

    auto closeEditor = [&]() {
        if (!editorAlive.load())
            return;
        editorMessenger.SendMessage(B_QUIT_REQUESTED);
        for (int i = 0; i < 200 && editorAlive.load(); ++i)
            snooze(10000);
        if (editorAlive.load())
            fprintf(stderr, "warning: editor window did not close in time\n");
    };

    auto repl = [&]() {
        std::string line;
        while (true) {
            printf("> ");
            fflush(stdout);
            if (!std::getline(std::cin, line))
                break;
            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;
            if (cmd.empty())
                continue;
            if (cmd == "quit" || cmd == "exit")
                break;
            if (cmd == "help") {
                printHelp(fileLoader != nullptr);
            } else if (cmd == "list") {
                printClasses(factory);
            } else if (cmd == "params") {
                onUiThread([&]() { printParams(controller); });
            } else if (cmd == "get") {
                unsigned long id;
                if (iss >> id)
                    onUiThread([&]() { printParam(controller, static_cast<Vst::ParamID>(id)); });
                else
                    printf("usage: get <id>\n");
            } else if (cmd == "set") {
                unsigned long id;
                double value;
                if (iss >> id >> value) {
                    if (value < 0.0 || value > 1.0) {
                        printf("value must be normalized (0..1)\n");
                        continue;
                    }
                    auto paramID = static_cast<Vst::ParamID>(id);
                    onUiThread([&]() {
                        // Keep the controller's view in sync, then hand the change
                        // to the RT thread through the lock-free transfer.
                        controller->setParamNormalized(paramID, value);
                        audioClient->setParameter(paramID, value, 0);
                        printParam(controller, paramID);
                    });
                } else {
                    printf("usage: set <id> <normalized 0..1>\n");
                }
            } else if (cmd == "editor") {
                std::string sub;
                iss >> sub;
                if (sub == "close") {
                    if (editorAlive.load())
                        closeEditor();
                    else
                        printf("no editor window is open\n");
                    continue;
                }
                if (editorAlive.load()) {
                    printf("editor window is already open\n");
                    continue;
                }
                IPlugView *view = controller->createView(Vst::ViewType::kEditor);
                if (!view) {
                    printf("plug-in has no editor\n");
                    continue;
                }
                if (view->isPlatformTypeSupported(kPlatformTypeHaikuBView) != kResultTrue) {
                    printf("plug-in editor does not support Haiku (kPlatformTypeHaikuBView)\n");
                    view->release();
                    continue;
                }
                editorAlive.store(true);
                auto *window =
                    new EditorWindow(className.c_str(), view, controller,
                                     &audioClient->getOutputParamTransferrer(), &editorAlive);
                editorMessenger = BMessenger(window);
                window->Show();
                printf("editor window opened\n");
            } else if (cmd == "loadmodel" || cmd == "loadir") {
                if (!fileLoader) {
                    printf("this plug-in does not support file loading\n");
                    continue;
                }
                std::string filePath;
                std::getline(iss >> std::ws, filePath);
                if (filePath.empty()) {
                    printf("usage: %s <path>\n", cmd.c_str());
                    continue;
                }
                onUiThread([&]() {
                    tresult r = (cmd == "loadmodel") ? fileLoader->setModelFile(filePath.c_str())
                                                     : fileLoader->setIrFile(filePath.c_str());
                    printf("%s: %s\n", cmd.c_str(), r == kResultOk ? "ok" : "FAILED");
                });
            } else if (cmd == "clearmodel" || cmd == "clearir") {
                if (!fileLoader) {
                    printf("this plug-in does not support file loading\n");
                    continue;
                }
                onUiThread([&]() {
                    tresult r = (cmd == "clearmodel") ? fileLoader->setModelFile("")
                                                      : fileLoader->setIrFile("");
                    printf("%s: %s\n", cmd.c_str(), r == kResultOk ? "ok" : "FAILED");
                });
            } else if (cmd == "savestate") {
                std::string filePath;
                std::getline(iss >> std::ws, filePath);
                if (filePath.empty()) {
                    printf("usage: savestate <file>\n");
                    continue;
                }
                onUiThread([&]() {
                    MemoryStream stream;
                    if (component->getState(&stream) != kResultOk) {
                        printf("savestate: FAILED (plug-in refused getState)\n");
                        return;
                    }
                    FILE *f = fopen(filePath.c_str(), "wb");
                    if (!f) {
                        printf("savestate: cannot open '%s' for writing\n", filePath.c_str());
                        return;
                    }
                    auto size = static_cast<size_t>(stream.getSize());
                    bool ok = fwrite(stream.getData(), 1, size, f) == size;
                    ok = (fclose(f) == 0) && ok;
                    printf("savestate: %s (%zu bytes)\n", ok ? "ok" : "FAILED", size);
                });
            } else if (cmd == "loadstate") {
                std::string filePath;
                std::getline(iss >> std::ws, filePath);
                if (filePath.empty()) {
                    printf("usage: loadstate <file>\n");
                    continue;
                }
                FILE *f = fopen(filePath.c_str(), "rb");
                if (!f) {
                    printf("loadstate: cannot open '%s'\n", filePath.c_str());
                    continue;
                }
                // A state file is untrusted input: reject absurd sizes before
                // allocating (the plug-in bounds-checks the contents itself).
                constexpr long kMaxStateSize = 16 * 1024 * 1024;
                long fileSize = -1;
                if (fseek(f, 0, SEEK_END) == 0)
                    fileSize = ftell(f);
                if (fileSize < 0 || fileSize > kMaxStateSize || fseek(f, 0, SEEK_SET) != 0) {
                    printf("loadstate: '%s' is not a usable state file\n", filePath.c_str());
                    fclose(f);
                    continue;
                }
                std::vector<char> buffer(static_cast<size_t>(fileSize));
                bool readOk = fread(buffer.data(), 1, buffer.size(), f) == buffer.size();
                fclose(f);
                if (!readOk) {
                    printf("loadstate: short read from '%s'\n", filePath.c_str());
                    continue;
                }
                onUiThread([&]() {
                    // Same order a DAW uses on project load: the processor takes
                    // the state, then the controller mirrors it.
                    MemoryStream stream(buffer.data(), static_cast<TSize>(buffer.size()));
                    if (component->setState(&stream) != kResultOk) {
                        printf("loadstate: FAILED (plug-in refused the state)\n");
                        return;
                    }
                    int64 pos = 0;
                    stream.seek(0, IBStream::kIBSeekSet, &pos);
                    if (controller->setComponentState(&stream) != kResultOk)
                        printf("loadstate: warning: controller refused the state\n");
                    printf("loadstate: ok (%ld bytes)\n", fileSize);
                });
            } else if (cmd == "files") {
                if (!fileLoader) {
                    printf("this plug-in does not support file loading\n");
                    continue;
                }
                onUiThread([&]() {
                    char model[1024] = "";
                    char ir[1024] = "";
                    fileLoader->getModelFile(model, sizeof(model));
                    fileLoader->getIrFile(ir, sizeof(ir));
                    printf("model: %s\nir:    %s\n", model[0] ? model : "(none)",
                           ir[0] ? ir : "(none)");
                });
            } else {
                printf("unknown command '%s' (try 'help')\n", cmd.c_str());
            }
        }
        closeEditor();
        be_app->PostMessage(B_QUIT_REQUESTED);
    };

    std::thread replThread(repl);
    app.Run();
    replThread.join();

    printf("shutting down\n");
    controller->setComponentHandler(nullptr);
    audioClient.reset();
    plugProvider->disconnectDirect();
    return 0;
}
