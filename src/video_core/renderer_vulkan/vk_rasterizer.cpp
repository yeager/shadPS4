// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/config.h"
#include "common/debug.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, buffer_cache, page_manager}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, pipeline_cache{instance, scheduler, liverpool} {
    if (!Config::nullGpu()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() = default;

void Rasterizer::CpSync() {
    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlagBits::eByRegion, ib_barrier, {}, {});
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    // There are several cases (e.g. FCE, FMask/HTile decompression) where we don't need to do an
    // actual draw hence can skip pipeline creation.
    if (regs.color_control.mode == Liverpool::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == Liverpool::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        return false;
    }
    if (regs.color_control.mode == Liverpool::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        return false;
    }

    return true;
}

RenderState Rasterizer::PrepareRenderState(u32 mrt_mask) {
    // Prefetch color and depth buffers to let texture cache handle possible overlaps with bound
    // textures (e.g. mipgen)
    RenderState state;

    cb_descs.clear();
    db_desc.reset();

    const auto& regs = liverpool->regs;

    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::Liverpool::ColorControl::OperationMode::Disable;
    for (auto col_buf_id = 0u; col_buf_id < Liverpool::NumColorBuffers; ++col_buf_id) {
        const auto& col_buf = regs.color_buffers[col_buf_id];
        if (skip_cb_binding || !col_buf) {
            continue;
        }

        // Skip stale color buffers if shader doesn't output to them. Otherwise it will perform
        // an unnecessary transition and may result in state conflict if the resource is already
        // bound for reading.
        if ((mrt_mask & (1 << col_buf_id)) == 0) {
            state.color_attachments[state.num_color_attachments++].imageView = VK_NULL_HANDLE;
            continue;
        }

        // If the color buffer is still bound but rendering to it is disabled by the target
        // mask, we need to prevent the render area from being affected by unbound render target
        // extents.
        if (!regs.color_target_mask.GetMask(col_buf_id)) {
            state.color_attachments[state.num_color_attachments++].imageView = VK_NULL_HANDLE;
            continue;
        }

        const auto& hint = liverpool->last_cb_extent[col_buf_id];
        auto& [image_id, desc] = cb_descs.emplace_back(std::piecewise_construct, std::tuple{},
                                                       std::tuple{col_buf, hint});
        const auto& image_view = texture_cache.FindRenderTarget(desc);
        image_id = bound_images.emplace_back(image_view.image_id);
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;

        const auto slice = image_view.info.range.base.layer;
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        const auto mip = image_view.info.range.base.level;
        state.width = std::min<u32>(state.width, std::max(image.info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image.info.size.height >> mip, 1u));
        state.color_attachments[state.num_color_attachments++] = {
            .imageView = *image_view.image_view,
            .imageLayout = vk::ImageLayout::eUndefined,
            .loadOp = is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue =
                is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{},
        };
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] =
            db_desc.emplace(std::piecewise_construct, std::tuple{},
                            std::tuple{regs.depth_buffer, regs.depth_view, regs.depth_control,
                                       htile_address, hint});
        const auto& image_view = texture_cache.FindDepthTarget(desc);
        image_id = bound_images.emplace_back(image_view.image_id);
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear = regs.depth_render_control.depth_clear_enable ||
                                    texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        ASSERT(desc.view_info.range.extent.layers == 1);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.has_depth = regs.depth_buffer.DepthValid();
        state.has_stencil = regs.depth_buffer.StencilValid();
        if (state.has_depth) {
            state.depth_attachment = {
                .imageView = *image_view.image_view,
                .imageLayout = vk::ImageLayout::eUndefined,
                .loadOp =
                    is_depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .clearValue = vk::ClearValue{.depthStencil = {.depth = regs.depth_clear}},
            };
        }
        if (state.has_stencil) {
            state.stencil_attachment = {
                .imageView = *image_view.image_view,
                .imageLayout = vk::ImageLayout::eUndefined,
                .loadOp =
                    is_stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .clearValue = vk::ClearValue{.depthStencil = {.stencil = regs.stencil_clear}},
            };
        }
        texture_cache.TouchMeta(htile_address, slice, false);
    }

    return state;
}

