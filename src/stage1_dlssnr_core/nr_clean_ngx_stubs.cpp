// Clean stubs for dlssnr_core.dll (2077FSR Stage 1):
// Disables NR Control DXGI Present hook, input/cursor MinHook thread, D3D12 passthrough, and OptiScaler binary patching.
#include "nr_pe_menu.hpp"
#include "nr_pe_control.hpp"
#include "nr_pe_optifix.hpp"
#include "nr_d3d12_passthrough.hpp"
#include "nr_d3d12_passthrough_backend.hpp"
#include "nr_control_shared.hpp"

namespace nr::pe {
void fix_optiscaler() {}
}  // namespace nr::pe

namespace nr::pe::menu {
void install() {}
void set_extra_diagnostics(void (*)()) {}
void set_extra_network(void (*)()) {}
void watch_queues(ID3D12Device*) {}
void on_d3d12_present(D3D12PresentHook) {}
void on_d3d12_shown(D3D12ShownHook) {}
void note_swapchain_queue(IDXGISwapChain*, ID3D12CommandQueue*) {}
void on_d3d11_present(D3D11PresentHook) {}
void on_execute(ExecuteObserver) {}
void draw_game(IDXGISwapChain*, IUnknown*, bool) {}
bool colour_space_of(IDXGISwapChain*, int*) { return false; }
void abandon_at_exit() {}
const VkPresentInfoKHR* vulkan_present(VkInstance, VkPhysicalDevice, VkDevice, VkQueue, uint32_t,
                                       VkSwapchainKHR, VkFormat, VkColorSpaceKHR, VkExtent2D,
                                       const std::vector<VkImage>&, const VkPresentInfoKHR* info) {
    return info;
}
void vulkan_forget(VkDevice, VkSwapchainKHR) {}
}  // namespace nr::pe::menu

namespace nr::pe::control {
nr::control::Shared* page() {
    static nr::control::Shared s = [] {
        nr::control::Shared v{};
        v.version = nr::control::kVersion;
        v.magic = nr::control::kMagic;
        v.net_running = v.net_want = v.building_net = 3;
        return v;
    }();
    return &s;
}
void report(const Status&) {}
void start_input() {}
void menu_window(void*) {}
void chain_layers(const std::vector<int>&) {}
void layer_ran(int, uint32_t, uint32_t, uint32_t, const char*) {}
void layer_changed(int) {}
void present_layers() {}
void asi_facts(const char*, const char*) {}
void hud_facts(const char*, const char*, const char*, const char*) {}
}  // namespace nr::pe::control

namespace nr::pe::d3d12_passthrough {
const char* canary_name(Canary) { return "disabled"; }
void start() {}
bool enabled() { return false; }
void on_device(ID3D12Device*) {}
Canary canary() { return Canary::kNotRun; }
nr::arch::GpuArch arch() { return nr::arch::GpuArch::unknown; }
uint32_t gfx11_mach() { return 0; }
void set_gfx11_mach(uint32_t) {}
void set_enabled(bool) {}
std::wstring aco_folder() { return {}; }
Status status() { return {}; }
void note_backend(const char*, bool, bool) {}
void note_backend_produced(bool) {}
bool backend_produced() { return false; }
void note_network(const char*) {}
void draw_network() {}
void draw_panel() {}
PassthroughNetStatus backend_net_status() { return {}; }
void release_renodx_feature(uint64_t) {}
bool evaluate_backend(const nr::dlssnr::D3d12BackendFrame&) { return false; }
bool wilsjo2_ready(ID3D12Device*, ID3D12GraphicsCommandList*, uint32_t, uint32_t, std::string*, bool*) {
    return false;
}
void abandon_at_exit() {}
}  // namespace nr::pe::d3d12_passthrough
