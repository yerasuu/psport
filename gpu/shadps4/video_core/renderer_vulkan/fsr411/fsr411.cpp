// SPDX-License-Identifier: GPL-2.0-or-later
// psport: FSR 4.1.1 replay on Vulkan (fsr411.h). The frame is the DLL's 29 dispatches: SPD auto
// exposure, prepass, pass0_post, model passes 1..12 each followed by its _post pass (tensor border
// clears), postpass, RCAS. The rules for group counts, the tensor size table and the constants
// are the ones tools/fsr4cap/extract.py checks against the recorded D3D12 frames. The DLL's FP8
// matrix variant (RDNA4) has the same passes, constants and buffers, other shaders and weights,
// and other group counts (Rules).

#include "fsr411.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace Fsr411 {

namespace {

constexpr const char* kPasses[] = {
    "spd",         "prepass",     "pass0_post",  "pass1",       "pass1_post",  "pass2",
    "pass2_post",  "pass3",       "pass3_post",  "pass4",       "pass4_post",  "pass5",
    "pass5_post",  "pass6",       "pass6_post",  "pass7",       "pass7_post",  "pass8",
    "pass8_post",  "pass9",       "pass9_post",  "pass10",      "pass10_post", "pass11",
    "pass11_post", "pass12",      "pass12_post", "postpass",    "rcas",
};
constexpr uint32_t kPassCount = sizeof(kPasses) / sizeof(kPasses[0]);
/// Tensor level (1/2^level of the aligned output) a model pass runs at, and of its _post pass.
constexpr uint32_t kRunLevel[13] = {1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 2, 2, 1};
constexpr uint32_t kPostLevel[13] = {1, 1, 1, 2, 2, 2, 3, 3, 3, 2, 2, 1, 1};

/// The group counts of a variant (extract.py VARIANTS): tensor elements per group of model pass k
/// at its level, output pixels per group of the prepass and postpass, and the alignment of the
/// tensors' width whose border the _post passes clear (up to align + 1 columns right of it).
struct Rules {
    const char* sz_name;
    const char* sz_folder; ///< of its asset sets
    uint32_t tile[13][2];
    uint32_t prepass[2], postpass[2];
    uint32_t post_align;
};
constexpr Rules kInt8{"INT8",
                      "",
                      {{0, 0}, {64, 1}, {64, 1}, {64, 1}, {64, 1}, {64, 1}, {64, 1}, {64, 1}, {64, 1},
                       {64, 1}, {64, 1}, {64, 1}, {64, 1}},
                      {16, 16},
                      {32, 32},
                      4};
constexpr Rules kFp8{"FP8",
                     "fp8/",
                     {{0, 0}, {16, 8}, {16, 8}, {32, 1}, {16, 4}, {16, 4}, {32, 1}, {16, 2}, {16, 2},
                      {16, 1}, {16, 4}, {16, 1}, {16, 8}},
                     {64, 2},
                     {64, 2},
                     32};
/// The FP8 passes with FP8 emulated through FP16 matrices, as vkd3d-proton runs them on RDNA3.
constexpr Rules kFp8Emulated = [] {
    Rules r = kFp8;
    r.sz_name = "FP8 (emulated)";
    r.sz_folder = "fp8emu/";
    return r;
}();

uint32_t CeilDiv(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = int32_t((x >> 23) & 0xffu) - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (exp <= 0) {
        return uint16_t(sign);
    }
    if (exp >= 31) {
        return uint16_t(sign | 0x7c00u);
    }
    // Round to nearest even.
    uint32_t h = sign | (uint32_t(exp) << 10) | (mant >> 13);
    const uint32_t rest = mant & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) {
        ++h;
    }
    return uint16_t(h);
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    data.resize(size_t(file.tellg()));
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())));
}

// ---- SPIR-V reflection ------------------------------------------------------------------------

struct Binding {
    uint32_t binding;
    VkDescriptorType type;
    std::string name;
};

struct Reflection {
    std::string entry = "main";
    uint32_t local_size[3] = {1, 1, 1};
    std::vector<Binding> bindings;
    bool has_matrices = false; ///< CooperativeMatrixKHR: runs as wave32
};