[[nodiscard]] std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Liverpool::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    const auto& hint = liverpool->last_cb_extent[0];
    VideoCore::TextureCache::RenderTargetDesc desc(col_buf, hint);
    const auto& image_view = texture_cache.FindRenderTarget(desc);
    auto& image = texture_cache.GetImage(image_view.image_id);
    const vk::ImageSubresourceRange range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = col_buf.view.slice_start,
        .layerCount = col_buf.view.slice_max - col_buf.view.slice_start + 1,
    };
    scheduler.EndRendering();
    image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    scheduler.CommandBuffer().clearColorImage(image.image, image.last_state.layout,
                                              LiverpoolToVK::ColorBufferClearValue(col_buf).color,
                                              range);
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    if (!FilterDraw()) {
        return;
    }

    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    auto state = PrepareRenderState(pipeline->GetMrtMask());
    if (!BindResources(pipeline)) {
        return;
    }

    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    buffer_cache.BindVertexBuffers(vs_info, fetch_shader);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(index_offset);
    }

    BeginRendering(*pipeline, state);
    UpdateDynamicState(*pipeline);

    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }

    ResetBindings();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    RENDERER_TRACE;

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    auto state = PrepareRenderState(pipeline->GetMrtMask());
    if (!BindResources(pipeline)) {
        return;
    }

    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    buffer_cache.BindVertexBuffers(vs_info, fetch_shader);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(0);
    }

    const auto& [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) = buffer_cache.ObtainBuffer(count_address, 4, false);
    }

    BeginRendering(*pipeline, state);
    UpdateDynamicState(*pipeline);

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_base, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
    }

    ResetBindings();
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);

    ResetBindings();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    scheduler.EndRendering();

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);

    ResetBindings();
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    buffer_infos.clear();
    buffer_views.clear();
    image_infos.clear();

    const auto& regs = liverpool->regs;

    if (pipeline->IsCompute()) {
        const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

        // Most of the time when a metadata is updated with a shader it gets cleared. It means
        // we can skip the whole dispatch and update the tracked state instead. Also, it is not
        // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
        // will need its full emulation anyways. For cases of metadata read a warning will be
        // logged.
        const auto IsMetaUpdate = [&](const auto& desc) {
            const auto sharp = desc.GetSharp(info);
            const VAddr address = sharp.base_address;
            if (desc.is_written) {
                // Assume all slices were updates
                if (texture_cache.ClearMeta(address)) {
                    LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                    return true;
                }
            } else {
                if (texture_cache.IsMeta(address)) {
                    LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a CS shader (buffer)");
                }
            }
            return false;
        };

        // Assume if a shader reads and writes metas at the same time, it is a copy shader.
        bool meta_read = false;
        for (const auto& desc : info.buffers) {
            if (desc.is_gds_buffer) {
                continue;
            }
            if (!desc.is_written) {
                const VAddr address = desc.GetSharp(info).base_address;
                meta_read = texture_cache.IsMeta(address);
            }
        }

        for (const auto& desc : info.texture_buffers) {
            if (!desc.is_written) {
                const VAddr address = desc.GetSharp(info).base_address;
                meta_read = texture_cache.IsMeta(address);
            }
        }

        if (!meta_read) {
            for (const auto& desc : info.buffers) {
                if (IsMetaUpdate(desc)) {
                    return false;
                }
            }

            for (const auto& desc : info.texture_buffers) {
                if (IsMetaUpdate(desc)) {
                    return false;
                }
            }
        }
    }

    set_writes.clear();
    buffer_barriers.clear();

    // Bind resource buffers and textures.
    Shader::PushData push_data{};
    Shader::Backend::Bindings binding{};

    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        push_data.step0 = regs.vgt_instance_step_rate_0;
        push_data.step1 = regs.vgt_instance_step_rate_1;
        stage->PushUd(binding, push_data);

        BindBuffers(*stage, binding, push_data, set_writes, buffer_barriers);
        BindTextures(*stage, binding, set_writes);
    }

    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data, Pipeline::DescriptorWrites& set_writes,
                             Pipeline::BufferBarriers& buffer_barriers) {
    buffer_bindings.clear();

    for (const auto& desc : stage.buffers) {
        const auto vsharp = desc.GetSharp(stage);
        if (!desc.is_gds_buffer && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            const auto buffer_id = buffer_cache.FindBuffer(vsharp.base_address, vsharp.GetSize());
            buffer_bindings.emplace_back(buffer_id, vsharp);
        } else {
            buffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp);
        }
    }

    texbuffer_bindings.clear();

    for (const auto& desc : stage.texture_buffers) {
        const auto vsharp = desc.GetSharp(stage);
        if (vsharp.base_address != 0 && vsharp.GetSize() > 0 &&
            vsharp.GetDataFmt() != AmdGpu::DataFormat::FormatInvalid) {
            const auto buffer_id = buffer_cache.FindBuffer(vsharp.base_address, vsharp.GetSize());
            texbuffer_bindings.emplace_back(buffer_id, vsharp);
        } else {
            texbuffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp);
        }
    }

    // Bind the flattened user data buffer as a UBO so it's accessible to the shader
    if (stage.has_readconst) {
        const auto [vk_buffer, offset] = buffer_cache.ObtainHostUBO(stage.flattened_ud_buf);
        buffer_infos.emplace_back(vk_buffer->Handle(), offset,
                                  stage.flattened_ud_buf.size() * sizeof(u32));
        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &buffer_infos.back(),
        });
        ++binding.buffer;
    }

    // Second pass to re-bind buffers that were updated after binding
    for (u32 i = 0; i < buffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp] = buffer_bindings[i];
        const auto& desc = stage.buffers[i];
        const bool is_storage = desc.IsStorage(vsharp);
        if (!buffer_id) {
            if (desc.is_gds_buffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
            } else if (instance.IsNullDescriptorSupported()) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                auto& null_buffer = buffer_cache.GetBuffer(VideoCore::NULL_BUFFER_ID);
                buffer_infos.emplace_back(null_buffer.Handle(), 0, VK_WHOLE_SIZE);
            }
        } else {
            const auto [vk_buffer, offset] = buffer_cache.ObtainBuffer(
                vsharp.base_address, vsharp.GetSize(), desc.is_written, false, buffer_id);
            const u32 alignment =
                is_storage ? instance.StorageMinAlignment() : instance.UniformMinAlignment();
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            ASSERT(adjust % 4 == 0);
            push_data.AddOffset(binding.buffer, adjust);
            buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned,
                                      vsharp.GetSize() + adjust);
            if (auto barrier =
                    vk_buffer->GetBarrier(desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                                          : vk::AccessFlagBits2::eShaderRead,
                                          vk::PipelineStageFlagBits2::eAllCommands)) {
                buffer_barriers.emplace_back(*barrier);
            }
        }

        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = is_storage ? vk::DescriptorType::eStorageBuffer
                                         : vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &buffer_infos.back(),
        });
        ++binding.buffer;
    }

    const auto null_buffer_view =
        instance.IsNullDescriptorSupported() ? VK_NULL_HANDLE : buffer_cache.NullBufferView();
    for (u32 i = 0; i < texbuffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp] = texbuffer_bindings[i];
        const auto& desc = stage.texture_buffers[i];
        vk::BufferView& buffer_view = buffer_views.emplace_back(null_buffer_view);
        if (buffer_id) {
            const u32 alignment = instance.TexelBufferMinAlignment();
            const auto [vk_buffer, offset] = buffer_cache.ObtainBuffer(
                vsharp.base_address, vsharp.GetSize(), desc.is_written, true, buffer_id);
            const u32 fmt_stride = AmdGpu::NumBits(vsharp.GetDataFmt()) >> 3;
            const u32 buf_stride = vsharp.GetStride();
            ASSERT_MSG(buf_stride % fmt_stride == 0,
                       "Texel buffer stride must match format stride");
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            ASSERT(adjust % fmt_stride == 0);
            push_data.AddTexelOffset(binding.buffer, buf_stride / fmt_stride, adjust / fmt_stride);
            buffer_view =
                vk_buffer->View(offset_aligned, vsharp.GetSize() + adjust, desc.is_written,
                                vsharp.GetDataFmt(), vsharp.GetNumberFmt());
            if (auto barrier =
                    vk_buffer->GetBarrier(desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                                          : vk::AccessFlagBits2::eShaderRead,
                                          vk::PipelineStageFlagBits2::eAllCommands)) {
                buffer_barriers.emplace_back(*barrier);
            }
            if (desc.is_written) {
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, vsharp.GetSize());
            }
        }

        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = desc.is_written ? vk::DescriptorType::eStorageTexelBuffer
                                              : vk::DescriptorType::eUniformTexelBuffer,
            .pTexelBufferView = &buffer_view,
        });
        ++binding.buffer;
    }
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                              Pipeline::DescriptorWrites& set_writes) {
    image_bindings.clear();

    for (const auto& image_desc : stage.images) {
        const auto tsharp = image_desc.GetSharp(stage);
        if (texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        if (tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            continue;
        }

        auto& [image_id, desc] = image_bindings.emplace_back(std::piecewise_construct, std::tuple{},
                                                             std::tuple{tsharp, image_desc});
        image_id = texture_cache.FindImage(desc);
        auto* image = &texture_cache.GetImage(image_id);
        if (image->depth_id) {
            // If this image has an associated depth image, it's a stencil attachment.
            // Redirect the access to the actual depth-stencil buffer.
            image_id = image->depth_id;
            image = &texture_cache.GetImage(image_id);
        }
        if (image->binding.is_bound) {
            // The image is already bound. In case if it is about to be used as storage we need
            // to force general layout on it.
            image->binding.force_general |= image_desc.IsStorage(tsharp);
        }
        if (image->binding.is_target) {
            // The image is already bound as target. Since we read and output to it need to force
            // general layout too.
            image->binding.force_general = 1u;
        }
        image->binding.is_bound = 1u;
    }

    // Second pass to re-bind images that were updated after binding
    for (auto& [image_id, desc] : image_bindings) {
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            if (instance.IsNullDescriptorSupported()) {
                image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            } else {
                auto& null_image = texture_cache.GetImageView(VideoCore::NULL_IMAGE_VIEW_ID);
                image_infos.emplace_back(VK_NULL_HANDLE, *null_image.image_view,
                                         vk::ImageLayout::eGeneral);
            }
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding.Reset(); // clean up previous image binding state
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc.view_info);

            if (image.binding.force_general || image.binding.is_target) {
                image.Transit(vk::ImageLayout::eGeneral,
                              vk::AccessFlagBits2::eShaderRead |
                                  (image.info.IsDepthStencil()
                                       ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                       : vk::AccessFlagBits2::eColorAttachmentWrite),
                              {});
            } else {
                if (is_storage) {
                    image.Transit(vk::ImageLayout::eGeneral,
                                  vk::AccessFlagBits2::eShaderRead |
                                      vk::AccessFlagBits2::eShaderWrite,
                                  desc.view_info.range);
                } else {
                    const auto new_layout = image.info.IsDepthStencil()
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    image.Transit(new_layout, vk::AccessFlagBits2::eShaderRead,
                                  desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.last_state.layout);
        }

        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType =
                is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage,
            .pImageInfo = &image_infos.back(),
        });
    }

    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (sampler.disable_aniso) {
            const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        }
        const auto vk_sampler = texture_cache.GetSampler(ssharp);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampler,
            .pImageInfo = &image_infos.back(),
        });
    }
}

