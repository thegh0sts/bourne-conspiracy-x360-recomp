// From Glue2GPU (https://github.com/zer0ducksgiven/Glue2GPU, MIT, licenses/Glue2GPU-LICENSE.txt).
#pragma once
// rexgpu-regpu: ReGPU behind ReXGlue's GPU plugin interface (rex/system/gpu_plugin.h, ABI 1).
#include <vector>

#include <rex/system/interfaces/graphics.h>

namespace glue2gpu
{
using rex::X_STATUS;

class ReGpuGraphicsSystem final : public rex::system::IGraphicsSystem
{
public:
    ReGpuGraphicsSystem() = default;
    ~ReGpuGraphicsSystem() override;

    // ReGPU presents into ReXGlue's window itself. provider()/presenter() stay null, so the
    // runtime's ImGui overlays (F3/F4 and the like) are off with this renderer.
    X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
    X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                                rex::system::KernelState* kernel_state) override;
    bool has_presentation() const override { return presentation_; }

    void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
    void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;
    void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                 bool blocking) override;
    void Shutdown() override;

private:
    static uint32_t ReadRegisterThunk(void* ppc_context, ReGpuGraphicsSystem* self, uint32_t addr);
    static void WriteRegisterThunk(void* ppc_context, ReGpuGraphicsSystem* self, uint32_t addr,
                                   uint32_t value);

    void FillHost();

    bool presentation_ = false;
    bool started_ = false;
    std::filesystem::path cacheRoot_;
};
} // namespace glue2gpu
