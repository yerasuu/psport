// SPDX-License-Identifier: GPL-2.0-or-later
// psport: FSR 4 benchmark outside the game. Runs the v07 INT8 provider (the same assets and
// provider as vk_fsr4.cpp) on synthetic inputs and prints GPU time per pass (PS_FSR4_PROFILE)
// and, with --stats, the driver's statistics of every pass (PS_FSR4_STATS).
//
//   fsr4-bench [render WxH] [output WxH] [preset 0-4] [frames] [--stats] [--fsr411]
// --fsr411: FSR 4.1.1 replay (fsr411.cpp, assets in PS_FSR411_DIR or fsr4_411) instead of v07;
// its frames match tools/fsr4cap (jitter phase, reset on the first frame).
//   defaults: 2260x1272 3840x2160 2 (balanced) 900
// BENCH_NOISE=1: pseudo-random inputs (a fixed seed); BENCH_DUMP=<file>: the output after the
// last frame, raw RGBA16F, for comparing shader variants.
// Assets: PS_FSR4_DIR or fsr4_shaders in the working directory.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "ffx_vk_fsr4_v07.h"
#include "ffx_vk_fsr4_v07_assets.h"
#include "fsr411.h"

namespace {

#define CHECK(call)                                                                            \
    do {                                                                                       \
        const VkResult r_ = (call);                                                            \
        if (r_ != VK_SUCCESS) {                                                                \
            std::fprintf(stderr, "%s failed: %d\n", #call, int(r_));                           \
            std::exit(1);                                                                      \
        }                                                                                      \
    } while (0)

bool ReadFile(const std::string& path, std::vector<unsigned char>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    data.resize(size_t(file.tellg()));
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())));
}

struct Gpu {
    VkInstance h_instance{};
    VkPhysicalDevice h_physical{};
    VkDevice h_device{};
    VkQueue h_queue{};
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    Fsr411::Features fsr411; ///< what the device was created with for FSR 4.1.1's variants
};