void Rasterizer::BeginRendering(const GraphicsPipeline& pipeline, RenderState& state) {
    int cb_index = 0;
    for (auto& [image_id, desc] : cb_descs) {
        if (auto& old_img = texture_cache.GetImage(image_id); old_img.binding.needs_rebind) {
            auto& view = texture_cache.FindRenderTarget(desc);
            ASSERT(view.image_id != image_id);
            image_id = bound_images.emplace_back(view.image_id);
            auto& image = texture_cache.GetImage(view.image_id);
            state.color_attachments[cb_index].imageView = *view.image_view;
            state.color_attachments[cb_index].imageLayout = image.last_state.layout;

            const auto mip = view.info.range.base.level;
            state.width = std::min<u32>(state.width, std::max(image.info.size.width >> mip, 1u));
            state.height = std::min<u32>(state.height, std::max(image.info.size.height >> mip, 1u));
            ASSERT(old_img.info.size.width == state.width);
            ASSERT(old_img.info.size.height == state.height);
        }
        auto& image = texture_cache.GetImage(image_id);
        if (image.binding.force_general) {
            image.Transit(
                vk::ImageLayout::eGeneral,
                vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eShaderRead, {});

        } else {
            image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
                          vk::AccessFlagBits2::eColorAttachmentWrite |
                              vk::AccessFlagBits2::eColorAttachmentRead,
                          desc.view_info.range);
        }
        image.usage.render_target = 1u;
        state.color_attachments[cb_index].imageLayout = image.last_state.layout;
        ++cb_index;
    }

    if (db_desc) {
        const auto& image_id = std::get<0>(*db_desc);
        const auto& desc = std::get<1>(*db_desc);
        auto& image = texture_cache.GetImage(image_id);
        ASSERT(image.binding.needs_rebind == 0);
        const bool has_stencil = image.usage.stencil;
        if (has_stencil) {
            image.aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
        if (image.binding.force_general) {
            image.Transit(vk::ImageLayout::eGeneral,
                          vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                              vk::AccessFlagBits2::eShaderRead,
                          {});
        } else {
            const auto new_layout = desc.view_info.is_storage
                                        ? has_stencil
                                              ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                              : vk::ImageLayout::eDepthAttachmentOptimal
                                    : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                  : vk::ImageLayout::eDepthReadOnlyOptimal;
            image.Transit(new_layout,
                          vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                              vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                          desc.view_info.range);
        }
        state.depth_attachment.imageLayout = image.last_state.layout;
        state.stencil_attachment.imageLayout = image.last_state.layout;
        image.usage.depth_target = true;
        image.usage.stencil = has_stencil;
    }

    scheduler.BeginRendering(state);
}