bool Reflect(const std::vector<uint32_t>& words, Reflection& out, std::string& error) {
    if (words.size() < 5 || words[0] != 0x07230203u) {
        error = "not SPIR-V";
        return false;
    }
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, uint32_t> binding_of;
    struct Type {
        uint32_t op = 0;
        uint32_t a = 0, b = 0, c = 0; // image: sampled type, dim, sampled; pointer: class, pointee
    };
    std::map<uint32_t, Type> types;
    struct Var {
        uint32_t id, type, storage;
    };
    std::vector<Var> vars;
    const auto string_at = [&](size_t start, size_t end) {
        std::string s;
        for (size_t i = start; i < end; ++i) {
            for (int k = 0; k < 4; ++k) {
                const char ch = char((words[i] >> (8 * k)) & 0xffu);
                if (!ch) {
                    return s;
                }
                s.push_back(ch);
            }
        }
        return s;
    };
    for (size_t i = 5; i < words.size();) {
        const uint32_t c_words = words[i] >> 16, op = words[i] & 0xffffu;
        if (c_words == 0 || i + c_words > words.size()) {
            error = "malformed SPIR-V";
            return false;
        }
        const uint32_t* p_instruction = &words[i];
        switch (op) {
        case 17: // OpCapability
            out.has_matrices |= p_instruction[1] == 6022; // CooperativeMatrixKHR
            break;
        case 5: // OpName
            names[p_instruction[1]] = string_at(i + 2, i + c_words);
            break;
        case 15: // OpEntryPoint: model, id, name
            out.entry = string_at(i + 3, i + c_words);
            break;
        case 16: // OpExecutionMode LocalSize
            if (c_words >= 6 && p_instruction[2] == 17) {
                out.local_size[0] = p_instruction[3];
                out.local_size[1] = p_instruction[4];
                out.local_size[2] = p_instruction[5];
            }
            break;
        case 25: // OpTypeImage: result, sampled type, dim, depth, arrayed, ms, sampled, format
            types[p_instruction[1]] = {op, p_instruction[2], p_instruction[3], p_instruction[7]};
            break;
        case 26: // OpTypeSampler
            types[p_instruction[1]] = {op};
            break;
        case 30: // OpTypeStruct
            types[p_instruction[1]] = {op};
            break;
        case 32: // OpTypePointer: result, storage class, type
            types[p_instruction[1]] = {op, p_instruction[2], p_instruction[3]};
            break;
        case 59: // OpVariable: result type, result, storage class
            vars.push_back({p_instruction[2], p_instruction[1], p_instruction[3]});
            break;
        case 71: // OpDecorate
            if (c_words >= 4 && p_instruction[2] == 33) {
                binding_of[p_instruction[1]] = p_instruction[3];
            }
            break;
        default:
            break;
        }
        i += c_words;
    }
    for (const Var& v : vars) {
        const auto b = binding_of.find(v.id);
        if (b == binding_of.end()) {
            continue;
        }
        const Type& ptr = types[v.type];
        const Type& pointee = types[ptr.b];
        VkDescriptorType type;
        if (v.storage == 2) {
            type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        } else if (v.storage == 12) {
            type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        } else if (pointee.op == 26) {
            type = VK_DESCRIPTOR_TYPE_SAMPLER;
        } else if (pointee.op == 25) {
            const bool is_buffer = pointee.b == 5;
            if (pointee.c == 2) {
                type = is_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            } else {
                type = is_buffer ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            }
        } else {
            continue;
        }
        // dxil-spirv may declare one resource twice (typed views of a buffer): one binding.
        const bool has_seen = std::any_of(out.bindings.begin(), out.bindings.end(),
                                      [&](const Binding& x) { return x.binding == b->second; });
        if (!has_seen) {
            std::string name = names[v.id];
            if (const size_t u = name.find("_1"); u != std::string::npos && u + 2 == name.size()) {
                name.resize(u);
            }
            out.bindings.push_back({b->second, type, name});
        }
    }
    return true;
}

} // namespace

// ---- implementation ---------------------------------------------------------------------------

struct Upscaler::Impl {
    VkPhysicalDevice m_h_physical;
    VkDevice m_h_device;
    std::string m_dir;
    const Rules* m_p_rules = &kInt8;
    std::string m_error;
    VkPhysicalDeviceMemoryProperties m_memory{};
    VkDeviceSize m_ubo_align = 256;
    PFN_vkCmdPushDescriptorSetKHR m_pfn_push_descriptors = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT m_pfn_set_object_name = nullptr;

    // Current set and sizes.
    std::string m_set;
    uint32_t m_output_width = 0, m_output_height = 0;
    bool m_is_ready = false;
    bool m_is_uploaded = false;

    struct Pass {
        Reflection refl;
        VkDescriptorSetLayout h_set_layout = VK_NULL_HANDLE;
        VkPipelineLayout h_layout = VK_NULL_HANDLE;
        VkPipeline h_pipeline = VK_NULL_HANDLE;
    };
    std::array<Pass, kPassCount> m_passes{};
    VkSampler m_h_linear_clamp = VK_NULL_HANDLE;