uint32_t MemoryType(const Gpu& gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < gpu.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (gpu.memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    std::fprintf(stderr, "no memory type\n");
    std::exit(1);
}

Gpu CreateGpu(bool should_report_stats) {
    Gpu gpu;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "fsr4-bench";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, nullptr, &gpu.h_instance));
    uint32_t c_devices = 0;
    CHECK(vkEnumeratePhysicalDevices(gpu.h_instance, &c_devices, nullptr));
    std::vector<VkPhysicalDevice> devices(c_devices);
    CHECK(vkEnumeratePhysicalDevices(gpu.h_instance, &c_devices, devices.data()));
    gpu.h_physical = devices.at(0);
    for (const auto h_device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(h_device, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            gpu.h_physical = h_device;
            break;
        }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu.h_physical, &props);
    std::printf("GPU: %s\n", props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(gpu.h_physical, &gpu.memory);

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.h_physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.h_physical, &families, family_props.data());
    while (gpu.family < families && !(family_props[gpu.family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        ++gpu.family;
    }

    // Everything the device supports, as the game's device enables what FSR 4 needs.
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    // Cooperative matrix (WMMA) for experimental model passes, when the device has it.
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coopmat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    // FSR 4.1.1's FP8 variant: FP8 cooperative matrices (RDNA4).
    VkPhysicalDeviceShaderFloat8FeaturesEXT float8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    // FSR 4.1.1 passes use mixed float dot products (dot2 of halves into float), as vkd3d-proton.
    VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE mixed_dot{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    f13.pNext = &derivatives;
    derivatives.pNext = &coopmat;
    coopmat.pNext = &float8;
    float8.pNext = &mixed_dot;
    if (should_report_stats) {
        mixed_dot.pNext = &executable;
    }
    vkGetPhysicalDeviceFeatures2(gpu.h_physical, &features);
    features.features.robustBufferAccess = VK_FALSE; // as the game: no robustness cost
    f13.robustImageAccess = VK_FALSE;
    std::vector<const char*> extensions{VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME};
    extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (mixed_dot.shaderMixedFloatDotProductFloat16AccFloat32) {
        extensions.push_back(VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME);
    }
    if (coopmat.cooperativeMatrix) {
        extensions.push_back(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    }
    // RADV fills the FP8 features in also where it does not offer the extension (RDNA3).
    uint32_t c_available = 0;
    vkEnumerateDeviceExtensionProperties(gpu.h_physical, nullptr, &c_available, nullptr);
    std::vector<VkExtensionProperties> available(c_available);
    vkEnumerateDeviceExtensionProperties(gpu.h_physical, nullptr, &c_available, available.data());
    const bool has_float8 = std::any_of(available.begin(), available.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME) == 0;
    });
    if (!has_float8) {
        float8.shaderFloat8 = float8.shaderFloat8CooperativeMatrix = VK_FALSE;
    }
    if (float8.shaderFloat8CooperativeMatrix) {
        extensions.push_back(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
    } else {
        coopmat.pNext = float8.pNext;
    }
    // The matrix passes run as wave32 in full subgroups, with the Vulkan memory model.
    const bool has_matrices = coopmat.cooperativeMatrix && f12.vulkanMemoryModel && f13.subgroupSizeControl &&
                          f13.computeFullSubgroups;
    gpu.fsr411.has_fp8_matrices = has_matrices && float8.shaderFloat8CooperativeMatrix;
    gpu.fsr411.has_fp16_matrices = has_matrices && f12.shaderFloat16;
    if (should_report_stats) {
        extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gpu.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    CHECK(vkCreateDevice(gpu.h_physical, &dci, nullptr, &gpu.h_device));
    vkGetDeviceQueue(gpu.h_device, gpu.family, 0, &gpu.h_queue);
    return gpu;
}

struct Buffer {
    VkBuffer h_buffer{};
    VkDeviceMemory h_memory{};
    void* p_data = nullptr;
};

Buffer CreateHostBuffer(const Gpu& gpu, VkDeviceSize cb_buffer, VkBufferUsageFlags usage) {
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = cb_buffer;
    ci.usage = usage;
    CHECK(vkCreateBuffer(gpu.h_device, &ci, nullptr, &b.h_buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gpu.h_device, b.h_buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(gpu, req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(gpu.h_device, &ai, nullptr, &b.h_memory));
    CHECK(vkBindBufferMemory(gpu.h_device, b.h_buffer, b.h_memory, 0));
    CHECK(vkMapMemory(gpu.h_device, b.h_memory, 0, VK_WHOLE_SIZE, 0, &b.p_data));
    return b;
}

uint16_t Half(float f) {
    // Inputs in [0, 2): exact enough for test data (truncating conversion).
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = int((x >> 23) & 0xffu) - 127 + 15;
    if (exp <= 0) {
        return uint16_t(sign);
    }
    return uint16_t(sign | (uint32_t(exp) << 10) | ((x >> 13) & 0x3ffu));
}

struct Image {
    VkImage h_image{};
    VkImageView h_view{};
    VkDeviceMemory h_memory{};
    uint32_t width = 0, height = 0;
};

Image CreateImage(const Gpu& gpu, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect) {
    Image image;
    image.width = w;
    image.height = h;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    CHECK(vkCreateImage(gpu.h_device, &ci, nullptr, &image.h_image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(gpu.h_device, image.h_image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(gpu.h_device, &ai, nullptr, &image.h_memory));
    CHECK(vkBindImageMemory(gpu.h_device, image.h_image, image.h_memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image.h_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    CHECK(vkCreateImageView(gpu.h_device, &vi, nullptr, &image.h_view));
    return image;
}

FfxFsr4ModelPreset ModelPreset(int preset) {
    switch (preset) {
    case 0:
        return FFX_FSR4_MODEL_PRESET_NATIVE_AA;
    case 1:
        return FFX_FSR4_MODEL_PRESET_QUALITY;
    case 2:
        return FFX_FSR4_MODEL_PRESET_BALANCED;
    case 3:
        return FFX_FSR4_MODEL_PRESET_PERFORMANCE;
    default:
        return FFX_FSR4_MODEL_PRESET_ULTRA_PERFORMANCE;
    }
}

} // namespace

int main(int argc, char** argv) {
    uint32_t rw = 2260, rh = 1272, ow = 3840, oh = 2160;
    int preset = 2, frames = 900;
    bool should_report_stats = false;
    bool should_use_fsr411 = false;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--fsr411") == 0) {
            should_use_fsr411 = true;
            continue;
        }
        if (std::strcmp(argv[i], "--stats") == 0) {
            should_report_stats = true;
            continue;
        }
        switch (positional++) {
        case 0:
            std::sscanf(argv[i], "%ux%u", &rw, &rh);
            break;
        case 1:
            std::sscanf(argv[i], "%ux%u", &ow, &oh);
            break;
        case 2:
            preset = std::atoi(argv[i]);
            break;
        case 3:
            frames = std::atoi(argv[i]);
            break;
        }
    }
    setenv("PS_FSR4_PROFILE", "1", 0);
    if (should_report_stats) {
        setenv("PS_FSR4_STATS", "1", 1);
    }
    const Gpu gpu = CreateGpu(should_report_stats);

    FfxInterface backend{};
    ffxContext p_context{};
    std::vector<unsigned char> scratch;
    if (!should_use_fsr411) {
    // Assets, as vk_fsr4.cpp loads them.
    FfxFsr4V07AssetSet assets{};
    if (!ffxFsr4V07BuildAssetSet(ModelPreset(preset), ow, oh, &assets)) {
        std::fprintf(stderr, "unsupported output size\n");
        return 1;
    }
    const char* sz_dir_env = std::getenv("PS_FSR4_DIR");
    const std::string dir = std::string{sz_dir_env && sz_dir_env[0] ? sz_dir_env : "fsr4_shaders"} + "/";
    std::array<std::vector<unsigned char>, FFX_FSR4_VK_PASS_COUNT> code;
    std::vector<unsigned char> initializer, weights;
    // As vk_fsr4.cpp: passes from opt/ (tools/fsr4_optimize.sh) unless PS_FSR4_OPT=0.
    const char* sz_opt_env = std::getenv("PS_FSR4_OPT");
    const bool should_use_opt = !(sz_opt_env && sz_opt_env[0] == '0');
    int optimized = 0;
    const auto load = [&](const char* sz_name, std::vector<unsigned char>& data) {
        if (should_use_opt && ReadFile(dir + "opt/" + sz_name, data)) {
            ++optimized;
            return;
        }
        if (!ReadFile(dir + sz_name, data)) {
            std::fprintf(stderr, "missing %s%s\n", dir.c_str(), sz_name);
            std::exit(1);
        }
    };
    load(assets.pre, code[0]);
    for (uint32_t pass = 0; pass < FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
        load(assets.model[pass], code[1 + pass]);
    }
    load(assets.post, code[13]);
    load(assets.rcas, code[14]);
    load(assets.spdAutoExposure, code[15]);
    load(assets.initializer, initializer);
    load(assets.prePassWeights, weights);
    std::printf("%d passes from %sopt/\n", optimized, dir.c_str());
    std::array<std::string, FFX_FSR4_VK_PASS_COUNT> entries;
    entries.fill("main");
    for (uint32_t pass = 1; pass <= FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
        entries[pass] = "fsr4_model_v07_i8_pass" + std::to_string(pass);
    }
    FfxFsr4VkCreateInfo ci{};
    ci.device = gpu.h_device;
    ci.physicalDevice = gpu.h_physical;
    for (uint32_t i = 0; i < FFX_FSR4_VK_PASS_COUNT; ++i) {
        ci.shaders[i] = {reinterpret_cast<const uint32_t*>(code[i].data()), code[i].size(),
                         entries[i].c_str()};
    }
    ci.modelInitializer = initializer.data();
    ci.modelInitializerSize = initializer.size();
    ci.prePassWeights = weights.data();
    ci.prePassWeightsSize = weights.size();
    scratch.assign(ffxFsr4VkGetScratchMemorySize(), 0);
    ci.scratchBuffer = scratch.data();
    ci.scratchBufferSize = scratch.size();
    CHECK(ffxFsr4VkCreateContext(&ci, &backend));

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize = {(ow + 7) & ~7u, (oh + 7) & ~7u};
    desc.maxUpscaleSize = {(ow + 7) & ~7u, (oh + 7) & ~7u};
    ffxFsr4V07SetBackendInterface(&backend);
    if (ffxFsr4V07CreateContext(&p_context, &desc.header, nullptr) != FFX_API_RETURN_OK) {
        std::fprintf(stderr, "provider context creation failed\n");
        return 1;
    }
    ffxFsr4V07SetBackendInterface(nullptr);
    std::printf("FSR 4 %s %ux%u -> %ux%u, %d frames\n",
                ffxFsr4ModelPresetName(ModelPreset(preset)), rw, rh, ow, oh, frames);
    }

    const VkImageUsageFlags rw_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Image color = CreateImage(gpu, VK_FORMAT_R16G16B16A16_SFLOAT, rw, rh, rw_usage,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth = CreateImage(gpu, VK_FORMAT_D32_SFLOAT, rw, rh,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT);
    Image motion = CreateImage(gpu, VK_FORMAT_R16G16_SFLOAT, rw, rh, rw_usage,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    Image output = CreateImage(gpu, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, rw_usage,
                               VK_IMAGE_ASPECT_COLOR_BIT);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gpu.family;
    VkCommandPool h_pool;
    CHECK(vkCreateCommandPool(gpu.h_device, &pci, nullptr, &h_pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = h_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer h_cmd;
    CHECK(vkAllocateCommandBuffers(gpu.h_device, &cai, &h_cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence h_fence;
    CHECK(vkCreateFence(gpu.h_device, &fci, nullptr, &h_fence));

    const auto submit = [&] {
        CHECK(vkEndCommandBuffer(h_cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &h_cmd;
        CHECK(vkQueueSubmit(gpu.h_queue, 1, &si, h_fence));
        CHECK(vkWaitForFences(gpu.h_device, 1, &h_fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(gpu.h_device, 1, &h_fence));
    };
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    // Inputs: a mid-grey frame, far depth, small motion; all in General.
    CHECK(vkBeginCommandBuffer(h_cmd, &begin));
    const auto to_general = [&](const Image& image, VkImageAspectFlags aspect) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.h_image;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(h_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    to_general(color, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(depth, VK_IMAGE_ASPECT_DEPTH_BIT);
    to_general(motion, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(output, VK_IMAGE_ASPECT_COLOR_BIT);
    const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkClearColorValue grey{{0.4f, 0.35f, 0.3f, 1.0f}}, mv{{0.25f, -0.5f, 0.0f, 0.0f}};
    vkCmdClearColorImage(h_cmd, color.h_image, VK_IMAGE_LAYOUT_GENERAL, &grey, 1, &color_range);
    vkCmdClearColorImage(h_cmd, motion.h_image, VK_IMAGE_LAYOUT_GENERAL, &mv, 1, &color_range);
    const VkClearDepthStencilValue far{0.5f, 0};
    const VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(h_cmd, depth.h_image, VK_IMAGE_LAYOUT_GENERAL, &far, 1, &depth_range);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(h_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    submit();

    const char* sz_noise_env = std::getenv("BENCH_NOISE");
    if (sz_noise_env && sz_noise_env[0] == '1') {
        // Pseudo-random color, motion within +-2 pixels, depth; one upload.
        const VkDeviceSize cb_color = VkDeviceSize(rw) * rh * 8, cb_motion = VkDeviceSize(rw) * rh * 4,
                           cb_depth = VkDeviceSize(rw) * rh * 4;
        Buffer staging = CreateHostBuffer(gpu, cb_color + cb_motion + cb_depth,
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        uint32_t seed = 12345u;
        const auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return float(seed >> 8) / float(1u << 24);
        };
        auto* p_staging = static_cast<unsigned char*>(staging.p_data);
        auto* p_color = reinterpret_cast<uint16_t*>(p_staging);
        for (VkDeviceSize i = 0; i < VkDeviceSize(rw) * rh; ++i) {
            p_color[i * 4 + 0] = Half(next() * 1.5f);
            p_color[i * 4 + 1] = Half(next() * 1.5f);
            p_color[i * 4 + 2] = Half(next() * 1.5f);
            p_color[i * 4 + 3] = Half(1.0f);
        }
        auto* p_motion = reinterpret_cast<uint16_t*>(p_staging + cb_color);
        for (VkDeviceSize i = 0; i < VkDeviceSize(rw) * rh * 2; ++i) {
            const float v = next() * 4.0f - 2.0f;
            p_motion[i] = Half(v);
        }
        auto* p_depth = reinterpret_cast<float*>(p_staging + cb_color + cb_motion);
        for (VkDeviceSize i = 0; i < VkDeviceSize(rw) * rh; ++i) {
            p_depth[i] = 0.9f + next() * 0.1f;
        }
        CHECK(vkBeginCommandBuffer(h_cmd, &begin));
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {rw, rh, 1};
        vkCmdCopyBufferToImage(h_cmd, staging.h_buffer, color.h_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        region.bufferOffset = cb_color;
        vkCmdCopyBufferToImage(h_cmd, staging.h_buffer, motion.h_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        region.bufferOffset = cb_color + cb_motion;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(h_cmd, staging.h_buffer, depth.h_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        vkCmdPipelineBarrier(h_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
        submit();
    }

    const auto resource = [](const Image& image, uint32_t format, uint32_t state) {
        FfxApiResource r{};
        r.resource = reinterpret_cast<void*>(image.h_view);
        r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
        r.description.format = format;
        r.description.width = image.width;
        r.description.height = image.height;
        r.description.depth = 1;
        r.description.mipCount = 1;
        r.state = state;
        return r;
    };
    const auto register_image = [&](const Image& image, VkAccessFlags access) {
        const FfxFsr4VkExternalImageState state{
            sizeof(FfxFsr4VkExternalImageState), image.h_image, image.h_view,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, access,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, access};
        CHECK(ffxFsr4VkSetExternalImageState(&backend, &state));
    };
    std::unique_ptr<Fsr411::Upscaler> upscaler411;
    VkQueryPool h_timestamps = VK_NULL_HANDLE;
    double gpu_ms = 0.0;
    float period_ns = 1.0f;
    if (should_use_fsr411) {
        const char* sz_dir411 = std::getenv("PS_FSR411_DIR");
        upscaler411 = std::make_unique<Fsr411::Upscaler>(gpu.h_physical, gpu.h_device,
                                                         sz_dir411 && sz_dir411[0] ? sz_dir411 : "fsr4_411", gpu.fsr411);
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2;
        CHECK(vkCreateQueryPool(gpu.h_device, &qci, nullptr, &h_timestamps));
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu.h_physical, &props);
        period_ns = props.limits.timestampPeriod;
        std::printf("FSR 4.1.1 replay %ux%u -> %ux%u, %d frames\n", rw, rh, ow, oh, frames);
    }
    for (int frame = 1; should_use_fsr411 && frame <= frames; ++frame) {
        CHECK(vkBeginCommandBuffer(h_cmd, &begin));
        vkCmdResetQueryPool(h_cmd, h_timestamps, 0, 2);
        vkCmdWriteTimestamp(h_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, h_timestamps, 0);
        Fsr411::Frame f;
        f.h_cmdbuf = h_cmd;
        f.color = {color.h_image, color.h_view, rw, rh};
        f.depth = {depth.h_image, depth.h_view, rw, rh};
        f.motion = {motion.h_image, motion.h_view, rw, rh};
        f.output = {output.h_image, output.h_view, ow, oh};
        f.render_width = rw;
        f.render_height = rh;
        f.is_ultra_performance = preset >= 4;
        f.jitter[0] = 0.25f * float((frame - 1) % 4) - 0.375f; // as fsr4cap
        f.jitter[1] = 0.125f;
        f.should_sharpen = true;
        f.sharpness = 0.5f;
        f.should_reset = frame == 1;
        f.has_auto_exposure = true;
        if (!upscaler411->Record(f)) {
            std::fprintf(stderr, "FSR 4.1.1: %s\n", upscaler411->Error().c_str());
            return 1;
        }
        if (frame == 1) {
            std::printf("FSR 4.1.1: %s\n", upscaler411->Describe().c_str());
        }
        vkCmdWriteTimestamp(h_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, h_timestamps, 1);
        submit();
        uint64_t ts[2];
        CHECK(vkGetQueryPoolResults(gpu.h_device, h_timestamps, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT));
        gpu_ms += double(ts[1] - ts[0]) * period_ns * 1e-6;
        if (frame % 300 == 0) {
            std::printf("FSR 4.1.1 replay: %.3f ms/frame (%s)\n", gpu_ms / 300.0, upscaler411->Describe().c_str());
            gpu_ms = 0.0;
        }
    }
    for (int frame = 1; !should_use_fsr411 && frame <= frames; ++frame) {
        CHECK(ffxFsr4VkBeginFrame(&backend, uint64_t(frame)));
        CHECK(vkBeginCommandBuffer(h_cmd, &begin));
        register_image(color, VK_ACCESS_SHADER_READ_BIT);
        register_image(depth, VK_ACCESS_SHADER_READ_BIT);
        register_image(motion, VK_ACCESS_SHADER_READ_BIT);
        register_image(output, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = h_cmd;
        d.color = resource(color, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                           FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = resource(depth, FFX_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = resource(motion, FFX_SURFACE_FORMAT_R16G16_FLOAT,
                                   FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = resource(output, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {0.25f * float(frame % 4) - 0.375f, 0.125f};
        d.motionVectorScale = {1.0f, 1.0f};
        d.renderSize = {rw, rh};
        d.upscaleSize = {ow, oh};
        d.enableSharpening = true;
        d.sharpness = 0.5f;
        d.enableAutoExposure = true;
        d.frameTimeDelta = 10.0f;
        d.preExposure = 1.0f;
        d.reset = frame == 1;
        d.cameraNear = 0.05f;
        d.cameraFar = 3000.0f;
        d.cameraFovAngleVertical = 0.75f;
        d.viewSpaceToMetersFactor = 1.0f;
        if (ffxFsr4V07Dispatch(&p_context, &d.header) != FFX_API_RETURN_OK) {
            std::fprintf(stderr, "dispatch failed\n");
            return 1;
        }
        submit();
        CHECK(ffxFsr4VkRetireFrame(&backend, uint64_t(frame)));
    }
    CHECK(vkDeviceWaitIdle(gpu.h_device));
    if (const char* sz_dump = std::getenv("BENCH_DUMP")) {
        const VkDeviceSize cb_dump = VkDeviceSize(ow) * oh * 8;
        Buffer readback = CreateHostBuffer(gpu, cb_dump, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        CHECK(vkBeginCommandBuffer(h_cmd, &begin));
        VkMemoryBarrier rb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        rb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        rb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(h_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &rb, 0, nullptr, 0, nullptr);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {ow, oh, 1};
        vkCmdCopyImageToBuffer(h_cmd, output.h_image, VK_IMAGE_LAYOUT_GENERAL, readback.h_buffer, 1,
                               &region);
        submit();
        if (FILE* p_file = std::fopen(sz_dump, "wb")) {
            std::fwrite(readback.p_data, 1, size_t(cb_dump), p_file);
            std::fclose(p_file);
        }
    }
    upscaler411.reset();
    if (!should_use_fsr411) {
        ffxFsr4V07DestroyContext(&p_context, nullptr);
        ffxFsr4VkDestroyContext(reinterpret_cast<FfxFsr4VkContext*>(scratch.data()));
    }
    return 0;
}