void Rasterizer::Resolve() {
    const auto cmdbuf = scheduler.CommandBuffer();

    // Read from MRT0, average all samples, and write to MRT1, which is one-sample
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::RenderTargetDesc mrt0_desc{liverpool->regs.color_buffers[0],
                                                        mrt0_hint};
    VideoCore::TextureCache::RenderTargetDesc mrt1_desc{liverpool->regs.color_buffers[1],
                                                        mrt1_hint};
    auto& mrt0_image =
        texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, VideoCore::FindFlags::ExactFmt));
    auto& mrt1_image =
        texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, VideoCore::FindFlags::ExactFmt));

    VideoCore::SubresourceRange mrt0_range;
    mrt0_range.base.layer = liverpool->regs.color_buffers[0].view.slice_start;
    mrt0_range.extent.layers = liverpool->regs.color_buffers[0].NumSlices() - mrt0_range.base.layer;
    VideoCore::SubresourceRange mrt1_range;
    mrt1_range.base.layer = liverpool->regs.color_buffers[1].view.slice_start;
    mrt1_range.extent.layers = liverpool->regs.color_buffers[1].NumSlices() - mrt1_range.base.layer;

    mrt0_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       mrt0_range);

    mrt1_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                       mrt1_range);

    if (mrt0_image.info.num_samples == 1) {
        // Vulkan does not allow resolve from a single sample image, so change it to a copy.
        // Note that resolving a single-sampled image doesn't really make sense, but a game might do
        // it.
        vk::ImageCopy region = {
            .srcSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = mrt0_range.base.layer,
                    .layerCount = mrt0_range.extent.layers,
                },
            .srcOffset = {0, 0, 0},
            .dstSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = mrt1_range.base.layer,
                    .layerCount = mrt1_range.extent.layers,
                },
            .dstOffset = {0, 0, 0},
            .extent = {mrt1_image.info.size.width, mrt1_image.info.size.height, 1},
        };
        cmdbuf.copyImage(mrt0_image.image, vk::ImageLayout::eTransferSrcOptimal, mrt1_image.image,
                         vk::ImageLayout::eTransferDstOptimal, region);
    } else {
        vk::ImageResolve region = {
            .srcSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = mrt0_range.base.layer,
                    .layerCount = mrt0_range.extent.layers,
                },
            .srcOffset = {0, 0, 0},
            .dstSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = mrt1_range.base.layer,
                    .layerCount = mrt1_range.extent.layers,
                },
            .dstOffset = {0, 0, 0},
            .extent = {mrt1_image.info.size.width, mrt1_image.info.size.height, 1},
        };
        cmdbuf.resolveImage(mrt0_image.image, vk::ImageLayout::eTransferSrcOptimal,
                            mrt1_image.image, vk::ImageLayout::eTransferDstOptimal, region);
    }
}