    struct Img {
        VkImage h_image = VK_NULL_HANDLE;
        VkImageView h_view = VK_NULL_HANDLE;
        VkDeviceMemory h_memory = VK_NULL_HANDLE;
    };
    Img m_recurrent, m_history, m_reprojected, m_mlsr_output, m_exposure, m_exposure_identity, m_spd_atomic, m_spd_mip5;
    struct Buf {
        VkBuffer h_buffer = VK_NULL_HANDLE;
        VkDeviceMemory h_memory = VK_NULL_HANDLE;
        VkDeviceSize cb_buffer = 0;
        void* p_mapped = nullptr;
    };
    Buf m_scratch, m_initializer, m_staging, m_ubo, m_tensor_ubo;
    std::vector<uint8_t> m_initializer_data;
    uint64_t m_i_frame = 0;
    float m_previous_pre_exposure = 0.0f;
    // PS_FSR4_PROFILE=1: GPU time per pass, read when a ring slot is reused, printed every 300 frames.
    VkQueryPool m_h_profile_pool = VK_NULL_HANDLE;
    float m_period_ns = 1.0f;
    std::array<uint32_t, kFramesInFlight> m_c_profiled{};
    std::array<std::array<uint8_t, kPassCount>, kFramesInFlight> m_profile_pass{};
    std::array<double, kPassCount> m_profile_ms{};
    uint64_t m_c_profile_frames = 0;

