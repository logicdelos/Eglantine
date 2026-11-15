// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include "common/common_types.h"
#include "common/settings.h"
#include "video_core/fsr.h"
#include "video_core/host_shaders/vulkan_buf_frag_spv.h"
#include "video_core/host_shaders/vulkan_fidelityfx_fsr_vert_spv.h"
#include "video_core/renderer_vulkan/present/buf.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {
// FsrEasuCon's four words, then strength and the output size (vulkan_buf.frag's push block).
struct PushConstants {
    std::array<u32, 4 * 4> easu_con;
    std::array<f32, 4> params;
};
static_assert(sizeof(PushConstants) == 80);
} // Anonymous namespace

BUF::BUF(const Device& device, MemoryAllocator& memory_allocator, size_t image_count,
         VkExtent2D extent)
    : m_memory_allocator{memory_allocator}, m_image_count{image_count}, m_extent{extent} {
    CreateImages(device);
    CreateRenderPass(device);
    CreateSampler(device);
    CreateShaders(device);
    CreateDescriptorPool(device);
    CreateDescriptorSetLayout(device);
    CreateDescriptorSets(device);
    CreatePipelineLayout(device);
    CreatePipeline(device);
}

void BUF::CreateImages(const Device& device) {
    m_dynamic_images.resize(m_image_count);
    for (auto& images : m_dynamic_images) {
        images.image =
            CreateWrappedImage(m_memory_allocator, m_extent, VK_FORMAT_R16G16B16A16_SFLOAT);
        images.image_view =
            CreateWrappedImageView(device, images.image, VK_FORMAT_R16G16B16A16_SFLOAT);
    }
}

void BUF::CreateRenderPass(const Device& device) {
    m_renderpass = CreateWrappedRenderPass(device, VK_FORMAT_R16G16B16A16_SFLOAT);
    for (auto& images : m_dynamic_images) {
        images.framebuffer =
            CreateWrappedFramebuffer(device, m_renderpass, images.image_view, m_extent);
    }
}

void BUF::CreateSampler(const Device& device) {
    m_sampler = CreateBilinearSampler(device);
}

void BUF::CreateShaders(const Device& device) {
    m_vert_shader = BuildShader(device, VULKAN_FIDELITYFX_FSR_VERT_SPV);
    m_frag_shader = BuildShader(device, VULKAN_BUF_FRAG_SPV);
}

void BUF::CreateDescriptorPool(const Device& device) {
    m_descriptor_pool = CreateWrappedDescriptorPool(device, m_image_count, m_image_count);
}

void BUF::CreateDescriptorSetLayout(const Device& device) {
    m_descriptor_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
}

void BUF::CreateDescriptorSets(const Device& device) {
    const std::vector<VkDescriptorSetLayout> layouts(1, *m_descriptor_set_layout);
    for (auto& images : m_dynamic_images) {
        images.descriptor_sets = CreateWrappedDescriptorSets(m_descriptor_pool, layouts);
    }
}

void BUF::CreatePipelineLayout(const Device& device) {
    const VkPushConstantRange range{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    const VkPipelineLayoutCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = m_descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    m_pipeline_layout = device.GetLogical().CreatePipelineLayout(ci);
}

void BUF::CreatePipeline(const Device& device) {
    m_pipeline = CreateWrappedPipeline(device, m_renderpass, m_pipeline_layout,
                                       std::tie(m_vert_shader, m_frag_shader));
}

void BUF::UpdateDescriptorSet(const Device& device, VkImageView image_view, size_t image_index) {
    Images& images = m_dynamic_images[image_index];
    std::vector<VkDescriptorImageInfo> image_infos;
    image_infos.reserve(1);
    const std::vector<VkWriteDescriptorSet> updates{
        CreateWriteDescriptorSet(image_infos, *m_sampler, image_view, images.descriptor_sets[0], 0),
    };
    device.GetLogical().UpdateDescriptorSets(updates, {});
}

void BUF::UploadImages(Scheduler& scheduler) {
    if (m_images_ready) {
        return;
    }
    m_images_ready = true;
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        for (auto& images : m_dynamic_images) {
            ClearColorImage(cmdbuf, *images.image);
        }
    });
    scheduler.Finish();
}

VkImageView BUF::Draw(const Device& device, Scheduler& scheduler, size_t image_index,
                      VkImage source_image, VkImageView source_image_view,
                      VkExtent2D input_image_extent, const Common::Rectangle<f32>& crop_rect) {
    Images& images = m_dynamic_images[image_index];
    const VkImage image = *images.image;
    const VkDescriptorSet descriptor_set = images.descriptor_sets[0];
    const VkFramebuffer framebuffer = *images.framebuffer;
    const VkPipeline pipeline = *m_pipeline;
    const VkPipelineLayout pipeline_layout = *m_pipeline_layout;
    const VkRenderPass renderpass = *m_renderpass;
    const VkExtent2D extent = m_extent;

    const f32 input_width = static_cast<f32>(input_image_extent.width);
    const f32 input_height = static_cast<f32>(input_image_extent.height);
    const f32 output_width = static_cast<f32>(extent.width);
    const f32 output_height = static_cast<f32>(extent.height);
    const f32 viewport_width = (crop_rect.right - crop_rect.left) * input_width;
    const f32 viewport_x = crop_rect.left * input_width;
    const f32 viewport_height = (crop_rect.bottom - crop_rect.top) * input_height;
    const f32 viewport_y = crop_rect.top * input_height;

    PushConstants push{};
    ::FSR::FsrEasuConOffset(push.easu_con.data() + 0, push.easu_con.data() + 4,
                            push.easu_con.data() + 8, push.easu_con.data() + 12, viewport_width,
                            viewport_height, input_width, input_height, output_width,
                            output_height, viewport_x, viewport_y);
    // 100% is the formula as tuned.
    const f32 strength = static_cast<f32>(Settings::values.buf_strength.GetValue()) / 100.0f;
    push.params = {strength, output_width, output_height, 0.0f};

    UploadImages(scheduler);
    UpdateDescriptorSet(device, source_image_view, image_index);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        TransitionImageLayout(cmdbuf, source_image, VK_IMAGE_LAYOUT_GENERAL);
        TransitionImageLayout(cmdbuf, image, VK_IMAGE_LAYOUT_GENERAL);
        BeginRenderPass(cmdbuf, renderpass, framebuffer, extent);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0,
                                  descriptor_set, {});
        cmdbuf.PushConstants(pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, push);
        cmdbuf.Draw(3, 1, 0, 0);
        cmdbuf.EndRenderPass();
        TransitionImageLayout(cmdbuf, image, VK_IMAGE_LAYOUT_GENERAL);
    });

    return *images.image_view;
}

} // namespace Vulkan
