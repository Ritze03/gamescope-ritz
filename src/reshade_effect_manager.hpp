#pragma once

#include "rendervulkan.hpp"
#include <optional>
#include <string>
#include <vector>

namespace reshadefx
{
    struct module;
}

class ReshadeUniform;

struct ReshadeCombinedImageSampler
{
    VkSampler sampler;
    gamescope::Rc<CVulkanTexture> texture;
};

struct ReshadeEffectKey
{
	std::string path;

	uint32_t bufferWidth;
	uint32_t bufferHeight;
	GamescopeAppTextureColorspace bufferColorSpace;
    VkFormat bufferFormat;

	uint32_t techniqueIdx;

    bool operator==(const ReshadeEffectKey& other) const = default;
    bool operator!=(const ReshadeEffectKey& other) const = default;
};

enum ReshadeDescriptorSets
{
    GAMESCOPE_RESHADE_DESCRIPTOR_SET_UBO = 0,
    GAMESCOPE_RESHADE_DESCRIPTOR_SET_SAMPLED_IMAGES,
    GAMESCOPE_RESHADE_DESCRIPTOR_SET_STORAGE_IMAGES,

    GAMESCOPE_RESHADE_DESCRIPTOR_SET_COUNT,
};

namespace ReshadeEffectFlag
{
    static constexpr uint32_t AlwaysScanout = 1u << 0;
}
using ReshadeEffectFlags = uint32_t;

class ReshadeEffectPipeline
{
public:
    ReshadeEffectPipeline();
    ~ReshadeEffectPipeline();

    bool init(CVulkanDevice *device, const ReshadeEffectKey &key);
    void update();
    uint64_t execute(gamescope::Rc<CVulkanTexture> inImage, gamescope::Rc<CVulkanTexture> *outImage);

    const ReshadeEffectKey& key() const { return m_key; }
    reshadefx::module *module() { return m_module.get(); }

    ReshadeEffectFlags flags() const { return m_flags; }

    gamescope::Rc<CVulkanTexture> findTexture(std::string_view name);

private:
    ReshadeEffectKey m_key;
    CVulkanDevice *m_device;

	std::unique_ptr<reshadefx::module> m_module;
    std::vector<VkPipeline> m_pipelines;
    std::vector<gamescope::OwningRc<CVulkanTexture>> m_textures;
    gamescope::OwningRc<CVulkanTexture> m_rt;
    std::vector<ReshadeCombinedImageSampler> m_samplers;
    std::vector<std::shared_ptr<ReshadeUniform>> m_uniforms;

    std::optional<CVulkanCmdBuffer> m_cmdBuffer = std::nullopt;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_bufferMemory = VK_NULL_HANDLE;
    void* m_mappedPtr = nullptr;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descriptorSetLayouts[GAMESCOPE_RESHADE_DESCRIPTOR_SET_COUNT] = {};
    VkDescriptorSet m_descriptorSets[GAMESCOPE_RESHADE_DESCRIPTOR_SET_COUNT] = {};

    ReshadeEffectFlags m_flags = 0;
};

class ReshadeEffectManager
{
public:
    ReshadeEffectManager();

    void init(CVulkanDevice *device);
    void clear();
    ReshadeEffectPipeline* pipeline(const ReshadeEffectKey &key);

private:
    ReshadeEffectKey m_lastKey{};
    std::unique_ptr<ReshadeEffectPipeline> m_lastPipeline;
    CVulkanDevice *m_device;
};

extern ReshadeEffectManager g_reshadeManager;


void reshade_effect_manager_set_uniform_variable(const char *key, uint8_t* value);

// Diagnostics for the Shaders panel. A .fx is compiled at runtime from a
// file on disk while its C++ driver is compiled into the binary, so the two
// can drift silently -- see the "Effect diagnostics" comment at the top of
// reshade_effect_manager.cpp for the failure this exists to make visible.
//
// shader_source(): the file the currently-loaded effect actually compiled
// from, or "" if nothing has compiled yet.
// missing_uniforms(): names gamescope set that the loaded module does not
// declare, in first-seen order. Non-empty means those controls do nothing.
std::string reshade_effect_manager_shader_source();
std::vector<std::string> reshade_effect_manager_missing_uniforms();
void reshade_effect_manager_set_effect(const char *path, std::function<void(const char*)> callback);
void reshade_effect_manager_enable_effect();
void reshade_effect_manager_disable_effect();