void Rasterizer::InlineData(VAddr address, const void* value, u32 num_bytes, bool is_gds) {
    buffer_cache.InlineData(address, value, num_bytes, is_gds);
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    return mapped_ranges.find(boost::icl::interval<VAddr>::right_open(addr, addr + size)) !=
           mapped_ranges.end();
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    mapped_ranges += boost::icl::interval<VAddr>::right_open(addr, addr + size);
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    page_manager.OnGpuUnmap(addr, size);
    mapped_ranges -= boost::icl::interval<VAddr>::right_open(addr, addr + size);
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline& pipeline) {
    UpdateViewportScissorState();

    auto& regs = liverpool->regs;
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.setBlendConstants(&regs.blend_constants.red);

    if (instance.IsColorWriteEnableSupported()) {
        const auto& write_masks = pipeline.GetWriteMasks();
        std::array<vk::Bool32, Liverpool::NumColorBuffers> write_ens{};
        std::transform(write_masks.cbegin(), write_masks.cend(), write_ens.begin(),
                       [](auto in) { return in ? vk::True : vk::False; });

        cmdbuf.setColorWriteEnableEXT(write_ens);
        cmdbuf.setColorWriteMaskEXT(0, write_masks);
    }
    if (regs.depth_control.depth_bounds_enable) {
        cmdbuf.setDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }
    if (regs.polygon_control.enable_polygon_offset_front) {
        cmdbuf.setDepthBias(regs.poly_offset.front_offset, regs.poly_offset.depth_bias,
                            regs.poly_offset.front_scale / 16.f);
    } else if (regs.polygon_control.enable_polygon_offset_back) {
        cmdbuf.setDepthBias(regs.poly_offset.back_offset, regs.poly_offset.depth_bias,
                            regs.poly_offset.back_scale / 16.f);
    }

    if (regs.depth_control.stencil_enable) {
        const auto front_fail_op =
            LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front);
        const auto front_pass_op =
            LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front);
        const auto front_depth_fail_op =
            LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front);
        const auto front_compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func);
        if (regs.depth_control.backface_enable) {
            const auto back_fail_op =
                LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back);
            const auto back_pass_op =
                LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back);
            const auto back_depth_fail_op =
                LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back);
            const auto back_compare_op =
                LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func);
            cmdbuf.setStencilOpEXT(vk::StencilFaceFlagBits::eFront, front_fail_op, front_pass_op,
                                   front_depth_fail_op, front_compare_op);
            cmdbuf.setStencilOpEXT(vk::StencilFaceFlagBits::eBack, back_fail_op, back_pass_op,
                                   back_depth_fail_op, back_compare_op);
        } else {
            cmdbuf.setStencilOpEXT(vk::StencilFaceFlagBits::eFrontAndBack, front_fail_op,
                                   front_pass_op, front_depth_fail_op, front_compare_op);
        }

        const auto front = regs.stencil_ref_front;
        const auto back = regs.stencil_ref_back;
        if (front.stencil_test_val == back.stencil_test_val) {
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       front.stencil_test_val);
        } else {
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront, front.stencil_test_val);
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, back.stencil_test_val);
        }

        if (front.stencil_write_mask == back.stencil_write_mask) {
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       front.stencil_write_mask);
        } else {
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront, front.stencil_write_mask);
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, back.stencil_write_mask);
        }

        if (front.stencil_mask == back.stencil_mask) {
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         front.stencil_mask);
        } else {
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront, front.stencil_mask);
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack, back.stencil_mask);
        }
    }
}

