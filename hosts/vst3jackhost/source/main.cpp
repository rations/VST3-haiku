// vst3jackhost — headless VST3 host for Haiku on JACK, with a stdin parameter REPL.
//
// Loads one VST3 audio-effect class from a .vst3 bundle, wires it between JACK
// ports (reusing the SDK audiohost sample's AudioClient/JackClient), and lets the
// user inspect and change parameters from the terminal — no GUI needed. Parameter
// changes reach the real-time thread only through AudioClient's lock-free
// ParameterChangeTransfer; the IEditController is only ever touched from this
// (main) thread.
//
// Usage:
//   vst3jackhost <plugin.vst3> [--uid <class-UID>]
//   vst3jackhost <plugin.vst3> --list
//
// REPL commands: help, list, params, get <id>, set <id> <normalized 0..1>, quit

#include "public.sdk/samples/vst-hosting/audiohost/source/media/audioclient.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace Steinberg;

namespace {

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
        Vst::ParameterInfo info {};
        if (controller->getParameterInfo(i, info) != kResultOk)
            continue;
        Vst::ParamValue norm = controller->getParamNormalized(info.id);
        Vst::String128 display {};
        std::string displayStr;
        if (controller->getParamStringByValue(info.id, norm, display) == kResultOk)
            displayStr = Vst::StringConvert::convert(display);
        std::string units = Vst::StringConvert::convert(info.units);
        const char *flagsNote = (info.flags & Vst::ParameterInfo::kIsReadOnly) ? " [read-only]"
            : (info.flags & Vst::ParameterInfo::kIsBypass)                     ? " [bypass]"
                                                                               : "";
        printf("  id %6u  %-28s = %.4f  (%s %s)%s\n", info.id, paramTitle(controller, info).c_str(),
               norm, displayStr.c_str(), units.c_str(), flagsNote);
    }
}

void printParam(Vst::IEditController *controller, Vst::ParamID id)
{
    int32 count = controller->getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info {};
        if (controller->getParameterInfo(i, info) != kResultOk || info.id != id)
            continue;
        Vst::ParamValue norm = controller->getParamNormalized(id);
        Vst::String128 display {};
        std::string displayStr;
        if (controller->getParamStringByValue(id, norm, display) == kResultOk)
            displayStr = Vst::StringConvert::convert(display);
        printf("  id %6u  %-28s = %.4f  (%s %s)\n", info.id, paramTitle(controller, info).c_str(),
               norm, displayStr.c_str(), Vst::StringConvert::convert(info.units).c_str());
        return;
    }
    printf("no parameter with id %u\n", id);
}

void printHelp()
{
    printf("commands:\n"
           "  params            list all parameters with current values\n"
           "  get <id>          show one parameter\n"
           "  set <id> <value>  set parameter (normalized 0..1)\n"
           "  list              list classes in the module\n"
           "  help              this help\n"
           "  quit              exit\n");
}

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
        }
        else
            path = arg;
    }
    if (path.empty()) {
        fprintf(stderr, "usage: vst3jackhost <plugin.vst3> [--uid <class-UID>] [--list]\n");
        return 1;
    }

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

    IPtr<Vst::PlugProvider> plugProvider;
    std::string className;
    for (auto &classInfo : factory.classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass)
            continue;
        if (effectID && *effectID != classInfo.ID())
            continue;
        plugProvider = owned(new Vst::PlugProvider(factory, classInfo, true));
        className = classInfo.name();
        break;
    }
    if (!plugProvider || !plugProvider->initialize()) {
        fprintf(stderr, "no matching audio-effect class in '%s' (try --list)\n", path.c_str());
        return 1;
    }

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

    printf("loaded '%s' from %s\n", className.c_str(), path.c_str());
    printf("JACK client '%s' is running; type 'help' for commands.\n", className.c_str());

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
            printHelp();
        }
        else if (cmd == "list") {
            printClasses(factory);
        }
        else if (cmd == "params") {
            printParams(controller);
        }
        else if (cmd == "get") {
            unsigned long id;
            if (iss >> id)
                printParam(controller, static_cast<Vst::ParamID>(id));
            else
                printf("usage: get <id>\n");
        }
        else if (cmd == "set") {
            unsigned long id;
            double value;
            if (iss >> id >> value) {
                if (value < 0.0 || value > 1.0) {
                    printf("value must be normalized (0..1)\n");
                    continue;
                }
                auto paramID = static_cast<Vst::ParamID>(id);
                // Keep the controller's view in sync, then hand the change to
                // the RT thread through the lock-free transfer.
                controller->setParamNormalized(paramID, value);
                audioClient->setParameter(paramID, value, 0);
                printParam(controller, paramID);
            }
            else {
                printf("usage: set <id> <normalized 0..1>\n");
            }
        }
        else {
            printf("unknown command '%s' (try 'help')\n", cmd.c_str());
        }
    }

    printf("shutting down\n");
    audioClient.reset();
    return 0;
}
