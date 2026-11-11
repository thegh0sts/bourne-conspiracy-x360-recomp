// rexgpu-regpu for The Bourne Conspiracy: ReGPU (a Direct3D 12 Xenos renderer) as a ReXGlue
// GPU plugin, selected with --gpu_plugin=regpu. Experimental.
//
// Adapted from Glue2GPU's plugin (https://github.com/zer0ducksgiven/Glue2GPU, MIT,
// licenses/Glue2GPU-LICENSE.txt), which targets Forza Horizon. Its Forza-only parts are left
// out: the graphics-screen bridge, the ambient occlusion and shadow shader rewrites, the shader
// pre-scan with its boot screen, and the shipped pipeline list. What stays is the host plumbing
// (memory, guest threads, interrupts, registers, vblank) and ReGPU's general switches.
//
// Settings, read from tbc's own command line (the launcher passes them):
//   --resolution_scale 1|2|3   render at 1280x720 times the scale
//   --frame_rate N             frame cap (0: none); src/fps_unlock.cpp sets the game's pacing
//   --vsync=true|false         wait for the display's refresh
//   --anisotropic_override N   -1 the game's filtering, 0 off, 1-5 1x-16x
// Environment overrides for testing: GLUE2GPU_RES=WxH, GLUE2GPU_UPSCALER=fsr1 with
// GLUE2GPU_QUALITY=0-4, GLUE2GPU_VSYNC=0|1, GLUE2GPU_WRITEBACK=some|all|now|none,
// GLUE2GPU_OWN_WINDOW=1, and every REGPU_* switch (see ReGPU's README).
// TBC_REGPU_OFF: switches this plugin turns on by default to leave off, comma-separated without
// the REGPU_ prefix (TBC_REGPU_OFF=LEAN_FETCH,TEXTURE_GAMMA), or "all" for ReGPU's own defaults.
// (Some REGPU_* switches count as on when merely set, so REGPU_X=0 doesn't turn them off.)
#include "regpu_graphics_system.h"
#include "stack_dump.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/ui/window.h>

#include <regpu/driver.h>
#include <regpu/regpu.h>

// Settings the stock xenos plugin defines (it isn't loaded alongside this one), so the
// launcher's switches mean something here too. src/gpu_preload.cpp loads this DLL before the
// command line is read, so their values come from it.
REXCVAR_DEFINE_BOOL(vsync, false, "Display", "Wait for the display's refresh (ReGPU)");
REXCVAR_DEFINE_INT32(anisotropic_override, -1, "Display",
                     "Texture filtering: -1 the game's, 0 off, 1-5 1x-16x anisotropic (ReGPU)");
REXCVAR_DEFINE_STRING(resolution_scale, "1", "Display",
                      "Render scale: 1 1280x720, 2 2560x1440, 3 3840x2160 (ReGPU)");