    Impl(VkPhysicalDevice h_physical, VkDevice h_device, std::string dir_, Features features)
        : m_h_physical{h_physical}, m_h_device{h_device}, m_dir{std::move(dir_)} {
        // FP8 when the device runs it and its sets were built (on RDNA4: tools/fsr4cap records
        // both variants there).
        const auto built = [&](const Rules& r) {
            return bool(std::ifstream(m_dir + "/" + r.sz_folder + "t1080_m0/initializer.bin"));
        };
        const char* sz_env = std::getenv("PS_FSR411_VARIANT");
        const std::string wanted = sz_env ? sz_env : "";
        if (wanted == "fp8emu" && features.has_fp16_matrices && built(kFp8Emulated)) {
            m_p_rules = &kFp8Emulated;
        } else if (wanted != "int8" && features.has_fp8_matrices && built(kFp8)) {
            m_p_rules = &kFp8;
        }
        vkGetPhysicalDeviceMemoryProperties(m_h_physical, &m_memory);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_h_physical, &props);
        m_ubo_align = std::max<VkDeviceSize>(256, props.limits.minUniformBufferOffsetAlignment);
        m_pfn_push_descriptors = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(m_h_device, "vkCmdPushDescriptorSetKHR"));
        m_pfn_set_object_name = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetDeviceProcAddr(m_h_device, "vkSetDebugUtilsObjectNameEXT"));
        m_period_ns = props.limits.timestampPeriod;
        const char* sz_profile = std::getenv("PS_FSR4_PROFILE");
        if (sz_profile && sz_profile[0] == '1') {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = kFramesInFlight * (kPassCount + 1);
            if (vkCreateQueryPool(m_h_device, &qci, nullptr, &m_h_profile_pool) != VK_SUCCESS) {
                m_h_profile_pool = VK_NULL_HANDLE;
            }
        }
    }

    void CollectProfile(uint32_t slot) {
        const uint32_t c_queries = m_c_profiled[slot];
        if (!m_h_profile_pool || !c_queries) {
            return;
        }
        m_c_profiled[slot] = 0;
        std::array<uint64_t, kPassCount + 1> ts{};
        if (vkGetQueryPoolResults(m_h_device, m_h_profile_pool, slot * (kPassCount + 1), c_queries + 1,
                                  sizeof(ts), ts.data(), 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
            return;
        }
        for (uint32_t i = 0; i < c_queries; ++i) {
            m_profile_ms[m_profile_pass[slot][i]] += double(ts[i + 1] - ts[i]) * m_period_ns * 1e-6;
        }
        if (++m_c_profile_frames % 300) {
            return;
        }
        double total = 0.0;
        for (double ms : m_profile_ms) total += ms;
        std::printf("FSR 4.1.1 profile: %.3f ms/frame\n", total / 300.0);
        for (uint32_t p = 0; p < kPassCount; ++p) {
            if (m_profile_ms[p] > 0.0) {
                std::printf("  %6.3f ms/frame  %s\n", m_profile_ms[p] / 300.0, kPasses[p]);
            }
            m_profile_ms[p] = 0.0;
        }
        std::fflush(stdout);
    }

    ~Impl() {
        Destroy();
        if (m_h_profile_pool) vkDestroyQueryPool(m_h_device, m_h_profile_pool, nullptr);
    }

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < m_memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (m_memory.memoryTypes[i].propertyFlags & flags) == flags) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags, VkDeviceMemory& out) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, flags);
        if (ai.memoryTypeIndex == UINT32_MAX) {
            m_error = "no memory type";
            return false;
        }
        if (vkAllocateMemory(m_h_device, &ai, nullptr, &out) != VK_SUCCESS) {
            m_error = "out of memory";
            return false;
        }
        return true;
    }

    bool MakeBuffer(Buf& b, VkDeviceSize cb_buffer, VkBufferUsageFlags usage, bool is_host) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = cb_buffer;
        ci.usage = usage;
        if (vkCreateBuffer(m_h_device, &ci, nullptr, &b.h_buffer) != VK_SUCCESS) {
            m_error = "buffer creation failed";
            return false;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_h_device, b.h_buffer, &req);
        const VkMemoryPropertyFlags flags =
            is_host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (!Allocate(req, flags, b.h_memory) ||
            vkBindBufferMemory(m_h_device, b.h_buffer, b.h_memory, 0) != VK_SUCCESS) {
            return false;
        }
        b.cb_buffer = cb_buffer;
        if (is_host && vkMapMemory(m_h_device, b.h_memory, 0, VK_WHOLE_SIZE, 0, &b.p_mapped) != VK_SUCCESS) {
            m_error = "map failed";
            return false;
        }
        return true;
    }

    bool MakeImage(Img& img, const char* sz_name, VkFormat format, uint32_t w, uint32_t h) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(m_h_device, &ci, nullptr, &img.h_image) != VK_SUCCESS) {
            m_error = "image creation failed";
            return false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_h_device, img.h_image, &req);
        if (!Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img.h_memory) ||
            vkBindImageMemory(m_h_device, img.h_image, img.h_memory, 0) != VK_SUCCESS) {
            return false;
        }
        if (m_pfn_set_object_name) {
            const std::string name = std::string("psport: FSR 4.1.1 ") + sz_name;
            VkDebugUtilsObjectNameInfoEXT name_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            name_info.objectType = VK_OBJECT_TYPE_IMAGE;
            name_info.objectHandle = reinterpret_cast<uint64_t>(img.h_image);
            name_info.pObjectName = name.c_str();
            (void)m_pfn_set_object_name(m_h_device, &name_info); // a debug name only: nothing depends on it
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.h_image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(m_h_device, &vi, nullptr, &img.h_view) != VK_SUCCESS) {
            m_error = "image view creation failed";
            return false;
        }
        return true;
    }

    void FreeImage(Img& img) {
        if (img.h_view) vkDestroyImageView(m_h_device, img.h_view, nullptr);
        if (img.h_image) vkDestroyImage(m_h_device, img.h_image, nullptr);
        if (img.h_memory) vkFreeMemory(m_h_device, img.h_memory, nullptr);
        img = {};
    }

    void FreeBuffer(Buf& b) {
        if (b.h_buffer) vkDestroyBuffer(m_h_device, b.h_buffer, nullptr);
        if (b.h_memory) vkFreeMemory(m_h_device, b.h_memory, nullptr);
        b = {};
    }

    void Destroy() {
        for (Pass& p : m_passes) {
            if (p.h_pipeline) vkDestroyPipeline(m_h_device, p.h_pipeline, nullptr);
            if (p.h_layout) vkDestroyPipelineLayout(m_h_device, p.h_layout, nullptr);
            if (p.h_set_layout) vkDestroyDescriptorSetLayout(m_h_device, p.h_set_layout, nullptr);
            p = {};
        }
        if (m_h_linear_clamp) vkDestroySampler(m_h_device, m_h_linear_clamp, nullptr);
        m_c_profiled = {};
        m_h_linear_clamp = VK_NULL_HANDLE;
        for (Img* p_img : {&m_recurrent, &m_history, &m_reprojected, &m_mlsr_output, &m_exposure,
                         &m_exposure_identity, &m_spd_atomic, &m_spd_mip5}) {
            FreeImage(*p_img);
        }
        for (Buf* p_buf : {&m_scratch, &m_initializer, &m_staging, &m_ubo, &m_tensor_ubo}) {
            FreeBuffer(*p_buf);
        }
        m_is_ready = false;
        m_is_uploaded = false;
        m_set.clear();
    }

    bool LoadPass(uint32_t i_pass) {
        std::vector<uint8_t> bytes;
        const std::string path = m_dir + "/" + m_p_rules->sz_folder + m_set + "/" + kPasses[i_pass] + ".spv";
        if (!ReadFile(path, bytes) || bytes.size() % 4) {
            m_error = "missing " + path + " (tools/fsr4cap: capture and extract the FSR 4.1.1 assets)";
            return false;
        }
        std::vector<uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        Pass& p = m_passes[i_pass];
        if (!Reflect(words, p.refl, m_error)) {
            m_error = path + ": " + m_error;
            return false;
        }
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const Binding& b : p.refl.bindings) {
            VkDescriptorSetLayoutBinding lb{};
            lb.binding = b.binding;
            lb.descriptorType = b.type;
            lb.descriptorCount = 1;
            lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                lb.pImmutableSamplers = &m_h_linear_clamp;
            }
            bindings.push_back(lb);
        }
        VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sci.bindingCount = uint32_t(bindings.size());
        sci.pBindings = bindings.data();
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &p.h_set_layout;
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = bytes.size();
        mci.pCode = words.data();
        VkShaderModule h_module = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(m_h_device, &sci, nullptr, &p.h_set_layout) != VK_SUCCESS ||
            vkCreatePipelineLayout(m_h_device, &lci, nullptr, &p.h_layout) != VK_SUCCESS ||
            vkCreateShaderModule(m_h_device, &mci, nullptr, &h_module) != VK_SUCCESS) {
            m_error = path + ": layout or module creation failed";
            return false;
        }
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        // Matrix passes as vkd3d-proton runs them (their wave size heuristic: extract.py checks
        // it is these): wave32, which WMMA is on RDNA, in full subgroups.
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo wave32{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        wave32.requiredSubgroupSize = 32;
        if (p.refl.has_matrices) {
            pci.stage.pNext = &wave32;
            if (p.refl.local_size[0] % 32 == 0) {
                pci.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
            }
        }
        pci.stage.module = h_module;
        pci.stage.pName = p.refl.entry.c_str();
        pci.layout = p.h_layout;
        const VkResult r = vkCreateComputePipelines(m_h_device, VK_NULL_HANDLE, 1, &pci, nullptr, &p.h_pipeline);
        vkDestroyShaderModule(m_h_device, h_module, nullptr);
        if (r != VK_SUCCESS) {
            m_error = path + ": pipeline creation failed (" + std::to_string(int(r)) + ")";
            return false;
        }
        return true;
    }

    bool Create(const std::string& wanted, uint32_t ow, uint32_t oh, bool is_tier2160) {
        Destroy();
        if (!m_pfn_push_descriptors) {
            m_error = "VK_KHR_push_descriptor is required";
            return false;
        }
        m_set = wanted;
        m_output_width = ow;
        m_output_height = oh;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(m_h_device, &si, nullptr, &m_h_linear_clamp) != VK_SUCCESS) {
            m_error = "sampler creation failed";
            return false;
        }
        for (uint32_t i = 0; i < kPassCount; ++i) {
            if (!LoadPass(i)) {
                return false;
            }
        }
        const std::string weights = m_dir + "/" + m_p_rules->sz_folder + m_set + "/initializer.bin";
        if (!ReadFile(weights, m_initializer_data) || m_initializer_data.size() != 131072) {
            m_error = "missing or wrong " + weights;
            return false;
        }
        const VkDeviceSize cb_scratch = is_tier2160 ? 83232256u : 20880256u;
        if (!MakeImage(m_recurrent, "recurrent", VK_FORMAT_R8G8B8A8_UNORM, ow, oh) ||
            !MakeImage(m_history, "history", VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh) ||
            !MakeImage(m_reprojected, "reprojected", VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh) ||
            !MakeImage(m_mlsr_output, "output", VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh) ||
            !MakeImage(m_exposure, "exposure", VK_FORMAT_R32_SFLOAT, 2, 1) ||
            !MakeImage(m_exposure_identity, "identity exposure", VK_FORMAT_R32_SFLOAT, 2, 1) ||
            !MakeImage(m_spd_atomic, "SPD atomic", VK_FORMAT_R32_UINT, 1, 1) ||
            !MakeImage(m_spd_mip5, "SPD mip 5", VK_FORMAT_R32_SFLOAT, CeilDiv(ow, 16), CeilDiv(oh, 16)) ||
            !MakeBuffer(m_scratch, cb_scratch,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
            !MakeBuffer(m_initializer, 131072,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
            !MakeBuffer(m_staging, 131072, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) ||
            !MakeBuffer(m_ubo, kFramesInFlight * 4 * m_ubo_align, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true) ||
            !MakeBuffer(m_tensor_ubo, 512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true)) {
            return false;
        }
        std::memcpy(m_staging.p_mapped, m_initializer_data.data(), m_initializer_data.size());
        // Tensor sizes (17 x uint4): levels of the output aligned to 8.
        const uint32_t aw = (ow + 7) & ~7u, ah = (oh + 7) & ~7u;
        static constexpr uint32_t kShift[17] = {1, 0, 1, 1, 2, 2, 2, 3, 3, 3, 2, 2, 1, 1, 0, 0, 0};
        auto* p_sizes = static_cast<uint32_t*>(m_tensor_ubo.p_mapped);
        std::memset(p_sizes, 0, 512);
        for (uint32_t i = 0; i < 17; ++i) {
            p_sizes[i * 4 + 0] = aw >> kShift[i];
            p_sizes[i * 4 + 1] = ah >> kShift[i];
        }
        m_is_ready = true;
        m_previous_pre_exposure = 0.0f;
        return true;
    }

    /// First frame after creation: weights, cleared state.
    void Upload(VkCommandBuffer h_cmd) {
        std::vector<VkImageMemoryBarrier> to_general;
        for (Img* p_img : {&m_recurrent, &m_history, &m_reprojected, &m_mlsr_output, &m_exposure,
                         &m_exposure_identity, &m_spd_atomic, &m_spd_mip5}) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = p_img->h_image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            to_general.push_back(b);
        }
        vkCmdPipelineBarrier(h_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, uint32_t(to_general.size()), to_general.data());
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue zero{}, one{{1.0f, 1.0f, 1.0f, 1.0f}};
        for (Img* p_img : {&m_recurrent, &m_history, &m_reprojected, &m_mlsr_output, &m_exposure, &m_spd_atomic, &m_spd_mip5}) {
            vkCmdClearColorImage(h_cmd, p_img->h_image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        vkCmdClearColorImage(h_cmd, m_exposure_identity.h_image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
        vkCmdFillBuffer(h_cmd, m_scratch.h_buffer, 0, VK_WHOLE_SIZE, 0);
        const VkBufferCopy copy{0, 0, 131072};
        vkCmdCopyBuffer(h_cmd, m_staging.h_buffer, m_initializer.h_buffer, 1, &copy);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(h_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
        m_is_uploaded = true;
    }

    bool Record(const Frame& f) {
        m_error.clear();
        const uint32_t ow = f.output.width, oh = f.output.height;
        const uint32_t rw = f.render_width, rh = f.render_height;
        if (!ow || !oh || !rw || !rh || rw > ow || rh > oh || ow > 3840 || oh > 2160) {
            m_error = "unsupported size " + std::to_string(rw) + "x" + std::to_string(rh) + " -> " +
                    std::to_string(ow) + "x" + std::to_string(oh);
            return false;
        }
        const bool is_tier2160 = ow > 1920 || oh > 1080;
        const std::string wanted = std::string(is_tier2160 ? "t2160" : "t1080") +
                                   (f.is_ultra_performance ? "_m1" : "_m0");
        if (!m_is_ready || wanted != m_set || ow != m_output_width || oh != m_output_height) {
            if (!Create(wanted, ow, oh, is_tier2160)) {
                Destroy();
                return false;
            }
        }
        const VkCommandBuffer h_cmd = f.h_cmdbuf;
        if (!m_is_uploaded) {
            Upload(h_cmd);
        }
        const uint32_t aw = (ow + 7) & ~7u, ah = (oh + 7) & ~7u;
        const float pre_exposure = f.pre_exposure > 0.0f ? f.pre_exposure : 1.0f;

        // Constant buffers of this frame: MLSR, SPD, RCAS.
        const uint32_t ring = uint32_t(m_i_frame++ % kFramesInFlight);
        const VkDeviceSize slot = ring * 4 * m_ubo_align;
        CollectProfile(ring);
        const uint32_t query_base = ring * (kPassCount + 1);
        uint32_t profiled = 0;
        if (m_h_profile_pool) {
            vkCmdResetQueryPool(h_cmd, m_h_profile_pool, query_base, kPassCount + 1);
            vkCmdWriteTimestamp(h_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_h_profile_pool, query_base);
        }
        auto* p_base = static_cast<uint8_t*>(m_ubo.p_mapped) + slot;
        struct Mlsr {
            float inv_size[2], scale[2], inv_scale[2], jitter[2], mv_scale[2], tex_size[2],
                max_render_size[2], mv_jitter_cancellation[2];
            uint32_t width, height, reset, width_lr, height_lr;
            float pre_exposure, previous_pre_exposure;
            uint32_t rcas_enabled;
            float rcas_sharpness, pad;
        } mlsr{};
        mlsr.inv_size[0] = 1.0f / float(ow);
        mlsr.inv_size[1] = 1.0f / float(oh);
        mlsr.scale[0] = float(ow) / float(rw);
        mlsr.scale[1] = float(oh) / float(rh);
        // As the DLL: the reciprocal of the scale (not rw / ow, which differs in the last bit).
        mlsr.inv_scale[0] = 1.0f / mlsr.scale[0];
        mlsr.inv_scale[1] = 1.0f / mlsr.scale[1];
        mlsr.jitter[0] = f.jitter[0];
        mlsr.jitter[1] = f.jitter[1];
        mlsr.mv_scale[0] = f.motion_scale[0] / float(rw);
        mlsr.mv_scale[1] = f.motion_scale[1] / float(rh);
        mlsr.tex_size[0] = mlsr.max_render_size[0] = float(ow);
        mlsr.tex_size[1] = mlsr.max_render_size[1] = float(oh);
        mlsr.width = ow;
        mlsr.height = oh;
        mlsr.reset = f.should_reset ? 1u : 0u;
        mlsr.width_lr = rw;
        mlsr.height_lr = rh;
        mlsr.pre_exposure = pre_exposure;
        mlsr.previous_pre_exposure = m_previous_pre_exposure;
        mlsr.rcas_enabled = f.should_sharpen ? 1u : 0u;
        mlsr.rcas_sharpness = f.sharpness;
        std::memcpy(p_base, &mlsr, sizeof(mlsr));
        struct Spd {
            uint32_t mips, work_groups, offset[2];
            float inv_input_size[2], pre_exposure, pad;
        } spd{};
        for (uint32_t m = std::max(rw, rh); m > 1 && spd.mips < 12; m >>= 1) {
            ++spd.mips;
        }
        spd.work_groups = CeilDiv(rw, 64) * CeilDiv(rh, 64);
        spd.inv_input_size[0] = 1.0f / float(rw);
        spd.inv_input_size[1] = 1.0f / float(rh);
        spd.pre_exposure = pre_exposure;
        std::memcpy(p_base + m_ubo_align, &spd, sizeof(spd));
        struct Rcas {
            uint32_t config[4];
            float pre_exposure;
            uint32_t pad[3];
        } rcas{};
        const float amount = std::clamp(f.sharpness, 0.0f, 1.0f);
        const float linear = std::exp2(-(2.0f - 2.0f * amount));
        std::memcpy(&rcas.config[0], &linear, 4);
        rcas.config[1] = uint32_t(FloatToHalf(linear)) | (uint32_t(FloatToHalf(linear)) << 16);
        rcas.pre_exposure = pre_exposure;
        std::memcpy(p_base + 2 * m_ubo_align, &rcas, sizeof(rcas));
        m_previous_pre_exposure = pre_exposure;

        const Img& exposure_image = f.has_auto_exposure ? m_exposure : m_exposure_identity;
        const Image* const p_final_output = &f.output;

        VkMemoryBarrier between{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        between.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        between.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        // Earlier work of the caller on the inputs (render, copies) is the caller's barrier.
        for (uint32_t i_pass = 0; i_pass < kPassCount; ++i_pass) {
            const std::string name = kPasses[i_pass];
            if ((name == "spd" && !f.has_auto_exposure) || (name == "rcas" && !f.should_sharpen)) {
                continue;
            }
            const Pass& p = m_passes[i_pass];
            std::vector<VkWriteDescriptorSet> writes;
            std::vector<VkDescriptorImageInfo> images;
            std::vector<VkDescriptorBufferInfo> buffers;
            images.reserve(p.refl.bindings.size());
            buffers.reserve(p.refl.bindings.size());
            for (const Binding& b : p.refl.bindings) {
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstBinding = b.binding;
                w.descriptorCount = 1;
                w.descriptorType = b.type;
                const auto image = [&](VkImageView h_view) {
                    images.push_back({VK_NULL_HANDLE, h_view, VK_IMAGE_LAYOUT_GENERAL});
                    w.pImageInfo = &images.back();
                };
                const auto buffer = [&](VkBuffer h_buffer, VkDeviceSize off_buffer, VkDeviceSize range) {
                    buffers.push_back({h_buffer, off_buffer, range});
                    w.pBufferInfo = &buffers.back();
                };
                const std::string& n = b.name;
                if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    continue; // immutable
                } else if (n == "r_input_color") image(f.color.h_view);
                else if (n == "r_velocity") image(f.motion.h_view);
                else if (n == "r_depth") image(f.depth.h_view);
                else if (n == "r_history_color" || n == "rw_history_color") image(m_history.h_view);
                else if (n == "r_reprojected_color" || n == "rw_reprojected_color") image(m_reprojected.h_view);
                else if (n == "r_recurrent_0" || n == "rw_recurrent_0") image(m_recurrent.h_view);
                else if (n == "r_auto_exposure_texture") image(exposure_image.h_view);
                else if (n == "rw_auto_exposure_texture") image(m_exposure.h_view);
                else if (n == "rw_spd_global_atomic") image(m_spd_atomic.h_view);
                else if (n == "rw_autoexp_mip_5") image(m_spd_mip5.h_view);
                else if (n == "rw_mlsr_output_color") image(f.should_sharpen ? m_mlsr_output.h_view : p_final_output->h_view);
                else if (n == "r_rcas_input") image(m_mlsr_output.h_view);
                else if (n == "rw_rcas_output") image(p_final_output->h_view);
                else if (n == "ScratchBuffer") buffer(m_scratch.h_buffer, 0, VK_WHOLE_SIZE);
                else if (n == "InitializerBuffer") buffer(m_initializer.h_buffer, 0, VK_WHOLE_SIZE);
                else if (n == "MLSR_Optimized_Constants") buffer(m_ubo.h_buffer, slot, sizeof(Mlsr));
                else if (n == "AutoExposureSPDConstants") buffer(m_ubo.h_buffer, slot + m_ubo_align, sizeof(Spd));
                else if (n == "cbRCAS") buffer(m_ubo.h_buffer, slot + 2 * m_ubo_align, sizeof(Rcas));
                else if (n == "CsTensorSizes") buffer(m_tensor_ubo.h_buffer, 0, 272);
                else {
                    m_error = name + ": unknown resource " + n;
                    return false;
                }
                writes.push_back(w);
            }
            vkCmdBindPipeline(h_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.h_pipeline);
            m_pfn_push_descriptors(h_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.h_layout, 0, uint32_t(writes.size()),
                             writes.data());
            uint32_t gx = 1, gy = 1;
            if (name == "spd") {
                gx = CeilDiv(rw, 64);
                gy = CeilDiv(rh, 64);
            } else if (name == "rcas") {
                gx = CeilDiv(aw, 16);
                gy = CeilDiv(ah, 16);
            } else if (name == "prepass" || name == "postpass") {
                const uint32_t* p_tile = name == "prepass" ? m_p_rules->prepass : m_p_rules->postpass;
                gx = CeilDiv(aw, p_tile[0]);
                gy = CeilDiv(ah, p_tile[1]);
            } else if (name.ends_with("_post")) {
                // Clears the border of a tensor: threads as the shader counts them (it stops the
                // rest) from the tensor size and the tier's allocation (aligned).
                const uint32_t k = name == "pass0_post" ? 0 : uint32_t(std::stoi(name.substr(4)));
                const uint32_t level = kPostLevel[k];
                const uint32_t w = aw >> level, h = ah >> level;
                const uint32_t align = m_p_rules->post_align;
                const uint32_t tw = ((is_tier2160 ? 3840u : 1920u) >> level) + align - 1;
                const uint32_t th = (is_tier2160 ? 2160u : 1080u) >> level;
                const uint32_t kx = std::min(tw / align * align + 1 - w, align + 1);
                const uint32_t ky = std::min(th + 1 - h, 1u);
                const uint32_t threads = (w + 1 + kx) + h + h * kx + (w + 1 + kx) * ky;
                gx = CeilDiv(threads, p.refl.local_size[0]);
            } else {
                const uint32_t k = uint32_t(std::stoi(name.substr(4)));
                const uint32_t level = kRunLevel[k];
                gx = CeilDiv(aw >> level, m_p_rules->tile[k][0]);
                gy = CeilDiv(ah >> level, m_p_rules->tile[k][1]);
            }
            vkCmdDispatch(h_cmd, gx, gy, 1);
            if (m_h_profile_pool) {
                vkCmdWriteTimestamp(h_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_h_profile_pool,
                                    query_base + 1 + profiled);
                m_profile_pass[ring][profiled++] = uint8_t(i_pass);
            }
            vkCmdPipelineBarrier(h_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &between, 0, nullptr, 0,
                                 nullptr);
        }
        m_c_profiled[ring] = profiled;
        return true;
    }
};

Upscaler::Upscaler(VkPhysicalDevice h_physical, VkDevice h_device, std::string dir, Features features)
    : m_impl{std::make_unique<Impl>(h_physical, h_device, std::move(dir), features)} {}

Upscaler::~Upscaler() = default;

bool Upscaler::Record(const Frame& frame) {
    return m_impl->Record(frame);
}

const std::string& Upscaler::Error() const noexcept {
    return m_impl->m_error;
}

std::string Upscaler::Describe() const {
    return std::string(m_impl->m_p_rules->sz_name) + " " + m_impl->m_set + " output " + std::to_string(m_impl->m_output_width) +
           "x" + std::to_string(m_impl->m_output_height);
}

} // namespace Fsr411
