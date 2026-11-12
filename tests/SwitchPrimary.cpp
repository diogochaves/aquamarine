// Manual check for CBackend::switchPrimary(): needs a seat with two KMS GPUs, so it is not a ctest.
// Draws solid red on every output, makes the second GPU the primary, then draws green.
// usage: switchPrimary [seconds per phase = 3] [back|release]
//   back:    a third phase, blue, with the first GPU as the primary again
//   release: after the switch, releaseGpu(first) (phase 2, blue, on what is left), then acquireGpu() it
//            and make it the primary again (phase 3, white)

#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/output/Output.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <poll.h>
#include <string>
#include <xf86drm.h>

using namespace Hyprutils::Signal;
using namespace Hyprutils::Memory;
using namespace Aquamarine;
#define SP CSharedPointer

struct SOut {
    SP<IOutput>         output;
    CHyprSignalListener frame, needsFrame, destroy;
    bool                dead = false;
    std::map<int, int>  ok, failed, drawn; // per phase
};

static SP<CBackend>      backend;
static std::vector<SOut> outs;
static int               phase     = 0;
static const uint32_t    COLORS[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF};

static void              log(eBackendLogLevel level, std::string msg) {
    if (level >= AQ_LOG_DEBUG)
        std::cout << "[AQ " << level << "] " << msg << "\n";
}

static std::string nodeOf(int fd) {
    char*       n = fd >= 0 ? drmGetDeviceNameFromFd2(fd) : nullptr;
    std::string s = n ? n : "none";
    free(n);
    return s;
}

// fds of this process that point at a device node whose path starts with prefix
static int fdsOn(const std::string& prefix) {
    int             n = 0;
    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
        const auto TARGET = std::filesystem::read_symlink(e.path(), ec);
        if (!ec && TARGET.string().starts_with(prefix))
            n++;
    }
    return n;
}

static void render(SOut& o) {
    if (o.dead)
        return;

    const auto MODE = o.output->state->state().mode;
    if (!MODE)
        return;

    auto opts = o.output->swapchain->currentOptions();
    if (opts.length != 2 || opts.size != MODE->pixelSize) {
        opts.length = 2;
        opts.size   = MODE->pixelSize;
        opts.format = DRM_FORMAT_XRGB8888;
        if (!o.output->swapchain->reconfigure(opts)) {
            o.failed[phase]++;
            return;
        }
    }

    auto buf = o.output->swapchain->next(nullptr);
    if (!buf) {
        o.failed[phase]++;
        return;
    }

    auto [ptr, fmt, len] = buf->beginDataPtr(0x2 /* GBM_BO_TRANSFER_WRITE */);
    if (ptr) {
        for (size_t i = 0; i + 4 <= len; i += 4)
            memcpy(ptr + i, &COLORS[phase], 4);
        o.drawn[phase]++;
    }
    buf->endDataPtr();

    o.output->state->setBuffer(buf);
    if (o.output->commit())
        o.ok[phase]++;
    else
        o.failed[phase]++;
}

static void onNewOutput(SP<IOutput> output) {
    std::cout << "[test] new output " << output->name << " on " << nodeOf(output->getBackend()->drmFD()) << "\n";

    SP<SOutputMode> mode;
    for (auto const& m : output->modes) {
        if (!mode || m->preferred)
            mode = m;
    }
    if (!mode)
        return;

    output->state->setEnabled(true);
    output->state->setMode(mode);
    output->state->setFormat(DRM_FORMAT_XRGB8888);

    auto& o    = outs.emplace_back(SOut{.output = output});
    auto  NAME = output->name;
    auto  find = [NAME]() -> SOut* {
        for (auto& x : outs) {
            if (!x.dead && x.output->name == NAME)
                return &x;
        }
        return nullptr;
    };
    o.frame      = output->events.frame.listen([find] { render(*find()); });
    o.needsFrame = output->events.needsFrame.listen([find] { find()->output->scheduleFrame(); });
    o.destroy    = output->events.destroy.listen([find] {
        std::cout << "[test] output " << find()->output->name << " destroyed\n";
        find()->dead = true;
    });

    if (backend->ready)
        render(o);
}

static void dispatchFor(std::chrono::milliseconds dur) {
    const auto END = std::chrono::steady_clock::now() + dur;
    while (std::chrono::steady_clock::now() < END) {
        auto                fds = backend->getPollFDs();
        std::vector<pollfd> pfds;
        for (auto const& f : fds)
            pfds.push_back({.fd = f->fd, .events = POLLIN});
        if (poll(pfds.data(), pfds.size(), 50) <= 0)
            continue;
        for (size_t i = 0; i < pfds.size(); ++i) {
            if (pfds[i].revents & POLLIN)
                fds[i]->onSignal();
        }
    }
}