namespace glue2gpu
{
namespace
{
constexpr uint32_t kRegisterBase = 0x7FC80000;
constexpr wchar_t kTitle[] = L"The Bourne Conspiracy (ReGPU)";

// ReGPU's Host takes plain function pointers, so its callbacks reach ReXGlue through these.
rex::memory::Memory* g_memory = nullptr;
rex::runtime::FunctionDispatcher* g_dispatcher = nullptr;
rex::system::KernelState* g_kernel = nullptr;

// ReGPU's threads (vblank interrupt, command processor) are guest threads: they need a
// PPCContext, and the interrupt one calls the title's callback on it.
std::mutex g_threadsMutex;
std::vector<rex::system::object_ref<rex::system::XHostThread>> g_threads;

std::atomic<bool> g_running{ false };
std::atomic<uint64_t> g_swaps{ 0 };     // frames presented (title bar frame rate)
std::atomic<uint64_t> g_swapCount{ 0 }; // ReGPU's frame number (write-back ageing)

bool EnvSet(const char* name) { return std::getenv(name) != nullptr; }

// tbc's frame_rate: 0 is "no cap". ReGPU needs a number, so that becomes a high one.
uint32_t FrameCap(const std::string& value)
{
    const int fps = value.empty() ? 30 : std::atoi(value.c_str());
    return fps <= 0 ? 1000u : uint32_t(std::max(fps, 30));
}

// The settings that can change while the game runs (a later in-game menu, or the debug
// console): frame cap, vsync and texture filtering.
void FollowSettings()
{
    static std::string lastRate = "?", lastVsync = "?", lastAniso = "?";
    const std::string rate = rex::cvar::GetFlagByName("frame_rate"), vsync = rex::cvar::GetFlagByName("vsync");
    if (rate != lastRate || vsync != lastVsync)
    {
        lastRate = rate;
        lastVsync = vsync;
        bool on = vsync == "true";
        if (const char* v = std::getenv("GLUE2GPU_VSYNC"))
            on = *v == '1';
        regpu::SetFrameLimit(FrameCap(rate), on);
        REXLOG_INFO("tbc-regpu: frame cap {} fps, vsync {}", FrameCap(rate), on);
    }
    if (const std::string aniso = rex::cvar::GetFlagByName("anisotropic_override"); aniso != lastAniso)
    {
        lastAniso = aniso;
        const int level = aniso.empty() ? -1 : std::atoi(aniso.c_str());
        regpu::SetAnisotropy(level <= 0 ? level : 1 << (std::min(level, 5) - 1));
        REXLOG_INFO("tbc-regpu: texture filtering {}", aniso);
    }
}

// The window title shows the renderer and its frame rate. SetWindowText waits for the
// window's thread, so it runs on a thread of its own (which also follows the settings).
void StartTitleThread(HWND hwnd)
{
    std::thread([hwnd] {
        auto last = std::chrono::steady_clock::now();
        uint64_t lastSwaps = g_swaps.load();
        if (hwnd)
            SetWindowTextW(hwnd, kTitle);
        while (g_running.load() && (!hwnd || IsWindow(hwnd)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            FollowSettings();
            if (!hwnd)
                continue;
            const auto now = std::chrono::steady_clock::now();
            const uint64_t swaps = g_swaps.load();
            const double fps = double(swaps - lastSwaps) / std::chrono::duration<double>(now - last).count();
            last = now;
            lastSwaps = swaps;
            wchar_t title[128];
            swprintf_s(title, L"%s - %.0f fps", kTitle, fps);
            SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(title), SMTO_ABORTIFHUNG, 200, nullptr);
        }
    }).detach();
}

uint32_t Alloc(bool physical, uint32_t size, uint32_t align)
{
    if (!physical)
        return g_memory->SystemHeapAlloc(size, align);
    // The 0xA0000000 view (64 KB pages), where ReGPU expects physical memory. As
    // MmAllocatePhysicalMemoryEx does: whole pages, over the heap's whole range.
    constexpr uint32_t kPage = 64 * 1024;
    auto* heap = g_memory->LookupHeapByType(true, kPage);
    const uint32_t base = heap->heap_base();
    uint32_t address = 0;
    if (!heap->AllocRange(base, base + heap->heap_size() - 1, (size + kPage - 1) & ~(kPage - 1),
                          std::max(align, kPage),
                          rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                          /*top_down=*/true, &address))
    {
        REXLOG_ERROR("regpu: physical alloc of {:#x} bytes (align {:#x}) failed", size, align);
        return 0;
    }
    REXLOG_INFO("regpu: physical alloc {:#x} bytes at {:08X}", size, address);
    return address;
}

void CreateThread(const char* name, void (*body)(PPCContext&))
{
    auto* thread = new rex::system::XHostThread(g_kernel, 128 * 1024, 0, [body]() {
        body(*rex::system::XThread::GetCurrentThread()->thread_state()->context());
        return 0;
    });
    thread->set_name(name);
    thread->Create();
    std::lock_guard lock(g_threadsMutex);
    g_threads.emplace_back(thread);
}

uint32_t CurrentPcr()
{
    return rex::system::XThread::GetCurrentThread()->pcr_ptr();
}

// ReGPU sets r3/r4 (source, user data) and calls this between interruptLockAcquire/Release.
// ExecuteInterrupt takes the dispatcher's global lock and sets the arguments itself, so the
// lock hooks are empty.
void GuestCall(PPCContext& ctx, uint32_t address)
{
    auto* thread = rex::system::XThread::GetCurrentThread();
    uint64_t args[] = { ctx.r3.u64, ctx.r4.u64 };
    g_dispatcher->ExecuteInterrupt(thread->thread_state(), address, args, 2);
}

void InterruptLockAcquire(int) {}
void InterruptLockRelease() {}

// Which resolves (EDRAM copies) are also written to guest memory, for anything the game reads
// on the CPU. GLUE2GPU_WRITEBACK: "some" (default, like ReXGlue's readback_resolve=some: an
// image the first time it is resolved, and again after 60 frames without it), "all" (every
// one, deferred; slow), "now" (every one, at once; slower), "none".
regpu::Writeback ResolveWriteback(const regpu::ResolveCopy& copy)
{
    static const std::string mode = std::getenv("GLUE2GPU_WRITEBACK") ? std::getenv("GLUE2GPU_WRITEBACK") : "some";
    regpu::Writeback w;
    if (mode == "none")
        return w;
    if (mode == "some")
    {
        static std::mutex mutex;
        static std::unordered_map<uint64_t, uint64_t> lastSeen; // image -> frame of its last resolve
        const uint64_t key = uint64_t(copy.base) << 32 ^ uint64_t(copy.width) << 18 ^ uint64_t(copy.height) << 6 ^
                             copy.format ^ (copy.depth ? 1ull << 63 : 0);
        const uint64_t now = g_swapCount.load(std::memory_order_relaxed);
        std::lock_guard lock(mutex);
        auto [it, fresh] = lastSeen.try_emplace(key, now);
        const bool stale = !fresh && now - it->second > 60;
        it->second = now;
        if (!fresh && !stale)
            return w;
    }
    w.mode = mode == "now" ? regpu::Writeback::Mode::Now : regpu::Writeback::Mode::Deferred;
    return w;
}

// ReXGlue's VdQueryVideoMode reports the refresh rate the title paces itself by, so ReGPU's
// vblank follows it, as ReXGlue's own vsync thread does.
void RefreshRateLoop()
{
    while (g_running)
    {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        regpu::driver::SetRefreshRate(std::max(1.0, double(float(mode.refresh_rate))));
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// ReGPU switches that are off by default but that Glue2GPU found right for ReXGlue hosts.
// Each can still be turned off by setting it to 0 in the environment before starting.
// Whether TBC_REGPU_OFF leaves this default switch off ("REGPU_" + one of its names, or "all").
bool LeftOff(const char* name)
{
    const char* list = std::getenv("TBC_REGPU_OFF");
    if (!list || !*list)
        return false;
    std::string want = name;
    if (want.rfind("REGPU_", 0) == 0)
        want = want.substr(6);
    std::string item;
    for (const char* p = list;; ++p)
    {
        if (*p == ',' || *p == ';' || *p == ' ' || *p == 0)
        {
            for (char& c : item)
                c = char(std::toupper(static_cast<unsigned char>(c)));
            if (item == "ALL" || item == want || item == "REGPU_" + want)
                return true;
            item.clear();
            if (*p == 0)
                return false;
        }
        else
            item += *p;
    }
}

void DefaultSwitches()
{
    if (const char* off = std::getenv("TBC_REGPU_OFF"); off && *off)
        REXLOG_INFO("tbc-regpu: TBC_REGPU_OFF={}", off);
    // Read when ReGPU loads (before this runs), so set through the driver.
    const auto driverSwitch = [](const char* name) {
        if (!EnvSet(name) && !LeftOff(name))
            regpu::driver::SetSwitch(name, true);
    };
    // Read when ReGPU starts.
    const auto envSwitch = [](const char* name) {
        if (!EnvSet(name) && !LeftOff(name))
            _putenv_s(name, "1");
    };
    // Gamma-signed texture fetches and gamma-encoding resolves (k_8_8_8_8_GAMMA), as the console
    // does; without them the picture comes out washed out.
    envSwitch("REGPU_TEXTURE_GAMMA");
    driverSwitch("REGPU_RESOLVE_GAMMA");
    // A draw whose shaders or pipeline are still compiling is skipped instead of waited for, so
    // a first boot doesn't stall the game for long stretches.
    driverSwitch("REGPU_ASYNC_SHADERS");
    driverSwitch("REGPU_ASYNC_PIPELINES");
    // ...except opaque draws once the game has settled: something drawn only once (a texture
    // the game renders and keeps) would otherwise be lost for good.
    driverSwitch("REGPU_NO_SETTLED_SKIP");
    // Tiled frames drawn tile by tile, as the game issues them.
    driverSwitch("REGPU_NO_TILE_MERGE");
    // Anisotropic filtering as each texture fetch asks for it (as the console), not 16x everywhere.
    driverSwitch("REGPU_ANISO_FROM_FETCH");
    // Vertex fetches not clamped to the fetch constant's size (as the console and Xenia).
    driverSwitch("REGPU_NO_FETCH_BOUNDS");
    // Resolves land at the image's own base address.
    envSwitch("REGPU_RESOLVE_IMAGE_BASE");
    // Fences written when the command processor reaches them, not after the host GPU: the game
    // no longer waits on the GPU every frame.
    if (!EnvSet("REGPU_EARLY_FENCES") && !LeftOff("REGPU_EARLY_FENCES"))
    {
        _putenv_s("REGPU_EARLY_FENCES", "1");
        regpu::driver::SetSwitch("REGPU_EARLY_FENCES", true);
    }
    // Draws translated on their own thread while the command processor parses on.
    envSwitch("REGPU_DRAW_THREAD");
    // One sample per 2D texture fetch (cheaper full-screen post-processing).
    envSwitch("REGPU_LEAN_FETCH");
}
} // namespace

ReGpuGraphicsSystem::~ReGpuGraphicsSystem() { Shutdown(); }

X_STATUS ReGpuGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext*)
{
    // ReGPU creates its device in regpu::Start(), which needs Host filled: that waits for
    // SetupGuestGpu.
    presentation_ = true;
    return X_STATUS_SUCCESS;
}

void ReGpuGraphicsSystem::FillHost()
{
    regpu::Host& host = regpu::GetHost();
    host.base = g_memory->virtual_membase();
    // ReXGlue doesn't expose its physical memory mapping's handle, so ReGPU opens the
    // 0xA0000000 view by address, or falls back to copying.
    host.physicalSection = nullptr;
    host.alloc = Alloc;
    host.createThread = CreateThread;
    host.currentPcr = CurrentPcr;
    host.guestCall = GuestCall;
    host.interruptLockAcquire = InterruptLockAcquire;
    host.interruptLockRelease = InterruptLockRelease;
    host.logThreadStacks = LogThreadStacks;
    host.resolveWriteback = ResolveWriteback;
    host.onSwap = [](uint64_t frame) {
        g_swapCount.store(frame, std::memory_order_relaxed);
        g_swaps.fetch_add(1, std::memory_order_relaxed);
    };
}

X_STATUS ReGpuGraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                                            rex::system::KernelState* kernel_state)
{
    g_dispatcher = dispatcher;
    g_memory = dispatcher->memory();
    g_kernel = kernel_state;
    FillHost();

    regpu::Config& config = regpu::GetConfig();
    // Render size: the console's 1280x720 times the render scale. ReGPU draws the scene and
    // the HUD at this size and stretches it to the window.
    const int scale = std::clamp(std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str()), 1, 3);
    config.width = 1280u * uint32_t(scale);
    config.height = 720u * uint32_t(scale);
    if (const char* res = std::getenv("GLUE2GPU_RES"))
        std::sscanf(res, "%ux%u", &config.width, &config.height);
    // GLUE2GPU_UPSCALER=fsr1 (fsr3 and dlss need ReGPU built with them), GLUE2GPU_QUALITY=0-4
    // (native, quality, balanced, performance, ultra performance).
    if (const char* up = std::getenv("GLUE2GPU_UPSCALER"))
    {
        const std::string u = up;
        config.upscaler = u == "fsr1" ? regpu::Config::Upscaler::Fsr1
                        : u == "fsr3" ? regpu::Config::Upscaler::Fsr3
                        : u == "dlss" ? regpu::Config::Upscaler::Dlss
                                      : regpu::Config::Upscaler::Off;
    }
    if (const char* q = std::getenv("GLUE2GPU_QUALITY"))
        config.quality = uint32_t(std::atoi(q));
    config.vsync = rex::cvar::GetFlagByName("vsync") == "true";
    if (const char* v = std::getenv("GLUE2GPU_VSYNC"))
        config.vsync = *v == '1';
    config.maxFps = FrameCap(rex::cvar::GetFlagByName("frame_rate"));
    config.windowTitle = kTitle;
    REXLOG_INFO("tbc-regpu: render {}x{}, frame cap {}, vsync {}", config.width, config.height, config.maxFps,
                config.vsync);