void Rasterizer::UpdateViewportScissorState() {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable.Value();

    Liverpool::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x.Value()),
        s16(regs.generic_scissor.top_left_x.Value()),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y.Value()),
        s16(regs.generic_scissor.top_left_y.Value()),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, Liverpool::NumViewports> viewports;
    boost::container::static_vector<vk::Rect2D, Liverpool::NumViewports> scissors;

    const auto& vp_ctl = regs.viewport_control;
    const float reduce_z =
        instance.IsDepthClipControlSupported() &&
                regs.clipper_control.clip_space == AmdGpu::Liverpool::ClipSpace::MinusWToW
            ? 1.0f
            : 0.0f;

    for (u32 i = 0; i < Liverpool::NumViewports; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }
        const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
        const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
        const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
        const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;
        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;
        viewports.push_back({
            .x = xoffset - xscale,
            .y = yoffset - yscale,
            .width = xscale * 2.0f,
            .height = yscale * 2.0f,
            .minDepth = zoffset - zscale * reduce_z,
            .maxDepth = zscale + zoffset,
        });

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x.Value()));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y.Value()));
            vp_scsr.bottom_right_x =
                std::min(vp_scsr.bottom_right_x, regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y =
                std::min(vp_scsr.bottom_right_y, regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = 0.0f,
            .y = 0.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.setViewportWithCountEXT(viewports);
    cmdbuf.setScissorWithCountEXT(scissors);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str) {
    if (Config::nullGpu() || !Config::vkMarkersEnabled()) {
        return;
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd() {
    if (Config::nullGpu() || !Config::vkMarkersEnabled()) {
        return;
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str) {
    if (Config::nullGpu() || !Config::vkMarkersEnabled()) {
        return;
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color) {
    if (Config::nullGpu() || !Config::vkMarkersEnabled()) {
        return;
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

} // namespace Vulkan