int main(int argc, char** argv) {
    const auto      SECS = std::chrono::seconds(argc > 1 ? std::stoi(argv[1]) : 3);
    const bool      BACK    = argc > 2 && std::string(argv[2]) == "back";
    const bool      RELEASE = argc > 2 && std::string(argv[2]) == "release";

    SBackendOptions options;
    options.logFunction = log;
    SBackendImplementationOptions drm;
    drm.backendType        = AQ_BACKEND_DRM;
    drm.backendRequestMode = AQ_BACKEND_REQUEST_MANDATORY;

    backend       = CBackend::create({drm}, options);
    auto newOut   = backend->events.newOutput.listen(onNewOutput);
    int  switches = 0;
    auto changed  = backend->gpuEvents.primaryChanged.listen([&] { switches++; });

    if (!backend->start()) {
        std::cout << "[test] FAIL: backend didn't start\n";
        return 1;
    }

    SP<IBackendImplementation> first, second;
    for (auto const& i : backend->getImplementations()) {
        if (i->type() != AQ_BACKEND_DRM)
            continue;
        if (!first)
            first = i;
        else if (!second)
            second = i;
    }
    if (!second) {
        std::cout << "[test] FAIL: needs two GPUs\n";
        return 1;
    }

    const auto NODE0 = nodeOf(first->drmFD()), NODE1 = nodeOf(second->drmFD());
    bool       pass  = true;
    auto       check = [&](const std::string& what, bool ok) {
        std::cout << "[test] " << (ok ? "ok  " : "FAIL") << ": " << what << "\n";
        pass = pass && ok;
    };
    auto state = [&](const std::string& want) {
        check(std::format("phase {}: drmFD() is {} (want {})", phase, nodeOf(backend->drmFD()), want), nodeOf(backend->drmFD()) == want);
        check(std::format("phase {}: primaryAllocator is on {}", phase, nodeOf(backend->primaryAllocator->drmFD())), nodeOf(backend->primaryAllocator->drmFD()) == want);
        for (auto& o : outs)
            if (!o.dead)
                check(std::format("phase {}: {}: {} commits ok, {} failed, {} drawn by CPU map", phase, o.output->name, o.ok[phase], o.failed[phase], o.drawn[phase]),
                      o.ok[phase] > 10 && o.failed[phase] <= 2);
    };

    for (auto& o : outs)
        render(o);
    dispatchFor(SECS);
    state(NODE0);

    phase = 1;
    check("switchPrimary(second)", backend->switchPrimary(second));
    check("primaryChanged emitted once", switches == 1);
    check("second has no primary, first points at second", !second->getPrimary() && first->getPrimary().lock() == second);
    dispatchFor(SECS);
    state(NODE1);

    if (BACK) {
        phase = 2;
        check("switchPrimary(first)", backend->switchPrimary(first));
        check("primaryChanged emitted twice", switches == 2);
        dispatchFor(SECS);
        state(NODE0);
    }

    if (RELEASE) {
        // by-path names, so the count doesn't depend on which card number the GPU got
        const auto  CARD0 = NODE0;
        std::string render0;
        for (auto const& e : std::filesystem::directory_iterator("/dev/dri/by-path")) {
            if (!e.path().string().ends_with("-card") || std::filesystem::canonical(e.path()).string() != CARD0)
                continue;
            auto r  = e.path().string();
            render0 = std::filesystem::canonical(r.substr(0, r.size() - 4) + "render").string();
        }
        auto fds = [&](const std::string& when) {
            std::cout << std::format("[test] fds {}: {} on {}, {} on {}, {} on /dev/nvidia*\n", when, fdsOn(CARD0), CARD0, fdsOn(render0), render0, fdsOn("/dev/nvidia"));
        };

        phase = 2;
        fds("before release");
        check("releaseGpu(second) is refused: it is the primary", !backend->releaseGpu(second));
        check("releaseGpu(first)", backend->releaseGpu(first));
        fds("after release");
        check(std::format("no fd left on {}", CARD0), fdsOn(CARD0) == 0);
        check("one DRM implementation left", backend->getImplementations().size() == 1);
        check("drmFD() still the second GPU", nodeOf(backend->drmFD()) == NODE1);
        backend->onNewGpu(NODE0);
        check("a hot-add of the released GPU is ignored", backend->getImplementations().size() == 1);
        first.reset();
        fds("after the last reference");
        dispatchFor(SECS);
        state(NODE1);

        phase = 3;
        check("acquireGpu(first)", backend->acquireGpu(NODE0));
        check("two DRM implementations again", backend->getImplementations().size() == 2);
        first = backend->getImplementations().back();
        check("the acquired GPU has the second as its primary", first->getPrimary().lock() == second);
        dispatchFor(std::chrono::seconds(1));
        check("switchPrimary(first) after acquire", backend->switchPrimary(first));
        dispatchFor(SECS);
        state(NODE0);
        fds("at the end");
    }

    std::cout << "[test] " << (pass ? "PASS" : "FAILED") << "\n";
    outs.clear();
    backend.reset();
    return pass ? 0 : 1;
}