    // ReGPU presents into ReXGlue's game window (which keeps input, focus, closing and
    // fullscreen). GLUE2GPU_OWN_WINDOW=1: ReGPU opens a window of its own instead.
    if (!EnvSet("GLUE2GPU_OWN_WINDOW"))
        if (auto* runtime = rex::Runtime::instance(); runtime && runtime->display_window())
            config.hostWindow = runtime->display_window()->GetNativeWindowHandle();

    // Shader and pipeline caches: <cache_root>/regpu. ReXGlue calls InitializeShaderStorage
    // after this, so the root comes from its setting.
    if (cacheRoot_.empty())
        if (const std::string root = rex::cvar::GetFlagByName("cache_root"); !root.empty())
            cacheRoot_ = root;
    if (!cacheRoot_.empty())
        config.userDir = cacheRoot_ / "regpu";

    // Same range, mask and size as ReXGlue's own GraphicsSystem.
    g_memory->AddVirtualMappedRange(
        kRegisterBase, 0xFFFF0000, 0x0000FFFF, this,
        reinterpret_cast<rex::runtime::MMIOReadCallback>(ReadRegisterThunk),
        reinterpret_cast<rex::runtime::MMIOWriteCallback>(WriteRegisterThunk));

    DefaultSwitches();

    regpu::Start();
    started_ = true;
    g_running = true;
    std::thread(RefreshRateLoop).detach();
    StartTitleThread(static_cast<HWND>(config.hostWindow));
    return X_STATUS_SUCCESS;
}

uint32_t ReGpuGraphicsSystem::ReadRegisterThunk(void*, ReGpuGraphicsSystem*, uint32_t addr)
{
    return regpu::driver::ReadRegister((addr & 0xFFFF) >> 2);
}

void ReGpuGraphicsSystem::WriteRegisterThunk(void*, ReGpuGraphicsSystem*, uint32_t addr, uint32_t value)
{
    regpu::driver::WriteRegister((addr & 0xFFFF) >> 2, value);
}

void ReGpuGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data)
{
    regpu::driver::SetInterruptCallback(callback, user_data);
}

void ReGpuGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2)
{
    regpu::driver::InitializeRingBuffer(ptr, size_log2);
}

void ReGpuGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2)
{
    regpu::driver::EnableReadPointerWriteBack(ptr, block_size_log2);
}

void ReGpuGraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t, bool)
{
    cacheRoot_ = cache_root;
}

void ReGpuGraphicsSystem::Shutdown()
{
    if (!started_)
        return;
    started_ = false;
    g_running = false;
    regpu::SaveCaches();
    // ReGPU has no Stop(); its threads run until the process exits.
}
} // namespace glue2gpu
