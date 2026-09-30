/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_cmd_buffer.h"

#include "nvk_buffer.h"
#include "nvk_cmd_pool.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_entrypoints.h"
#include "nvk_event.h"
#include "nvk_image.h"
#include "nvk_mme.h"
#include "nvk_physical_device.h"
#include "nvk_shader.h"
#include "nvkmd/nvkmd.h"

#include "vk_pipeline_layout.h"
#ifdef HAVE_SWITCH_PLATFORM
#include "vk_pipeline.h" /* NFSMW: vk_switch_precarga */
#endif
#include "vk_synchronization.h"
#include "util/compiler.h"

#include "clb097.h"
#include "clb197.h"
#include "clcb97.h"
#include "nv_push_cl906f.h"
#include "nv_push_cla16f.h"
#include "nv_push_cl9097.h"
#include "nv_push_cl90c0.h"
#include "nv_push_cl90b5.h"
#include "nv_push_cla097.h"
#include "nv_push_cla0c0.h"
#include "nv_push_clb06f.h"
#include "nv_push_clb1c0.h"
#include "nv_push_clc597.h"
#include "nv_push_clc86f.h"

static uint8_t
nvk_cmd_buffer_subchannel_mask(struct nvk_cmd_buffer *cmd);

void
nvk_descriptor_state_fini(struct nvk_cmd_buffer *cmd,
                          struct nvk_descriptor_state *desc)
{
   struct nvk_cmd_pool *pool = nvk_cmd_buffer_pool(cmd);

   for (unsigned i = 0; i < NVK_MAX_SETS; i++) {
      vk_free(&pool->vk.alloc, desc->sets[i].push);
      desc->sets[i].push = NULL;
   }
}

static void
nvk_destroy_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer)
{
   struct nvk_cmd_buffer *cmd =
      container_of(vk_cmd_buffer, struct nvk_cmd_buffer, vk);
   struct nvk_cmd_pool *pool = nvk_cmd_buffer_pool(cmd);

   nvk_descriptor_state_fini(cmd, &cmd->state.gfx.descriptors);
   nvk_descriptor_state_fini(cmd, &cmd->state.cs.descriptors);

   nvk_cmd_pool_free_mem_list(pool, &cmd->owned_mem);
   nvk_cmd_pool_free_gart_mem_list(pool, &cmd->owned_gart_mem);
   nvk_cmd_pool_free_qmd_list(pool, &cmd->owned_qmd);
   util_dynarray_fini(&cmd->pushes);
   util_dynarray_fini(&cmd->copy_memory_indirect_temps);
   vk_command_buffer_finish(&cmd->vk);
   vk_free(&pool->vk.alloc, cmd);
}

static VkResult
nvk_create_cmd_buffer(struct vk_command_pool *vk_pool,
                      VkCommandBufferLevel level,
                      struct vk_command_buffer **cmd_buffer_out)
{
   struct nvk_cmd_pool *pool = container_of(vk_pool, struct nvk_cmd_pool, vk);
   struct nvk_device *dev = nvk_cmd_pool_device(pool);
   struct nvk_cmd_buffer *cmd;
   VkResult result;

   cmd = vk_zalloc(&pool->vk.alloc, sizeof(*cmd), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = vk_command_buffer_init(&pool->vk, &cmd->vk,
                                   &nvk_cmd_buffer_ops, level);
   if (result != VK_SUCCESS) {
      vk_free(&pool->vk.alloc, cmd);
      return result;
   }

   cmd->vk.dynamic_graphics_state.vi = &cmd->state.gfx._dynamic_vi;
   cmd->vk.dynamic_graphics_state.ms.sample_locations =
      &cmd->state.gfx._dynamic_sl;

   list_inithead(&cmd->owned_mem);
   list_inithead(&cmd->owned_gart_mem);
   list_inithead(&cmd->owned_qmd);
   cmd->pushes = UTIL_DYNARRAY_INIT;
   cmd->copy_memory_indirect_temps = UTIL_DYNARRAY_INIT;

   cmd->prev_subc = ffs(nvk_cmd_buffer_subchannel_mask(cmd)) - 1;

   *cmd_buffer_out = &cmd->vk;

   return VK_SUCCESS;
}

static void
nvk_reset_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer,
                     UNUSED VkCommandBufferResetFlags flags)
{
   struct nvk_cmd_buffer *cmd =
      container_of(vk_cmd_buffer, struct nvk_cmd_buffer, vk);
   struct nvk_cmd_pool *pool = nvk_cmd_buffer_pool(cmd);

   vk_command_buffer_reset(&cmd->vk);

   nvk_descriptor_state_fini(cmd, &cmd->state.gfx.descriptors);
   nvk_descriptor_state_fini(cmd, &cmd->state.cs.descriptors);

   nvk_cmd_pool_free_mem_list(pool, &cmd->owned_mem);
   nvk_cmd_pool_free_gart_mem_list(pool, &cmd->owned_gart_mem);
   nvk_cmd_pool_free_qmd_list(pool, &cmd->owned_qmd);
   cmd->upload_mem = NULL;
   cmd->push_mem = NULL;
   cmd->push_mem_limit = NULL;
   cmd->push = (struct nv_push) {0};
   cmd->cond_render_mem = NULL;

   util_dynarray_clear(&cmd->pushes);
   util_dynarray_clear(&cmd->copy_memory_indirect_temps);
#ifdef HAVE_SWITCH_PLATFORM
   cmd->switch_mme_sync_pending = false;
#endif

   memset(&cmd->state, 0, sizeof(cmd->state));
}

VkQueueFlags
nvk_cmd_buffer_queue_flags(struct nvk_cmd_buffer *cmd)
{
   const struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   uint32_t queue_family_index = cmd->vk.pool->queue_family_index;
   assert(queue_family_index < pdev->queue_family_count);
   const struct nvk_queue_family *queue_family =
      &pdev->queue_families[queue_family_index];

   return queue_family->queue_flags;
}

static uint8_t
nvk_cmd_buffer_subchannel_mask(struct nvk_cmd_buffer *cmd)
{
   VkQueueFlags queue_flags = nvk_cmd_buffer_queue_flags(cmd);
   enum nvkmd_engines engines =
      nvk_queue_engines_from_queue_flags(queue_flags);
   return nvk_queue_subchannels_from_engines(engines);
}

const struct vk_command_buffer_ops nvk_cmd_buffer_ops = {
   .create = nvk_create_cmd_buffer,
   .reset = nvk_reset_cmd_buffer,
   .destroy = nvk_destroy_cmd_buffer,
};

/* If we ever fail to allocate a push, we use this */
static uint32_t push_runout[NVK_CMD_BUFFER_MAX_PUSH];

static void
nvk_cmd_mem_mark_dirty(struct nvk_cmd_mem *mem, uint32_t end_B)
{
   mem->dirty_end_B = MAX2(mem->dirty_end_B, end_B);
}

static void
nvk_cmd_mem_flush(struct nvk_device *dev, struct nvk_cmd_mem *mem)
{
   if (mem->dirty_end_B == 0)
      return;

   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   const uint32_t atom_size_B = pdev->info.nc_atom_size_B;
   const uint64_t flush_size_B =
      MIN2(align(mem->dirty_end_B, atom_size_B), mem->mem->size_B);

   nvkmd_mem_sync_map_to_gpu(mem->mem, 0, flush_size_B);
   mem->dirty_end_B = 0;
}

VkResult
nvk_cmd_buffer_alloc_mem(struct nvk_cmd_buffer *cmd, bool force_gart,
                         struct nvk_cmd_mem **mem_out)
{
   VkResult result = nvk_cmd_pool_alloc_mem(nvk_cmd_buffer_pool(cmd),
                                            force_gart, mem_out);
   if (result != VK_SUCCESS)
      return result;

   if (force_gart)
      list_addtail(&(*mem_out)->link, &cmd->owned_gart_mem);
   else
      list_addtail(&(*mem_out)->link, &cmd->owned_mem);

   return VK_SUCCESS;
}

static void
nvk_cmd_buffer_flush_push_flags(struct nvk_cmd_buffer *cmd,
                                bool incomplete, bool no_prefetch)
{
   if (likely(cmd->push_mem != NULL)) {
      const uint32_t mem_offset =
         (char *)cmd->push.start - (char *)cmd->push_mem->mem->map;
      const uint32_t range = nv_push_dw_count(&cmd->push) * 4;

      if (range > 0) {
         struct nvk_cmd_push push = {
            .map = cmd->push.start,
            .addr = cmd->push_mem->mem->va->addr + mem_offset,
            .range = range,
            .incomplete = incomplete,
            .no_prefetch = no_prefetch,
         };

         nvk_cmd_mem_mark_dirty(cmd->push_mem, mem_offset + range);

         if (cmd->pushes.size >= sizeof(struct nvk_cmd_push)) {
            struct nvk_cmd_push *last =
               util_dynarray_top_ptr(&cmd->pushes, struct nvk_cmd_push);
            /* SYNC/no-prefetch applies at entry start. An existing SYNC
             * entry can absorb following contiguous commands, but a new
             * SYNC segment must retain its start boundary.
             */
            if (!push.no_prefetch &&
                last->map != NULL &&
                (char *)last->map + last->range == push.map &&
                last->addr + last->range == push.addr) {
               last->range += push.range;
               last->incomplete = push.incomplete;
               cmd->prev_subc = NVC0_FIFO_SUBC_FROM_PKHDR(cmd->push.last_hdr_dw);
               cmd->push.start = cmd->push.end;
               return;
            }
         }

         util_dynarray_append(&cmd->pushes, push);

         cmd->prev_subc = NVC0_FIFO_SUBC_FROM_PKHDR(cmd->push.last_hdr_dw);
      }
   }

   cmd->push.start = cmd->push.end;
}

static void
nvk_cmd_buffer_flush_push(struct nvk_cmd_buffer *cmd, bool incomplete)
{
   nvk_cmd_buffer_flush_push_flags(cmd, incomplete, false);
}

#ifdef HAVE_SWITCH_PLATFORM
static void
nvk_cmd_buffer_flush_no_prefetch_push(struct nvk_cmd_buffer *cmd)
{
   nvk_cmd_buffer_flush_push_flags(cmd, false, true);
}
#endif

void
nvk_cmd_buffer_new_push(struct nvk_cmd_buffer *cmd)
{
#ifdef HAVE_SWITCH_PLATFORM
   const uint64_t t_sw = nvk_sw_ticks(); /* NFSMW: always, this path is rare */
#endif
   nvk_cmd_buffer_flush_push(cmd, false);

   uint8_t subc_mask = nvk_cmd_buffer_subchannel_mask(cmd);

   /* Strictly speaking, pushbufs don't need to live in GART but the command
    * streamer is pretty efficient at pulling across PCI and command buffers tend
    * to be read-once so there's not much benefit to putting them in VRAM.
    */
   VkResult result = nvk_cmd_buffer_alloc_mem(cmd, true, &cmd->push_mem);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(&cmd->vk, result);
      STATIC_ASSERT(NVK_CMD_BUFFER_MAX_PUSH <= NVK_CMD_MEM_SIZE / 4);
      cmd->push_mem = NULL;
      nv_push_init(&cmd->push, push_runout, 0, subc_mask);
      cmd->push_mem_limit = &push_runout[NVK_CMD_BUFFER_MAX_PUSH];
   } else {
      nv_push_init(&cmd->push, cmd->push_mem->mem->map, 0, subc_mask);
      cmd->push_mem_limit =
         (uint32_t *)((char *)cmd->push_mem->mem->map + NVK_CMD_MEM_SIZE);
   }
#ifdef HAVE_SWITCH_PLATFORM
   cmd->state.sw.cuentas[NVK_SW_C_TROZOS]++;
   cmd->state.sw.partes[NVK_SW_P_TROZO].veces++;
   cmd->state.sw.partes[NVK_SW_P_TROZO].ticks += nvk_sw_ticks() - t_sw;
#endif
}

void
nvk_cmd_buffer_push_indirect(struct nvk_cmd_buffer *cmd,
                             uint64_t addr, uint32_t range)
{
#ifdef HAVE_SWITCH_PLATFORM
   if (range > 0)
      nvk_cmd_buffer_switch_mme_consumer(cmd);
#endif

   nvk_cmd_buffer_flush_push(cmd, true);

   struct nvk_cmd_push push = {
      .addr = addr,
      .range = range,
      .no_prefetch = true,
   };

   util_dynarray_append(&cmd->pushes, push);
}

VkResult
nvk_cmd_buffer_upload_alloc(struct nvk_cmd_buffer *cmd,
                            uint32_t size, uint32_t alignment,
                            uint64_t *addr, void **ptr)
{
   assert(size % 4 == 0);
   assert(size <= NVK_CMD_MEM_SIZE);

   uint32_t offset = cmd->upload_offset;
   if (alignment > 0)
      offset = align(offset, alignment);

   assert(offset <= NVK_CMD_MEM_SIZE);
   if (cmd->upload_mem != NULL && size <= NVK_CMD_MEM_SIZE - offset) {
      *addr = cmd->upload_mem->mem->va->addr + offset;
      *ptr = (char *)cmd->upload_mem->mem->map + offset;

      cmd->upload_offset = offset + size;
      nvk_cmd_mem_mark_dirty(cmd->upload_mem, cmd->upload_offset);

      return VK_SUCCESS;
   }

   struct nvk_cmd_mem *mem;
   VkResult result = nvk_cmd_buffer_alloc_mem(cmd, false, &mem);
   if (unlikely(result != VK_SUCCESS))
      return result;

   *addr = mem->mem->va->addr;
   *ptr = mem->mem->map;
   nvk_cmd_mem_mark_dirty(mem, size);

   /* Pick whichever of the current upload BO and the new BO will have more
    * room left to be the BO for the next upload.  If our upload size is
    * bigger than the old offset, we're better off burning the whole new
    * upload BO on this one allocation and continuing on the current upload
    * BO.
    */
   if (cmd->upload_mem == NULL || size < cmd->upload_offset) {
      cmd->upload_mem = mem;
      cmd->upload_offset = size;
   }

   return VK_SUCCESS;
}

VkResult
nvk_cmd_buffer_upload_data(struct nvk_cmd_buffer *cmd,
                           const void *data, uint32_t size,
                           uint32_t alignment, uint64_t *addr)
{
   VkResult result;
   void *map;

   result = nvk_cmd_buffer_upload_alloc(cmd, size, alignment, addr, &map);
   if (unlikely(result != VK_SUCCESS))
      return result;

   memcpy(map, data, size);

   return VK_SUCCESS;
}

#ifdef HAVE_SWITCH_PLATFORM
static void
nvk_cmd_buffer_switch_report_semaphore(struct nvk_cmd_buffer *cmd,
                                       uint64_t addr, uint32_t value)
{
   const uint8_t subc = nvk_cmd_buffer_last_subchannel(cmd);

   if (subc == SUBC_NV90C0) {
      struct nv_push *p = nvk_cmd_buffer_push(cmd, 6);
      P_IMMD(p, NV90C0, WAIT_FOR_IDLE, 0);
      P_MTHD(p, NV90C0, SET_REPORT_SEMAPHORE_A);
      P_NV90C0_SET_REPORT_SEMAPHORE_A(p, addr >> 32);
      P_NV90C0_SET_REPORT_SEMAPHORE_B(p, addr);
      P_NV90C0_SET_REPORT_SEMAPHORE_C(p, value);
      P_NV90C0_SET_REPORT_SEMAPHORE_D(p, {
         .operation = OPERATION_RELEASE,
         .structure_size = STRUCTURE_SIZE_ONE_WORD,
      });
   } else if (subc == SUBC_NV90B5) {
      /* A pipelined copy-engine launch may signal its semaphore before all
       * earlier writes are globally visible on GM20B.  Use a non-pipelined,
       * flushed release before the cross-engine acquire below.
       */
      struct nv_push *p = nvk_cmd_buffer_push(cmd, 9);
      P_MTHD(p, NV90B5, SET_SEMAPHORE_A);
      P_NV90B5_SET_SEMAPHORE_A(p, addr >> 32);
      P_NV90B5_SET_SEMAPHORE_B(p, addr);
      P_NV90B5_SET_SEMAPHORE_PAYLOAD(p, value);

      P_MTHD(p, NV90B5, LINE_LENGTH_IN);
      P_NV90B5_LINE_LENGTH_IN(p, 0);
      P_NV90B5_LINE_COUNT(p, 0);

      P_IMMD(p, NV90B5, LAUNCH_DMA, {
         .data_transfer_type = DATA_TRANSFER_TYPE_NON_PIPELINED,
         .semaphore_type = SEMAPHORE_TYPE_RELEASE_ONE_WORD_SEMAPHORE,
         .flush_enable = FLUSH_ENABLE_TRUE,
         /* Note: FLUSH_TYPE=SYS implicitly for NVC3B5+ */
      });
   } else {
      assert(subc == SUBC_NV9097);
      struct nv_push *p = nvk_cmd_buffer_push(cmd, 7);
      P_IMMD(p, NV9097, FLUSH_PENDING_WRITES, 0);
      P_MTHD(p, NV9097, SET_REPORT_SEMAPHORE_A);
      P_NV9097_SET_REPORT_SEMAPHORE_A(p, addr >> 32);
      P_NV9097_SET_REPORT_SEMAPHORE_B(p, addr);
      P_NV9097_SET_REPORT_SEMAPHORE_C(p, value);
      P_NV9097_SET_REPORT_SEMAPHORE_D(p, {
         .operation = OPERATION_RELEASE,
         .release = RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE,
         .pipeline_location = PIPELINE_LOCATION_ALL,
         .structure_size = STRUCTURE_SIZE_ONE_WORD,
      });
   }
}

static void
nvk_cmd_buffer_switch_defer_mme_sync(struct nvk_cmd_buffer *cmd)
{
   cmd->switch_mme_sync_pending = true;
}

static void
nvk_cmd_buffer_switch_flush_pending_mme_sync(struct nvk_cmd_buffer *cmd)
{
   if (!cmd->switch_mme_sync_pending)
      return;

   nvk_cmd_buffer_switch_sync_host(cmd);
}

void
nvk_cmd_buffer_switch_mme_consumer(struct nvk_cmd_buffer *cmd)
{
   nvk_cmd_buffer_switch_flush_pending_mme_sync(cmd);
}

void
nvk_cmd_buffer_switch_sync_host(struct nvk_cmd_buffer *cmd)
{
   uint64_t addr;
   void *map;
   VkResult result =
      nvk_cmd_buffer_upload_alloc(cmd, sizeof(uint32_t), 4, &addr, &map);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   *(uint32_t *)map = 0;

   cmd->switch_mme_sync_pending = false;

   /* Reset the semaphore on every execution; record-time initialization
    * leaves a stale 1 on resubmission, allowing an early acquire. Disable
    * RELEASE_WFI for an in-order PBDMA store without waiting for channel
    * idle.
    */
   struct nv_push *p = nvk_cmd_buffer_push(cmd, 5);
   __push_mthd(p, SUBC_NV9097, NV906F_SEMAPHOREA);
   P_NV906F_SEMAPHOREA(p, addr >> 32);
   P_NV906F_SEMAPHOREB(p, (addr & UINT32_MAX) >> 2);
   P_NV906F_SEMAPHOREC(p, 0);
   P_NV906F_SEMAPHORED(p, {
      .operation = OPERATION_RELEASE,
      .release_wfi = RELEASE_WFI_DIS,
      .release_size = RELEASE_SIZE_4BYTE,
   });

   /* Use a semaphore release/acquire pair followed by a GPFIFO
    * sync/no-prefetch split.  This avoids the channel host-WFI from
    * SET_REFERENCE while still stopping host prefetch before later indirect
    * GPFIFO segments.
    */
   nvk_cmd_buffer_switch_report_semaphore(cmd, addr, 1);

   p = nvk_cmd_buffer_push(cmd, 5);
   __push_mthd(p, SUBC_NV9097, NV906F_SEMAPHOREA);
   P_NV906F_SEMAPHOREA(p, addr >> 32);
   P_NV906F_SEMAPHOREB(p, (addr & UINT32_MAX) >> 2);
   P_NV906F_SEMAPHOREC(p, 1);
   P_NV906F_SEMAPHORED(p, {
      .operation = OPERATION_ACQ_GEQ,
      .acquire_switch = ACQUIRE_SWITCH_ENABLED,
      .release_size = RELEASE_SIZE_4BYTE,
   });

   nvk_cmd_buffer_flush_push(cmd, false);

   /* The no-prefetch bit is the GPFIFO sync bit on GM20B.  Use a tiny NOP
    * segment so following GPFIFO entries cannot be prefetched before the
    * semaphore acquire above has completed.
    */
   p = nvk_cmd_buffer_push(cmd, 2);
   P_MTHD(p, NV9097, NO_OPERATION);
   P_INLINE_DATA(p, 0);
   nvk_cmd_buffer_flush_no_prefetch_push(cmd);
}
#endif

VkResult
nvk_cmd_buffer_alloc_qmd(struct nvk_cmd_buffer *cmd,
                         uint32_t size, uint32_t alignment,
                         uint64_t *addr, void **ptr)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   /* On Maxwell B and later, we have INVALIDATE_SKED_CACHES so we can just
    * allocate from wherever we want (the upload stream in this case).
    */
   if (pdev->info.cls_compute >= MAXWELL_COMPUTE_B)
      return nvk_cmd_buffer_upload_alloc(cmd, size, alignment, addr, ptr);

   /* The GPU compute scheduler (SKED) has a cache.  Maxwell B added the
    * INVALIDATE_SKED_CACHES instruction to manage the SKED cache.  We call
    * that at the top of every command buffer so that we always pick up
    * whatever QMDs we've written from the CPU fresh.  On Maxwell A and
    * earlier, the SKED cache still exists in some form but we have no way to
    * invalidate it.  If a compute shader has been dispatched from a QMD at an
    * address that's no longer valid, the SKED cache can fault.  To work
    * around this, we have a QMD heap on the device and we allocate QMDs from
    * that on Maxwell A and earlier.
    *
    * Prior to Maxwell B, the GPU doesn't seem to need any sort of SKED cache
    * invalidation to pick up new writes from the CPU.  However, we do still
    * have to worry about faults that may be caused by the SKED cache
    * containing a stale address.  Just allocating all QMDs from a central
    * heap which never throws memory away seems to be sufficient for this.
    */
   assert(size <= NVK_CMD_QMD_SIZE);
   assert(alignment <= NVK_CMD_QMD_SIZE);

   struct nvk_cmd_qmd *qmd;
   VkResult result = nvk_cmd_pool_alloc_qmd(nvk_cmd_buffer_pool(cmd), &qmd);
   if (unlikely(result != VK_SUCCESS))
      return result;

   list_addtail(&qmd->link, &cmd->owned_qmd);

   *addr = qmd->addr;
   *ptr = qmd->map;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                       const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VkQueueFlags queue_flags = nvk_cmd_buffer_queue_flags(cmd);

   nvk_reset_cmd_buffer(&cmd->vk, 0);
#ifdef HAVE_SWITCH_PLATFORM
   nvk_sw_empezar_bufer(cmd); /* NFSMW: measurement and improvements for this buffer */
#endif

   if (cmd->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
      cmd->state.inherited_pipeline_statistics =
         pBeginInfo->pInheritanceInfo->pipelineStatistics;

   /* Give empty command buffers a NOP on an initialized subchannel.
    * Touching the copy engine first can fault on Switch.
    */
   struct nv_push *p = nvk_cmd_buffer_push(cmd, 2);
   if (queue_flags & VK_QUEUE_GRAPHICS_BIT) {
      P_MTHD(p, NV9097, NO_OPERATION);
      P_INLINE_DATA(p, 0);
   } else if (queue_flags & VK_QUEUE_COMPUTE_BIT) {
      P_MTHD(p, NV90C0, NO_OPERATION);
      P_INLINE_DATA(p, 0);
   } else {
      P_MTHD(p, NV90B5, NOP);
      P_NV90B5_NOP(p, 0);
   }

   if (queue_flags & VK_QUEUE_COMPUTE_BIT)
      nvk_cmd_buffer_begin_compute(cmd, pBeginInfo);

   if (queue_flags & VK_QUEUE_GRAPHICS_BIT)
      nvk_cmd_buffer_begin_graphics(cmd, pBeginInfo);

   return VK_SUCCESS;
}

static void
flush_mem_list(struct nvk_cmd_buffer *cmd, struct list_head *mem_list)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);

   list_for_each_entry_safe(struct nvk_cmd_mem, mem, mem_list, link)
      nvk_cmd_mem_flush(dev, mem);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

#ifdef HAVE_SWITCH_PLATFORM
   /* A barrier at the end of one command buffer may synchronize a consumer
    * in a later command buffer.  Keep that cross-buffer case conservative;
    * the common in-buffer case is still deferred to its first real consumer.
    */
   nvk_cmd_buffer_switch_flush_pending_mme_sync(cmd);
   nvk_switch_set4_sumar(cmd); /* NFSMW */
   nvk_sw_sumar(cmd); /* NFSMW */
#endif

   nvk_cmd_buffer_flush_push(cmd, false);

   /* We only need to flush the memory objects we own because, if there are
    * secondaries, they will have been flushed in their EndCommandBuffer()
    * call.
    */
   flush_mem_list(cmd, &cmd->owned_mem);
   flush_mem_list(cmd, &cmd->owned_gart_mem);

   return vk_command_buffer_get_record_result(&cmd->vk);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                       uint32_t commandBufferCount,
                       const VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (commandBufferCount == 0)
      return;

#ifdef HAVE_SWITCH_PLATFORM
   /* A secondary may begin with an indirect consumer.  Its recorded pushes
    * cannot observe pending primary state, so close that boundary first.
    */
   nvk_cmd_buffer_switch_flush_pending_mme_sync(cmd);
#endif

   nvk_cmd_buffer_flush_push(cmd, false);

   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(nvk_cmd_buffer, other, pCommandBuffers[i]);

      /* We only need to copy the pushes.  We do not copy the
       * nvk_cmd_buffer::bos because that tracks ownership.  Instead, we
       * depend on the app to not discard secondaries while they are used by a
       * primary.  The Vulkan 1.3.227 spec for vkFreeCommandBuffers() says:
       *
       *    "Any primary command buffer that is in the recording or executable
       *    state and has any element of pCommandBuffers recorded into it,
       *    becomes invalid."
       *
       * In other words, if the secondary command buffer ever goes away, this
       * command buffer is invalid and the only thing the client can validly
       * do with it is reset it.  vkResetCommandPool() has similar language.
       */
      util_dynarray_append_dynarray(&cmd->pushes, &other->pushes);
#ifdef HAVE_SWITCH_PLATFORM
      assert(!other->switch_mme_sync_pending);
#endif

      cmd->prev_subc = nvk_cmd_buffer_last_subchannel(other);
   }

   /* From the Vulkan 1.3.275 spec:
    *
    *    "When secondary command buffer(s) are recorded to execute on a
    *    primary command buffer, the secondary command buffer inherits no
    *    state from the primary command buffer, and all state of the primary
    *    command buffer is undefined after an execute secondary command buffer
    *    command is recorded. There is one exception to this rule - if the
    *    primary command buffer is inside a render pass instance, then the
    *    render pass and subpass state is not disturbed by executing secondary
    *    command buffers. For state dependent commands (such as draws and
    *    dispatches), any state consumed by those commands must not be
    *    undefined."
    *
    * Therefore, it's the client's job to reset all the state in the primary
    * after the secondary executes.  However, if we're doing any internal
    * dirty tracking, we may miss the fact that a secondary has messed with
    * GPU state if we don't invalidate all our internal tracking.
   */
   nvk_cmd_invalidate_graphics_state(cmd);
   nvk_cmd_invalidate_compute_state(cmd);
}

enum nvk_barrier {
   NVK_BARRIER_WFI                        = 1 << 0,
   NVK_BARRIER_FLUSH_SHADER_DATA          = 1 << 1,
   NVK_BARRIER_INVALIDATE_SHADER_DATA     = 1 << 2,
   NVK_BARRIER_INVALIDATE_TEX_DATA        = 1 << 3,
   NVK_BARRIER_INVALIDATE_CONSTANT        = 1 << 4,
   NVK_BARRIER_INVALIDATE_MME_DATA        = 1 << 5,
   NVK_BARRIER_INVALIDATE_QMD_DATA        = 1 << 6,
   NVK_BARRIER_INVALIDATE_RASTER_CACHE    = 1 << 7,
   NVK_BARRIER_HOST_WFI_INVALIDATE_SYSMEM = 1 << 8,
   NVK_BARRIER_HOST_WFI_FLUSH_SYSMEM      = 1 << 9,
};

#ifdef HAVE_SWITCH_PLATFORM
#define NVK_SWITCH_MME_ACCESS_FLAGS \
   (VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | \
    VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT | \
    VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT | \
    VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT)

enum nvk_switch_barrier_resource {
   NVK_SWITCH_BARRIER_MEMORY,
   NVK_SWITCH_BARRIER_BUFFER,
   NVK_SWITCH_BARRIER_IMAGE,
};

/* MEMORY_READ expansion can include buffer-only accesses. Filter accesses
 * incompatible with the resource type or buffer usage.
 */
static VkAccessFlags2
nvk_switch_filter_mme_dst_access(VkPipelineStageFlags2 stages,
                                 VkAccessFlags2 access,
                                 enum nvk_switch_barrier_resource resource,
                                 VkBufferUsageFlags2KHR buffer_usage)
{
   stages = vk_expand_dst_stage_flags2(stages);
   access = vk_filter_dst_access_flags2(stages, access);

   const VkAccessFlags2 requested = access & NVK_SWITCH_MME_ACCESS_FLAGS;
   if (requested == 0)
      return access;

   VkAccessFlags2 allowed = requested;
   switch (resource) {
   case NVK_SWITCH_BARRIER_MEMORY:
      break;

   case NVK_SWITCH_BARRIER_IMAGE:
      allowed = 0;
      break;

   case NVK_SWITCH_BARRIER_BUFFER: {
      VkAccessFlags2 usage_access = 0;

      if (buffer_usage & VK_BUFFER_USAGE_2_INDIRECT_BUFFER_BIT_KHR)
         usage_access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;

      if (buffer_usage &
          VK_BUFFER_USAGE_2_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT)
         usage_access |= VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT;

      if (buffer_usage & VK_BUFFER_USAGE_2_CONDITIONAL_RENDERING_BIT_EXT)
         usage_access |= VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;

      if (buffer_usage &
          (VK_BUFFER_USAGE_2_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
           VK_BUFFER_USAGE_2_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
           VK_BUFFER_USAGE_2_PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT))
         usage_access |= VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT;

      allowed &= usage_access;
      break;
   }
   }

   return (access & ~NVK_SWITCH_MME_ACCESS_FLAGS) | allowed;
}
#endif

static enum nvk_barrier
nvk_barrier_flushes_waits(VkPipelineStageFlags2 stages,
                          VkAccessFlags2 access)
{
   stages = vk_expand_src_stage_flags2(stages);
   access = vk_filter_src_access_flags2(stages, access);

   enum nvk_barrier barriers = 0;

   if (stages &
       vk_expand_pipeline_stage_flags2(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
      barriers |= NVK_BARRIER_WFI;

   if (access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
      barriers |= NVK_BARRIER_FLUSH_SHADER_DATA;

   if ((access & VK_ACCESS_2_TRANSFER_WRITE_BIT) &&
       (stages & VK_PIPELINE_STAGE_2_COPY_BIT))
      barriers |= NVK_BARRIER_FLUSH_SHADER_DATA;

   if (access & VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_EXT)
      barriers |= NVK_BARRIER_FLUSH_SHADER_DATA;

   if (access & VK_ACCESS_2_HOST_WRITE_BIT)
      barriers |= NVK_BARRIER_HOST_WFI_FLUSH_SYSMEM;

   return barriers;
}

static enum nvk_barrier
nvk_barrier_invalidates(VkPipelineStageFlags2 stages,
                        VkAccessFlags2 access)
{
   stages = vk_expand_dst_stage_flags2(stages);
   access = vk_filter_dst_access_flags2(stages, access);

   enum nvk_barrier barriers = 0;

   if (access & (VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                 VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |
                 VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT |
                 VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT))
      barriers |= NVK_BARRIER_INVALIDATE_MME_DATA;

   if (access & VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT)
      barriers |= NVK_BARRIER_INVALIDATE_CONSTANT |
                  NVK_BARRIER_INVALIDATE_QMD_DATA;

   if (access & (VK_ACCESS_2_UNIFORM_READ_BIT |
                 VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT))
      barriers |= NVK_BARRIER_INVALIDATE_SHADER_DATA |
                  NVK_BARRIER_INVALIDATE_CONSTANT;

   if (access & (VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT |
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT))
      barriers |= NVK_BARRIER_INVALIDATE_TEX_DATA;

   if (access & VK_ACCESS_2_SHADER_STORAGE_READ_BIT)
      barriers |= NVK_BARRIER_INVALIDATE_SHADER_DATA;

   if ((access & VK_ACCESS_2_TRANSFER_READ_BIT) &&
       (stages & (VK_PIPELINE_STAGE_2_RESOLVE_BIT |
                  VK_PIPELINE_STAGE_2_BLIT_BIT)))
      barriers |= NVK_BARRIER_INVALIDATE_TEX_DATA;

   if ((access & VK_ACCESS_2_TRANSFER_READ_BIT) &&
       (stages & VK_PIPELINE_STAGE_2_COPY_BIT))
      barriers |= NVK_BARRIER_INVALIDATE_TEX_DATA |
                  NVK_BARRIER_INVALIDATE_SHADER_DATA;

   if (access & VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR)
      barriers |= NVK_BARRIER_INVALIDATE_RASTER_CACHE;

   if (access & (VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT))
      barriers |= NVK_BARRIER_HOST_WFI_FLUSH_SYSMEM;

   return barriers;
}

void
nvk_cmd_flush_wait_dep(struct nvk_cmd_buffer *cmd,
                       const VkDependencyInfo *dep,
                       bool wait)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   VkQueueFlags queue_flags = nvk_cmd_buffer_queue_flags(cmd);
   enum nvkmd_engines engines =
      nvk_queue_engines_from_queue_flags(queue_flags);

   enum nvk_barrier barriers = 0;

   /* For asymmetric, we don't know what the access flags will be yet.
    * Handle this by setting access to everything.
    */
   if (dep->dependencyFlags & VK_DEPENDENCY_ASYMMETRIC_EVENT_BIT_KHR) {
      /* VUID-vkCmdSetEvent2-dependencyFlags-10785, 10786, 10787 */
      assert(dep->memoryBarrierCount == 1 &&
             dep->bufferMemoryBarrierCount == 0 &&
             dep->imageMemoryBarrierCount == 0);

      const VkMemoryBarrier2 *bar = &dep->pMemoryBarriers[0];
      barriers |= nvk_barrier_flushes_waits(bar->srcStageMask,
                                            VK_ACCESS_2_MEMORY_READ_BIT |
                                            VK_ACCESS_2_MEMORY_WRITE_BIT);
   }

   for (uint32_t i = 0; i < dep->memoryBarrierCount; i++) {
      const VkMemoryBarrier2 *bar = &dep->pMemoryBarriers[i];
      barriers |= nvk_barrier_flushes_waits(bar->srcStageMask,
                                            bar->srcAccessMask);
   }

   for (uint32_t i = 0; i < dep->bufferMemoryBarrierCount; i++) {
      const VkBufferMemoryBarrier2 *bar = &dep->pBufferMemoryBarriers[i];
      barriers |= nvk_barrier_flushes_waits(bar->srcStageMask,
                                            bar->srcAccessMask);

      if (bar->srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)
         barriers |= NVK_BARRIER_HOST_WFI_INVALIDATE_SYSMEM;
   }

   for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; i++) {
      const VkImageMemoryBarrier2 *bar = &dep->pImageMemoryBarriers[i];
      barriers |= nvk_barrier_flushes_waits(bar->srcStageMask,
                                            bar->srcAccessMask);

      if (bar->srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)
         barriers |= NVK_BARRIER_HOST_WFI_INVALIDATE_SYSMEM;
   }

   if (!(engines & (NVKMD_ENGINE_3D | NVKMD_ENGINE_COMPUTE)))
      barriers &= ~NVK_BARRIER_FLUSH_SHADER_DATA;

   if (!barriers)
      return;

   if (barriers & NVK_BARRIER_FLUSH_SHADER_DATA) {
      struct nv_push *p = nvk_cmd_buffer_push(cmd, 2);

      /* This is also implicitly a WFI */
      if (nvk_cmd_buffer_last_subchannel(cmd) == SUBC_NVA097) {
         P_IMMD(p, NVA097, INVALIDATE_SHADER_CACHES, {
            .data = DATA_TRUE,
            .flush_data = FLUSH_DATA_TRUE,
         });
      } else {
         P_IMMD(p, NVA0C0, INVALIDATE_SHADER_CACHES, {
            .data = DATA_TRUE,
            .flush_data = FLUSH_DATA_TRUE,
         });
      }
   } else if ((barriers & NVK_BARRIER_WFI) && wait) {
      /* If this comes from a vkCmdSetEvent, we don't need to wait
       *
       * We only need to WFI on a single channel. The others will implicitly get
       * a WFI from the channel switch.
       */
      switch (nvk_cmd_buffer_last_subchannel(cmd)) {
      case SUBC_NV9097: {
         struct nv_push *p = nvk_cmd_buffer_push(cmd, 2);
         P_IMMD(p, NV9097, WAIT_FOR_IDLE, 0);
         break;
      }
      case SUBC_NV90C0: {
         struct nv_push *p = nvk_cmd_buffer_push(cmd, 2);
         P_IMMD(p, NVA0C0, WAIT_FOR_IDLE, 0);
         break;
      }
      default:
         assert(!"Unknown subc");
         FALLTHROUGH;
      case SUBC_NV90B5: {
         struct nv_push *p = nvk_cmd_buffer_push(cmd, 5);
         P_MTHD(p, NV90B5, LINE_LENGTH_IN);
         P_NV90B5_LINE_LENGTH_IN(p, 0);
         P_NV90B5_LINE_COUNT(p, 0);

         P_IMMD(p, NV90B5, LAUNCH_DMA, {
            .data_transfer_type = DATA_TRANSFER_TYPE_NON_PIPELINED,
            .multi_line_enable = false,
            .flush_enable = FLUSH_ENABLE_TRUE,
            /* Note: FLUSH_TYPE=SYS implicitly for NVC3B5+ */
            .src_memory_layout = SRC_MEMORY_LAYOUT_PITCH,
            .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
            .remap_enable = REMAP_ENABLE_TRUE,
         });
         break;
      }
      }
   }

   if (barriers & NVK_BARRIER_HOST_WFI_INVALIDATE_SYSMEM) {
      struct nv_push *p = nvk_cmd_buffer_push(cmd, 8);
      uint32_t last_subchannel = nvk_cmd_buffer_last_subchannel(cmd);

      if (pdev->info.cls_eng3d >= HOPPER_A) {
         __push_immd(p, last_subchannel, NVC86F_WFI, 0);
         __push_mthd(p, last_subchannel, NVC86F_MEM_OP_A);
         P_NVC86F_MEM_OP_A(p, {});
         P_NVC86F_MEM_OP_B(p, 0);
         P_NVC86F_MEM_OP_C(p, { .membar_type = 0 });
         P_NVC86F_MEM_OP_D(p, { .operation = OPERATION_MEMBAR });
      } else {
         __push_immd(p, last_subchannel, NV906F_SET_REFERENCE, 0);
      }

      /* MEM_OP_D path is really usable starting with Maxwell B */
      if (pdev->info.cls_eng3d >= MAXWELL_B) {
         __push_mthd(p, last_subchannel, NVC86F_MEM_OP_D);
         P_NVC86F_MEM_OP_D(p, {.operation = OPERATION_L2_SYSMEM_INVALIDATE});
      } else {
         __push_mthd(p, last_subchannel, NV906F_MEM_OP_B);
         P_NV906F_MEM_OP_B(p, {.operation = OPERATION_L2_SYSMEM_INVALIDATE});
      }
   }
}

void
nvk_cmd_invalidate_deps(struct nvk_cmd_buffer *cmd,
                        uint32_t dep_count,
                        const VkDependencyInfo *deps)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   enum nvk_barrier barriers = 0;

   for (uint32_t d = 0; d < dep_count; d++) {
      const VkDependencyInfo *dep = &deps[d];

      for (uint32_t i = 0; i < dep->memoryBarrierCount; i++) {
         const VkMemoryBarrier2 *bar = &dep->pMemoryBarriers[i];
         VkAccessFlags2 dst_access = bar->dstAccessMask;
#ifdef HAVE_SWITCH_PLATFORM
         dst_access = nvk_switch_filter_mme_dst_access(
            bar->dstStageMask, dst_access,
            NVK_SWITCH_BARRIER_MEMORY, 0);
#endif
         barriers |= nvk_barrier_invalidates(bar->dstStageMask,
                                             dst_access);
      }

      for (uint32_t i = 0; i < dep->bufferMemoryBarrierCount; i++) {
         const VkBufferMemoryBarrier2 *bar = &dep->pBufferMemoryBarriers[i];
         VkAccessFlags2 dst_access = bar->dstAccessMask;
#ifdef HAVE_SWITCH_PLATFORM
         VK_FROM_HANDLE(nvk_buffer, buffer, bar->buffer);
         dst_access = nvk_switch_filter_mme_dst_access(
            bar->dstStageMask, dst_access,
            NVK_SWITCH_BARRIER_BUFFER, buffer->vk.usage);
#endif
         barriers |= nvk_barrier_invalidates(bar->dstStageMask,
                                             dst_access);

         if (bar->dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)
            barriers |= NVK_BARRIER_HOST_WFI_FLUSH_SYSMEM;
      }

      for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; i++) {
         const VkImageMemoryBarrier2 *bar = &dep->pImageMemoryBarriers[i];
         VkAccessFlags2 dst_access = bar->dstAccessMask;
#ifdef HAVE_SWITCH_PLATFORM
         dst_access = nvk_switch_filter_mme_dst_access(
            bar->dstStageMask, dst_access,
            NVK_SWITCH_BARRIER_IMAGE, 0);
#endif
         barriers |= nvk_barrier_invalidates(bar->dstStageMask,
                                             dst_access);

         if (bar->dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT)
            barriers |= NVK_BARRIER_HOST_WFI_FLUSH_SYSMEM;

      }
   }

   VkQueueFlags queue_flags = nvk_cmd_buffer_queue_flags(cmd);
   enum nvkmd_engines engines =
      nvk_queue_engines_from_queue_flags(queue_flags);

   if (!(engines & (NVKMD_ENGINE_3D | NVKMD_ENGINE_COMPUTE)))
      barriers &= ~(NVK_BARRIER_INVALIDATE_TEX_DATA |
                    NVK_BARRIER_INVALIDATE_RASTER_CACHE |
                    NVK_BARRIER_INVALIDATE_SHADER_DATA |
                    NVK_BARRIER_INVALIDATE_CONSTANT |
                    NVK_BARRIER_INVALIDATE_MME_DATA);

   if (!(engines & NVKMD_ENGINE_COMPUTE))
      barriers &= ~NVK_BARRIER_INVALIDATE_QMD_DATA;

#ifdef HAVE_SWITCH_PLATFORM
   if ((barriers & NVK_BARRIER_INVALIDATE_MME_DATA) &&
       pdev->info.cls_eng3d < HOPPER_A) {
      nvk_cmd_buffer_switch_defer_mme_sync(cmd);
      barriers &= ~NVK_BARRIER_INVALIDATE_MME_DATA;
   }
#endif

   if (!barriers)
      return;

   struct nv_push *p = nvk_cmd_buffer_push(cmd, 32);

   if (barriers & NVK_BARRIER_INVALIDATE_TEX_DATA) {
      if (pdev->info.cls_eng3d >= MAXWELL_A) {
         if (nvk_cmd_buffer_last_subchannel(cmd) == SUBC_NVA097) {
            P_IMMD(p, NVA097, INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, {
               .lines = LINES_ALL,
            });
         } else {
            P_IMMD(p, NVA0C0, INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, {
               .lines = LINES_ALL,
            });
         }
      } else {
         /* On Kepler, the _NO_WFI form doesn't appear to actually work
          * properly.  It exists in the headers but it doesn't fully
          * invalidate everything.  Even doing a full WFI before hand isn't
          * sufficient.
          */
         if (nvk_cmd_buffer_last_subchannel(cmd) == SUBC_NVA097) {
            P_IMMD(p, NVA097, INVALIDATE_TEXTURE_DATA_CACHE, {
               .lines = LINES_ALL,
            });
         } else {
            P_IMMD(p, NVA0C0, INVALIDATE_TEXTURE_DATA_CACHE, {
               .lines = LINES_ALL,
            });
         }
      }
   }

   if (barriers & NVK_BARRIER_INVALIDATE_RASTER_CACHE &&
       dev->vk.enabled_features.pipelineFragmentShadingRate)
      P_IMMD(p, NVC597, INVALIDATE_RASTER_CACHE_NO_WFI, 0);

   if (barriers & (NVK_BARRIER_INVALIDATE_SHADER_DATA |
                   NVK_BARRIER_INVALIDATE_CONSTANT)) {
      if (nvk_cmd_buffer_last_subchannel(cmd) == SUBC_NVA097) {
         P_IMMD(p, NVA097, INVALIDATE_SHADER_CACHES_NO_WFI, {
            .global_data = (barriers & NVK_BARRIER_INVALIDATE_SHADER_DATA) != 0,
            .constant = (barriers & NVK_BARRIER_INVALIDATE_CONSTANT) != 0,
         });
      } else {
         P_IMMD(p, NVA0C0, INVALIDATE_SHADER_CACHES_NO_WFI, {
            .global_data = (barriers & NVK_BARRIER_INVALIDATE_SHADER_DATA) != 0,
            .constant = (barriers & NVK_BARRIER_INVALIDATE_CONSTANT) != 0,
         });
      }
   }

   if (barriers & NVK_BARRIER_HOST_WFI_FLUSH_SYSMEM) {
      uint32_t last_subchannel = nvk_cmd_buffer_last_subchannel(cmd);
      if (pdev->info.cls_eng3d >= HOPPER_A) {
         __push_immd(p, last_subchannel, NVC86F_WFI, 0);
         __push_mthd(p, last_subchannel, NVC86F_MEM_OP_A);
         P_NVC86F_MEM_OP_A(p, {});
         P_NVC86F_MEM_OP_B(p, 0);
         P_NVC86F_MEM_OP_C(p, { .membar_type = 0 });
         P_NVC86F_MEM_OP_D(p, { .operation = OPERATION_MEMBAR });
      } else {
         __push_immd(p, last_subchannel, NV906F_SET_REFERENCE, 0);
      }
   }

   if (barriers & (NVK_BARRIER_INVALIDATE_MME_DATA)) {
      if (pdev->info.cls_eng3d >= HOPPER_A) {
         /* take from the open kernel watchdog handling, might be overkill */
         P_IMMD(p, NVC86F, WFI, 0);
         P_MTHD(p, NVC86F, MEM_OP_A);
         P_NVC86F_MEM_OP_A(p, {});
         P_NVC86F_MEM_OP_B(p, 0);
         P_NVC86F_MEM_OP_C(p, { .membar_type = 0 });
         P_NVC86F_MEM_OP_D(p, { .operation = OPERATION_MEMBAR });

      } else {
         __push_immd(p, SUBC_NV9097, NV906F_SET_REFERENCE, 0);

         if (pdev->info.cls_eng3d >= TURING_A)
            P_IMMD(p, NVC597, MME_DMA_SYSMEMBAR, 0);
      }
   }

   if ((barriers & NVK_BARRIER_INVALIDATE_QMD_DATA) &&
       pdev->info.cls_compute >= MAXWELL_COMPUTE_B)
      P_IMMD(p, NVB1C0, INVALIDATE_SKED_CACHES, 0);
}

static void
nvk_cmd_image_layout_transition(struct nvk_cmd_buffer *cmd,
                                const VkDependencyInfo *dep)
{
   for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; i++) {
      const VkImageMemoryBarrier2 *bar = &dep->pImageMemoryBarriers[i];
      if (bar->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
          bar->newLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
         VK_FROM_HANDLE(nvk_image, image, bar->image);
         /*
          * zcull hardware kills the context if we try to LOAD_ZCULL on garbage
          * data. Handle this by initializing the zcull data to zero.
          */
         if (image->zcull.nil.size_B > 0)
            nvk_cmd_fill_memory_ce(cmd, image->zcull.addr, image->zcull.nil.size_B, 0);
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                        const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   nvk_cmd_flush_wait_dep(cmd, pDependencyInfo, true);
   nvk_cmd_image_layout_transition(cmd, pDependencyInfo);
   nvk_cmd_invalidate_deps(cmd, 1, pDependencyInfo);
}

void
nvk_cmd_bind_shaders(struct vk_command_buffer *vk_cmd,
                     uint32_t stage_count,
                     const mesa_shader_stage *stages,
                     struct vk_shader ** const shaders)
{
   struct nvk_cmd_buffer *cmd = container_of(vk_cmd, struct nvk_cmd_buffer, vk);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);

#ifdef HAVE_SWITCH_PLATFORM
   /* NFSMW: vkCmdBindPipeline measurement, 1 of every N. It is decided
    * here, the first thing binding a graphics pipeline does, and
    * nvk_sw_cmd_set_dynamic_graphics_state, which runs right after, uses it too.
    */
   const bool graficos_sw = stage_count > 0 &&
                            stages[0] != MESA_SHADER_COMPUTE &&
                            stages[0] != MESA_SHADER_KERNEL;
   cmd->state.sw.midiendo_pipe = graficos_sw && nvk_sw_toca_medir(cmd, NVK_SW_T_PIPE);
   const uint64_t t_sw = cmd->state.sw.midiendo_pipe ? nvk_sw_ticks() : 0;
#endif

   for (uint32_t i = 0; i < stage_count; i++) {
      struct nvk_shader *shader =
         container_of(shaders[i], struct nvk_shader, vk);

      if (shader != NULL) {
         nvk_device_ensure_slm(dev, shader->info.slm_size,
                                    shader->info.crs_size);
      }

      if (stages[i] == MESA_SHADER_COMPUTE ||
          stages[i] == MESA_SHADER_KERNEL)
         nvk_cmd_bind_compute_shader(cmd, shader);
      else
         nvk_cmd_bind_graphics_shader(cmd, stages[i], shader);
   }
#ifdef HAVE_SWITCH_PLATFORM
   nvk_sw_parte(cmd, NVK_SW_P_PIPE_SHADERS, t_sw); /* NFSMW */
#endif
}

#define NVK_VK_GRAPHICS_STAGE_BITS (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)

#ifdef HAVE_SWITCH_PLATFORM
/* NFSMW: NVK_SW_M_CBUFS. The cbuf groups of a set of stages (the
 * usual loop), cached with their key: the stages and whether there is a task shader,
 * which is all they depend on. When checking, it is recomputed and compared.
 */
static inline uint32_t
nvk_sw_grupos_de_etapas(struct nvk_cmd_buffer *cmd, VkShaderStageFlags stages,
                        bool usar_guardados, bool comprobar)
{
   const struct nvk_shader *mesh_shader =
      cmd->state.gfx.shaders[MESA_SHADER_MESH];
   const bool has_task_shader =
      mesh_shader != NULL && mesh_shader->info.mesh.has_task_shader;
   const bool guardados = usar_guardados &&
                          cmd->state.gfx.sw_grupos_etapas == stages &&
                          cmd->state.gfx.sw_grupos_tarea == has_task_shader;
   if (guardados && !comprobar)
      return cmd->state.gfx.sw_grupos;

   uint32_t groups = 0;
   u_foreach_bit(i, stages & NVK_VK_GRAPHICS_STAGE_BITS) {
      mesa_shader_stage stage = vk_to_mesa_shader_stage(1 << i);
      uint32_t g = nvk_cbuf_binding_for_stage(stage, has_task_shader);
      groups |= BITFIELD_BIT(g);
   }

   if (guardados) {
      if (groups == cmd->state.gfx.sw_grupos)
         cmd->state.sw.validadas[NVK_SW_M_CBUFS]++;
      else
         nvk_sw_diferencia(cmd, NVK_SW_M_CBUFS, "grupos de cbufs guardados");
   }
   cmd->state.gfx.sw_grupos_etapas = stages;
   cmd->state.gfx.sw_grupos_tarea = has_task_shader;
   cmd->state.gfx.sw_grupos = groups;
   return groups;
}
#endif

void
nvk_cmd_dirty_cbufs_for_descriptors(struct nvk_cmd_buffer *cmd,
                                    VkShaderStageFlags stages,
                                    uint32_t sets_start, uint32_t sets_end)
{
   if (!(stages & NVK_VK_GRAPHICS_STAGE_BITS))
      return;

#ifdef HAVE_SWITCH_PLATFORM
   /* NFSMW: NVK_SW_M_CBUFS */
   cmd->state.gfx.sw_cbufs_limpios = false;
   const bool sw_cbufs = nvk_sw_activa(cmd, NVK_SW_M_CBUFS);
   bool sw_comprobar = false;
   if (sw_cbufs) {
      cmd->state.sw.usos[NVK_SW_M_CBUFS]++;
      sw_comprobar = nvk_sw_toca_comprobar(cmd, NVK_SW_M_CBUFS);
   }
   const uint32_t groups =
      nvk_sw_grupos_de_etapas(cmd, stages, sw_cbufs, sw_comprobar);
#else
   const struct nvk_shader *mesh_shader =
      cmd->state.gfx.shaders[MESA_SHADER_MESH];
   const bool has_task_shader =
      mesh_shader != NULL && mesh_shader->info.mesh.has_task_shader;

   uint32_t groups = 0;
   u_foreach_bit(i, stages & NVK_VK_GRAPHICS_STAGE_BITS) {
      mesa_shader_stage stage = vk_to_mesa_shader_stage(1 << i);
      uint32_t g = nvk_cbuf_binding_for_stage(stage, has_task_shader);
      groups |= BITFIELD_BIT(g);
   }
#endif

   u_foreach_bit(g, groups) {
      struct nvk_cbuf_group *group = &cmd->state.gfx.cbuf_groups[g];

#ifdef HAVE_SWITCH_PLATFORM
      /* NFSMW: NVK_SW_M_CBUFS. Only the DESC_SET, UBO_DESC and
       * DYNAMIC_UBO slots can get dirty, and those are desc_slots and
       * dyn_ubo_slots (nvk_switch_set4_anotar_tipo keeps them up to date whenever
       * cbufs[] changes): those are walked instead of all 16 with their switch. The
       * guard walks all 16 as usual and compares the resulting dirty mask.
       */
      uint16_t sw_sucio = group->dirty;
      if (sw_cbufs) {
         unsigned casillas = group->desc_slots | group->dyn_ubo_slots;
         while (casillas) {
            const unsigned i = u_bit_scan(&casillas);
            const struct nvk_cbuf *cbuf = &group->cbufs[i];
            if (cbuf->desc_set >= sets_start && cbuf->desc_set < sets_end)
               sw_sucio |= BITFIELD_BIT(i);
         }
         if (!sw_comprobar) {
            group->dirty = sw_sucio;
            continue;
         }
      }
#endif
      for (uint32_t i = 0; i < ARRAY_SIZE(group->cbufs); i++) {
         const struct nvk_cbuf *cbuf = &group->cbufs[i];
         switch (cbuf->type) {
         case NVK_CBUF_TYPE_INVALID:
         case NVK_CBUF_TYPE_ROOT_DESC:
         case NVK_CBUF_TYPE_SHADER_DATA:
            break;

         case NVK_CBUF_TYPE_DESC_SET:
         case NVK_CBUF_TYPE_UBO_DESC:
         case NVK_CBUF_TYPE_DYNAMIC_UBO:
            if (cbuf->desc_set >= sets_start && cbuf->desc_set < sets_end)
               group->dirty |= BITFIELD_BIT(i);
            break;

         default:
            UNREACHABLE("Invalid cbuf type");
         }
      }
#ifdef HAVE_SWITCH_PLATFORM
      if (sw_cbufs && sw_comprobar) {
         if (group->dirty == sw_sucio)
            cmd->state.sw.validadas[NVK_SW_M_CBUFS]++;
         else
            nvk_sw_diferencia(cmd, NVK_SW_M_CBUFS, "casillas de cbufs a ensuciar");
      }
#endif
   }
}

#ifdef HAVE_SWITCH_PLATFORM
#include "util/u_debug.h"

/* NFSMW: set 4 by differences (NVK_SWITCH_DYN_UBO_DELTA,
 * on by default).
 *
 * The game binds set 4 (three dynamic UBOs: VS constants, PS constants and
 * shared ones) in 70-78 % of the draws, almost always because one of the three
 * offsets changed. On Maxwell, without a hardware root table,
 * each bind cost 1.3 us of CPU on the recording thread:
 * - four flush_root, one per word of the dynamic descriptors
 * (dynamic_buffers[k], k = 0..3), even if only the low part of the
 * address (k = 0) changes: the high part, the size and the zero are the same;
 * - a walk over 5 groups x 16 slots to dirty all the cbufs of the
 * set, and on the next draw four rebinds (VS b0 and b2, FS b1 and
 * b2) with their SELECT_CB0, even if only one changed.
 *
 * By differences:
 * - only the words that change or that have not been sent yet in this command
 * buffer (dyn_synced) go to the GPU root table, with one command per
 * array k that covers them: almost always a single command of one to three words
 * (the low part of the address of the offsets that change);
 * - only the DYNAMIC_UBO cbufs whose descriptor changed value get
 * dirtied (all of them, if the start of a set's dynamic range changes); the
 * DESC_SET and UBO_DESC ones of the bound sets, as always; and without
 * walking empty slots (dyn_ubo_slots and desc_slots of each group).
 *
 * WHY IT IS EXACT. The GPU ends up with the same root table: whatever is not sent
 * was already sent in this command buffer with that same value (between two points of
 * a buffer, cb0 is only changed by the buffer itself, and dynamic_buffers is only
 * written by nvk_bind_descriptor_sets). And every cbuf ends up bound to the same thing: a
 * cbuf that is not dirty keeps what was bound in its last flush, which came from
 * root; if since then no bind has changed the value of its descriptor,
 * root still says the same. Dirty bits are not cleared until the next flush,
 * so a change that is later reverted also rebinds. Changing
 * shaders dirties on its own the slots whose map changes
 * (nvk_cmd_flush_gfx_shaders), as always.
 *
 * SELF-CHECKING GUARD (nvk_switch_set4_comprobar, nvk_cmd_draw.c):
 * in the first 200,000 draws of the process and then in 1 of every 4,096 of
 * each command buffer, every DYNAMIC_UBO cbuf about to be used without a rebind
 * is compared with what was really bound in that slot. On a
 * difference: that slot and all DYNAMIC_UBO ones are rebound (the draw comes out
 * right), DIFERENCIA on stderr and off for the rest of the session (apagado =
 * 1, which the app forwards to its log with REXLOG_ERROR).
 *
 * The app (nfsmw_nativo_dibujos.cpp, cvar nfsmw_nativo_set4_diferencias) requests
 * or withdraws it on every submit through pedido, and reads total for its C6 set 4 line.
 * NVK_SWITCH_DYN_UBO_DELTA=false (in nfsmw_mesa_entorno) turns it off from outside.
 */
struct nvk_switch_set4 nvk_switch_set4 = {
   .version = 1,
   .pedido = -1,
   .entorno = -1,
   .apagado = 0,
};

DEBUG_GET_ONCE_BOOL_OPTION(nvk_switch_dyn_ubo_delta, "NVK_SWITCH_DYN_UBO_DELTA", true)

int32_t
nvk_switch_set4_leer_entorno(void)
{
   const int32_t entorno = debug_get_option_nvk_switch_dyn_ubo_delta() ? 1 : 0;
   if (p_atomic_xchg(&nvk_switch_set4.entorno, entorno) < 0) {
      fprintf(stderr, "[nvk] set 4 por diferencias (NFSMW, build 184): %s\n",
              entorno ? "disponible (NVK_SWITCH_DYN_UBO_DELTA)"
                      : "apagado por NVK_SWITCH_DYN_UBO_DELTA");
   }
   return entorno;
}

void
nvk_switch_set4_sumar(struct nvk_cmd_buffer *cmd)
{
   struct nvk_switch_set4_cuentas *c = &cmd->state.set4;
   struct nvk_switch_set4_cuentas *t = &nvk_switch_set4.total;

   if (c->enlaces_diferencia)
      p_atomic_add(&t->enlaces_diferencia, c->enlaces_diferencia);
   if (c->enlaces_completos)
      p_atomic_add(&t->enlaces_completos, c->enlaces_completos);
   if (c->escrituras_raiz)
      p_atomic_add(&t->escrituras_raiz, c->escrituras_raiz);
   if (c->dwords_raiz)
      p_atomic_add(&t->dwords_raiz, c->dwords_raiz);
   if (c->cbufs_sucios)
      p_atomic_add(&t->cbufs_sucios, c->cbufs_sucios);
   if (c->cbufs_ahorrados)
      p_atomic_add(&t->cbufs_ahorrados, c->cbufs_ahorrados);
   if (c->dibujos)
      p_atomic_add(&t->dibujos, c->dibujos);
   if (c->comprobaciones)
      p_atomic_add(&t->comprobaciones, c->comprobaciones);
   if (c->diferencias)
      p_atomic_add(&t->diferencias, c->diferencias);
   memset(c, 0, sizeof(*c));
}

/* Sends to the root table only the words dynamic_buffers[k][d], with d from
 * inicio to fin - 1, that change or have not been sent yet in this command
 * buffer, with one command per array k that covers them. Returns in *cambiados
 * the descriptors whose value changed (so that only their cbufs get dirtied).
 */
static void
nvk_switch_set4_escribir_raiz(struct nvk_cmd_buffer *cmd,
                              struct nvk_descriptor_state *desc,
                              uint32_t dynamic_buffers[4][NVK_MAX_DYNAMIC_BUFFERS],
                              uint32_t inicio, uint32_t fin,
                              uint64_t *cambiados)
{
   const struct nvk_root_descriptor_table *raiz =
      (const struct nvk_root_descriptor_table *)desc->root;
   uint64_t distintos = 0;

   for (uint32_t k = 0; k < 4; k++) {
      uint32_t primero = fin, ultimo = inicio;
      for (uint32_t d = inicio; d < fin; d++) {
         const bool distinto =
            raiz->dynamic_buffers[k][d] != dynamic_buffers[k][d];
         if (distinto)
            distintos |= BITFIELD64_BIT(d);
         if (distinto || !(desc->dyn_synced & BITFIELD64_BIT(d))) {
            if (primero == fin)
               primero = d;
            ultimo = d;
         }
      }
      if (primero < fin) {
         const uint32_t n = ultimo - primero + 1;
         nvk_descriptor_state_set_root_array(cmd, desc, dynamic_buffers[k],
                                             primero, n,
                                             &dynamic_buffers[k][primero]);
         cmd->state.set4.escrituras_raiz++;
         cmd->state.set4.dwords_raiz += n;
      }
   }
   if (fin > inicio) {
      desc->dyn_synced |= BITFIELD64_RANGE(inicio, fin - inicio);
      cmd->state.set4.enlaces_diferencia++;
   }
   *cambiados = distintos;
}

/* The usual path, as is, recording that what was sent is up to date. */
static void
nvk_switch_set4_escribir_raiz_completa(struct nvk_cmd_buffer *cmd,
                                       struct nvk_descriptor_state *desc,
                                       uint32_t dynamic_buffers[4][NVK_MAX_DYNAMIC_BUFFERS],
                                       uint32_t inicio, uint32_t fin)
{
   for (int i = 0; i < 4; i++)
      nvk_descriptor_state_set_root_array(cmd, desc, dynamic_buffers[i],
                                          inicio, fin - inicio,
                                          &dynamic_buffers[i][inicio]);
   if (desc->flush_root != NULL && fin > inicio)
      desc->dyn_synced |= BITFIELD64_RANGE(inicio, fin - inicio);
   if (desc == &cmd->state.gfx.descriptors && fin > inicio)
      cmd->state.set4.enlaces_completos++;
}

/* Like nvk_cmd_dirty_cbufs_for_descriptors, but the DYNAMIC_UBO cbufs are only
 * dirtied if the descriptor they read changed value. The DESC_SET and
 * UBO_DESC ones of the bound sets are always dirtied, as before.
 */
static void
nvk_switch_set4_ensuciar(struct nvk_cmd_buffer *cmd,
                         VkShaderStageFlags stages,
                         uint32_t sets_start, uint32_t sets_end,
                         const uint8_t *inicio_dinamico,
                         uint64_t cambiados)
{
   if (!(stages & NVK_VK_GRAPHICS_STAGE_BITS))
      return;

   /* NFSMW: NVK_SW_M_CBUFS: something gets dirtied, and the cached groups of
    * these stages (nvk_sw_grupos_de_etapas)
    */
   cmd->state.gfx.sw_cbufs_limpios = false;
   const bool sw_cbufs = nvk_sw_activa(cmd, NVK_SW_M_CBUFS);
   bool sw_comprobar = false;
   if (sw_cbufs) {
      cmd->state.sw.usos[NVK_SW_M_CBUFS]++;
      sw_comprobar = nvk_sw_toca_comprobar(cmd, NVK_SW_M_CBUFS);
   }
   const uint32_t groups =
      nvk_sw_grupos_de_etapas(cmd, stages, sw_cbufs, sw_comprobar);

   u_foreach_bit(g, groups) {
      struct nvk_cbuf_group *group = &cmd->state.gfx.cbuf_groups[g];

      unsigned conjuntos = group->desc_slots;
      while (conjuntos) {
         const unsigned i = u_bit_scan(&conjuntos);
         const struct nvk_cbuf *cbuf = &group->cbufs[i];
         if (cbuf->desc_set >= sets_start && cbuf->desc_set < sets_end)
            group->dirty |= BITFIELD_BIT(i);
      }

      unsigned dinamicos = group->dyn_ubo_slots;
      while (dinamicos) {
         const unsigned i = u_bit_scan(&dinamicos);
         const struct nvk_cbuf *cbuf = &group->cbufs[i];
         if (cbuf->desc_set < sets_start || cbuf->desc_set >= sets_end)
            continue;

         const uint32_t d = inicio_dinamico[cbuf->desc_set] + cbuf->dynamic_idx;
         if (d >= NVK_MAX_DYNAMIC_BUFFERS || (cambiados & BITFIELD64_BIT(d))) {
            group->dirty |= BITFIELD_BIT(i);
            cmd->state.set4.cbufs_sucios++;
         } else {
            cmd->state.set4.cbufs_ahorrados++;
         }
      }
   }
}
#endif

#ifdef HAVE_SWITCH_PLATFORM
/* NFSMW: per-part measurement of the draw and exact improvements
 * (nvk_switch_dibujo, NVK_SWITCH_DIBUJO).
 *
 * The thread that records the draws spends about 3.4 us per draw inside NVK in
 * a race ("C6 subetapas" line: Draw 1.4-2.0 us; BindPipeline 3.2-4.4 us
 * in 25-27 % of the draws; set 4 1.2-1.5 us in 64-72 %). That is about
 * 6.5 ms per frame on the game's bottleneck. Before optimizing blindly, it is
 * measured inside NVK:
 *
 * - partes: CNTPCT_EL0 ticks (19.2 MHz: 52 ns per tick; with thousands of
 * samples the average is unbiased) of each piece of vkCmdDraw*,
 * vkCmdBindPipeline and vkCmdBindDescriptorSets, 1 of every N calls (the app
 * sets N in medir; 0 = not measured and the cost is one branch per call);
 * - cuentas: dirty dynamic bits per draw, dirty shaders, rebound
 * cbufs, dwords per draw, pipeline state copies that changed
 * nothing, root table uploads and new pushbuf chunks;
 * - medir_fallos: in the measured ones, before the pipeline copy and the shader
 * flush all their data is read and that time is counted apart (the TOQUE parts).
 *
 * The improvements p02..p06 also use this structure: each has its pedido
 * (the app), its apagado (its guard) and its counts. They are all exact: they leave
 * the same commands with the same data in the pushbuf. Each has a
 * self-checking guard (the first NVK_SW_A_COMPROBAR uses of the
 * process and then 1 of every NVK_SW_CADA): it takes the usual path,
 * compares it with the new one and, if they differ, keeps the usual one, reports it on
 * stderr and turns that improvement off for the rest of the session.
 */
struct nvk_switch_dibujo nvk_switch_dibujo = {
   .version = 1,
   .medir = 0,
   .medir_fallos = 0,
   .entorno = -1,
   .mejoras = {
      [NVK_SW_M_EMISION] = { .pedido = -1 },
      [NVK_SW_M_CBUFS] = { .pedido = -1 },
      [NVK_SW_M_DINAMICO] = { .pedido = -1 },
      [NVK_SW_M_SET4_RAPIDO] = { .pedido = -1 },
      [NVK_SW_M_PRECARGA] = { .pedido = -1 },
   },
};

DEBUG_GET_ONCE_BOOL_OPTION(nvk_switch_dibujo, "NVK_SWITCH_DIBUJO", true)

static int32_t
nvk_sw_leer_entorno(void)
{
   const int32_t entorno = debug_get_option_nvk_switch_dibujo() ? 1 : 0;
   uint64_t frecuencia = 0;
#if defined(__aarch64__)
   __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frecuencia));
#endif
   p_atomic_set(&nvk_switch_dibujo.ticks_por_segundo, frecuencia);
   if (p_atomic_xchg(&nvk_switch_dibujo.entorno, entorno) < 0) {
      fprintf(stderr, "[nvk] c186 medida por partes y mejoras del dibujo: %s "
              "(reloj %llu Hz)\n",
              entorno ? "disponibles (NVK_SWITCH_DIBUJO)"
                      : "apagadas por NVK_SWITCH_DIBUJO",
              (unsigned long long)frecuencia);
   }
   return entorno;
}

/* In vkBeginCommandBuffer, after the reset memset: what is measured and which
 * improvements take the new path in this buffer. Whatever the app changes afterwards
 * applies to the next buffer.
 */
void
nvk_sw_empezar_bufer(struct nvk_cmd_buffer *cmd)
{
   struct nvk_sw_cmd *sw = &cmd->state.sw;
   int32_t entorno = p_atomic_read_relaxed(&nvk_switch_dibujo.entorno);
   if (unlikely(entorno < 0))
      entorno = nvk_sw_leer_entorno();

   const int32_t medir = p_atomic_read_relaxed(&nvk_switch_dibujo.medir);
   sw->mascara_medir =
      (entorno > 0 && medir > 0 && util_is_power_of_two_nonzero((unsigned)medir))
         ? (uint32_t)medir - 1 : UINT32_MAX;
   sw->medir_fallos = sw->mascara_medir != UINT32_MAX &&
                      p_atomic_read_relaxed(&nvk_switch_dibujo.medir_fallos) > 0;
   sw->activas = 0;
   if (entorno > 0) {
      for (uint32_t m = 0; m < NVK_SW_MEJORAS; m++) {
         const struct nvk_sw_mejora_estado *e = &nvk_switch_dibujo.mejoras[m];
         /* Only for an app that asks for it (nfsmw-nx writes pedido = 1 on
          * every submit): any other app gets the usual path.
          */
         if (p_atomic_read_relaxed(&e->pedido) > 0 &&
             p_atomic_read_relaxed(&e->apagado) == 0)
            sw->activas |= 1u << m;
      }
   }
   /* p06: the runtime's prefetch when binding pipelines goes with NVK's */
   p_atomic_set(&vk_switch_precarga,
                (int32_t)((sw->activas >> NVK_SW_M_PRECARGA) & 1));
}

static void
nvk_sw_sumar_u64(uint64_t *total, uint64_t *local)
{
   if (*local != 0) {
      p_atomic_add(total, *local);
      *local = 0;
   }
}

void
nvk_sw_sumar(struct nvk_cmd_buffer *cmd)
{
   struct nvk_sw_cmd *sw = &cmd->state.sw;
   for (uint32_t i = 0; i < NVK_SW_PARTES; i++) {
      nvk_sw_sumar_u64(&nvk_switch_dibujo.partes[i].veces, &sw->partes[i].veces);
      nvk_sw_sumar_u64(&nvk_switch_dibujo.partes[i].ticks, &sw->partes[i].ticks);
   }
   for (uint32_t i = 0; i < NVK_SW_CUENTAS; i++)
      nvk_sw_sumar_u64(&nvk_switch_dibujo.cuentas[i], &sw->cuentas[i]);
   for (uint32_t m = 0; m < NVK_SW_MEJORAS; m++) {
      struct nvk_sw_mejora_estado *e = &nvk_switch_dibujo.mejoras[m];
      nvk_sw_sumar_u64(&e->usos, &sw->usos[m]);
      nvk_sw_sumar_u64(&e->validadas, &sw->validadas[m]);
      nvk_sw_sumar_u64(&e->diferencias, &sw->diferencias[m]);
      nvk_sw_sumar_u64(&e->sin_comprobar, &sw->sin_comprobar[m]);
   }
}

/* The guard of improvement m saw a difference. The caller has already kept
 * the result of the usual path. It is turned off for the session (the
 * following buffers do not enable it; this one stops using it right away).
 */
void
nvk_sw_diferencia(struct nvk_cmd_buffer *cmd, enum nvk_sw_mejora m,
                  const char *que)
{
   cmd->state.sw.diferencias[m]++;
   cmd->state.sw.activas &= ~(1u << m);
   if (p_atomic_xchg(&nvk_switch_dibujo.mejoras[m].apagado, 1) == 0) {
      fprintf(stderr, "[nvk] c186: DIFERENCIA en la mejora %u (%s): este caso "
              "sale por el camino de siempre y la mejora se apaga para el resto "
              "de la sesion\n", (unsigned)m, que);
   }
}
#endif

static void
nvk_bind_descriptor_sets(struct nvk_cmd_buffer *cmd,
                         struct nvk_descriptor_state *desc,
                         const VkBindDescriptorSetsInfoKHR *info)
{
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, info->layout);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   uint32_t dynamic_buffers[4][NVK_MAX_DYNAMIC_BUFFERS];
   uint8_t set_dynamic_buffer_start[NVK_MAX_SETS];

   /* Read off the current dynamic buffer start array so we can use it to
    * determine where we should start binding dynamic buffers.
    */
   nvk_descriptor_state_get_root_array(desc, set_dynamic_buffer_start,
                                       0, NVK_MAX_SETS,
                                       set_dynamic_buffer_start);

   /* From the Vulkan 1.3.275 spec:
    *
    *    "When binding a descriptor set (see Descriptor Set Binding) to
    *    set number N...
    *
    *    If, additionally, the previously bound descriptor set for set
    *    N was bound using a pipeline layout not compatible for set N,
    *    then all bindings in sets numbered greater than N are
    *    disturbed."
    *
    * This means that, if some earlier set gets bound in such a way that
    * it changes set_dynamic_buffer_start[s], this binding is implicitly
    * invalidated.
    */
   const uint8_t dyn_buffer_start =
      pipeline_layout->dynamic_descriptor_offset[info->firstSet];
   uint8_t dyn_buffer_end = dyn_buffer_start;

#ifdef HAVE_SWITCH_PLATFORM
   bool inicio_dinamico_cambiado = false; /* NFSMW */
#endif
   uint32_t next_dyn_offset = 0;
   for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
      unsigned s = i + info->firstSet;
      VK_FROM_HANDLE(nvk_descriptor_set, set, info->pDescriptorSets[i]);

      if (desc->sets[s].type != NVK_DESCRIPTOR_SET_TYPE_SET ||
          desc->sets[s].set != set) {
         struct nvk_buffer_address set_addr;
         if (set != NULL) {
            desc->sets[s].type = NVK_DESCRIPTOR_SET_TYPE_SET;
            desc->sets[s].set = set;
            set_addr = nvk_descriptor_set_addr(set);
         } else {
            desc->sets[s].type = NVK_DESCRIPTOR_SET_TYPE_NONE;
            desc->sets[s].set = NULL;
            set_addr = NVK_BUFFER_ADDRESS_NULL;
         }
         nvk_descriptor_state_set_root(cmd, desc, sets[s], set_addr);
      }

#ifdef HAVE_SWITCH_PLATFORM
      /* NFSMW: if the start of a set's dynamic range changes, its
       * DYNAMIC_UBO cbufs read another descriptor: they are all
       * dirtied, as always.
       */
      if (set_dynamic_buffer_start[s] != dyn_buffer_end)
         inicio_dinamico_cambiado = true;
#endif
      set_dynamic_buffer_start[s] = dyn_buffer_end;

      if (pipeline_layout->set_layouts[s] != NULL) {
         const struct nvk_descriptor_set_layout *set_layout =
            vk_to_nvk_descriptor_set_layout(pipeline_layout->set_layouts[s]);

         if (set != NULL && set_layout->vk.dynamic_descriptor_count > 0) {
            for (uint32_t j = 0; j < set_layout->vk.dynamic_descriptor_count; j++) {
               union nvk_buffer_descriptor db = set->dynamic_buffers[j];
               uint32_t offset = info->pDynamicOffsets[next_dyn_offset + j];
               if (BITSET_TEST(set_layout->dynamic_ubos, j) &&
                   nvk_use_bindless_cbuf(&pdev->info)) {
                  assert((offset & 0xf) == 0);
                  if (nvk_use_bindless_cbuf_2(&pdev->info)) {
                     db.cbuf2.base_addr_shift_6 += offset >> 6;
                  } else {
                     db.cbuf.base_addr_shift_4 += offset >> 4;
                  }
               } else {
                  db.addr.base_addr += offset;
               }
               for (int k = 0; k < 4; k++)
                  dynamic_buffers[k][dyn_buffer_end + j] = db.values[k];
            }
            next_dyn_offset += set->layout->vk.dynamic_descriptor_count;
         }

         dyn_buffer_end += set_layout->vk.dynamic_descriptor_count;
      } else {
         assert(set == NULL);
      }
   }
   assert(dyn_buffer_end <= NVK_MAX_DYNAMIC_BUFFERS);
   assert(next_dyn_offset <= info->dynamicOffsetCount);

#ifdef HAVE_SWITCH_PLATFORM
   /* NFSMW: measurement of the root table part */
   uint64_t t_sw = unlikely(cmd->state.sw.midiendo_sets) ? nvk_sw_ticks() : 0;
   /* NFSMW: set 4 by differences (see nvk_switch_set4). */
   const bool por_diferencias = desc == &cmd->state.gfx.descriptors &&
                                desc->flush_root != NULL &&
                                !nvk_use_hw_root_table(&pdev->info, true) &&
                                nvk_switch_set4_activo();
   uint64_t dinamicos_cambiados = 0;
   if (por_diferencias) {
      nvk_switch_set4_escribir_raiz(cmd, desc, dynamic_buffers,
                                    dyn_buffer_start, dyn_buffer_end,
                                    &dinamicos_cambiados);
   } else {
      nvk_switch_set4_escribir_raiz_completa(cmd, desc, dynamic_buffers,
                                             dyn_buffer_start, dyn_buffer_end);
   }
#else
   for (int i = 0; i < 4; i++)
      nvk_descriptor_state_set_root_array(cmd, desc, dynamic_buffers[i],
                                          dyn_buffer_start, dyn_buffer_end - dyn_buffer_start,
                                          &dynamic_buffers[i][dyn_buffer_start]);
#endif

   /* We need to at least sync everything from first_set to NVK_MAX_SETS.
    * However, we only save anything if firstSet >= 4 so we may as well sync
    * everything just to be safe.
    */
   /* NFSMW: without a hardware root table (Maxwell) each upload of this
    * is 10 dwords in the pushbuf, and the game binds set 4 in 70 %
    * of the draws without anything changing here. It is skipped only if it was already
    * sent in this same command buffer (the CPU copy matches the GPU's).
    */
   {
      const struct nvk_root_descriptor_table *root_actual =
         (const struct nvk_root_descriptor_table *)desc->root;
      if (desc->flush_root == NULL || !desc->dyn_start_synced ||
          memcmp(root_actual->set_dynamic_buffer_start, set_dynamic_buffer_start,
                 sizeof(set_dynamic_buffer_start)) != 0) {
         nvk_descriptor_state_set_root_array(cmd, desc, set_dynamic_buffer_start,
                                             0, NVK_MAX_SETS,
                                             set_dynamic_buffer_start);
         desc->dyn_start_synced = desc->flush_root != NULL;
      }
   }

#ifdef HAVE_SWITCH_PLATFORM
   t_sw = nvk_sw_parte(cmd, NVK_SW_P_SETS_RAIZ, t_sw); /* NFSMW */
   if (por_diferencias && !inicio_dinamico_cambiado) {
      nvk_switch_set4_ensuciar(cmd, info->stageFlags, info->firstSet,
                               info->firstSet + info->descriptorSetCount,
                               set_dynamic_buffer_start, dinamicos_cambiados);
      nvk_sw_parte(cmd, NVK_SW_P_SETS_ENSUCIAR, t_sw); /* NFSMW */
      return;
   }
#endif
   nvk_cmd_dirty_cbufs_for_descriptors(cmd, info->stageFlags, info->firstSet,
                                       info->firstSet + info->descriptorSetCount);
#ifdef HAVE_SWITCH_PLATFORM
   nvk_sw_parte(cmd, NVK_SW_P_SETS_ENSUCIAR, t_sw); /* NFSMW */
#endif
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindDescriptorSets2KHR(VkCommandBuffer commandBuffer,
                              const VkBindDescriptorSetsInfoKHR *pBindDescriptorSetsInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (pBindDescriptorSetsInfo->stageFlags & NVK_VK_GRAPHICS_STAGE_BITS) {
#ifdef HAVE_SWITCH_PLATFORM
      /* NFSMW: per-part measurement, 1 of every N */
      if (unlikely(nvk_sw_toca_medir(cmd, NVK_SW_T_SETS))) {
         cmd->state.sw.midiendo_sets = true;
         const uint64_t t_sw = nvk_sw_ticks();
         nvk_bind_descriptor_sets(cmd, &cmd->state.gfx.descriptors,
                                  pBindDescriptorSetsInfo);
         nvk_sw_parte(cmd, NVK_SW_P_SETS, t_sw);
         cmd->state.sw.midiendo_sets = false;
         cmd->state.sw.cuentas[NVK_SW_C_SETS_MEDIDOS]++;
      } else
#endif
      nvk_bind_descriptor_sets(cmd, &cmd->state.gfx.descriptors,
                               pBindDescriptorSetsInfo);
   }

   if (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      nvk_bind_descriptor_sets(cmd, &cmd->state.cs.descriptors,
                               pBindDescriptorSetsInfo);
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindDescriptorBuffersEXT(VkCommandBuffer commandBuffer,
                                uint32_t bufferCount,
                                const VkDescriptorBufferBindingInfoEXT *pBindingInfos)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   for (uint32_t i = 0; i < bufferCount; i++)
      cmd->state.descriptor_buffers[i] = pBindingInfos[i].address;
}

static void
nvk_set_descriptor_buffer_offsets(struct nvk_cmd_buffer *cmd,
                                  struct nvk_descriptor_state *desc,
                                  const VkSetDescriptorBufferOffsetsInfoEXT *info)
{
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, info->layout);

   for (uint32_t i = 0; i < info->setCount; ++i) {
      const uint32_t s = i + info->firstSet;

      desc->sets[s].type = NVK_DESCRIPTOR_SET_TYPE_BUFFER;
      desc->sets[s].set = NULL;

      struct nvk_buffer_address set_addr;
      if (pipeline_layout->set_layouts[s] != NULL) {
         const struct nvk_descriptor_set_layout *set_layout =
            vk_to_nvk_descriptor_set_layout(pipeline_layout->set_layouts[s]);
         assert(set_layout->flags &
                VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT);

         const uint64_t buffer_base_addr =
            cmd->state.descriptor_buffers[info->pBufferIndices[i]];

         set_addr = (struct nvk_buffer_address) {
            .base_addr = buffer_base_addr + info->pOffsets[i],
            .size = set_layout->max_buffer_size,
         };
      } else {
         set_addr = NVK_BUFFER_ADDRESS_NULL;
      }
      nvk_descriptor_state_set_root(cmd, desc, sets[s], set_addr);
   }

   nvk_cmd_dirty_cbufs_for_descriptors(cmd, info->stageFlags,
                                       info->firstSet,
                                       info->firstSet + info->setCount);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdSetDescriptorBufferOffsets2EXT(VkCommandBuffer commandBuffer,
                                      const VkSetDescriptorBufferOffsetsInfoEXT *pInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (pInfo->stageFlags & NVK_VK_GRAPHICS_STAGE_BITS) {
      nvk_set_descriptor_buffer_offsets(cmd, &cmd->state.gfx.descriptors,
                                        pInfo);
   }

   if (pInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      nvk_set_descriptor_buffer_offsets(cmd, &cmd->state.cs.descriptors,
                                        pInfo);
   }
}

static void
nvk_bind_embedded_samplers(struct nvk_cmd_buffer *cmd,
                           struct nvk_descriptor_state *desc,
                           const VkBindDescriptorBufferEmbeddedSamplersInfoEXT *info)
{
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, info->layout);
   const struct nvk_descriptor_set_layout *set_layout =
      vk_to_nvk_descriptor_set_layout(pipeline_layout->set_layouts[info->set]);

   struct nvk_buffer_address set_addr = {
      .base_addr = set_layout->embedded_samplers_addr,
      .size = set_layout->non_variable_descriptor_buffer_size,
   };
   nvk_descriptor_state_set_root(cmd, desc, sets[info->set], set_addr);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindDescriptorBufferEmbeddedSamplers2EXT(
    VkCommandBuffer commandBuffer,
    const VkBindDescriptorBufferEmbeddedSamplersInfoEXT *pInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (pInfo->stageFlags & NVK_VK_GRAPHICS_STAGE_BITS) {
      nvk_bind_embedded_samplers(cmd, &cmd->state.gfx.descriptors, pInfo);
   }

   if (pInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      nvk_bind_embedded_samplers(cmd, &cmd->state.cs.descriptors, pInfo);
   }
}

static void
nvk_push_constants(UNUSED struct nvk_cmd_buffer *cmd,
                   struct nvk_descriptor_state *desc,
                   const VkPushConstantsInfoKHR *info)
{
   nvk_descriptor_state_set_root_array(cmd, desc, push,
                                       info->offset, info->size,
                                       (char *)info->pValues);
}


VKAPI_ATTR void VKAPI_CALL
nvk_CmdPushConstants2KHR(VkCommandBuffer commandBuffer,
                         const VkPushConstantsInfoKHR *pPushConstantsInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (pPushConstantsInfo->stageFlags & NVK_VK_GRAPHICS_STAGE_BITS)
      nvk_push_constants(cmd, &cmd->state.gfx.descriptors, pPushConstantsInfo);

   if (pPushConstantsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
      nvk_push_constants(cmd, &cmd->state.cs.descriptors, pPushConstantsInfo);
}

static struct nvk_push_descriptor_set *
nvk_cmd_push_descriptors(struct nvk_cmd_buffer *cmd,
                         struct nvk_descriptor_state *desc,
                         uint32_t set)
{
   assert(set < NVK_MAX_SETS);
   if (unlikely(desc->sets[set].push == NULL)) {
      desc->sets[set].push = vk_zalloc(&cmd->vk.pool->alloc,
                                       sizeof(*desc->sets[set].push), 8,
                                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (unlikely(desc->sets[set].push == NULL)) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
   }

   /* Pushing descriptors replaces whatever sets are bound */
   desc->sets[set].type = NVK_DESCRIPTOR_SET_TYPE_PUSH;
   desc->sets[set].set = NULL;
   desc->push_dirty |= BITFIELD_BIT(set);

   return desc->sets[set].push;
}

static void
nvk_push_descriptor_set(struct nvk_cmd_buffer *cmd,
                        struct nvk_descriptor_state *desc,
                        const VkPushDescriptorSetInfoKHR *info)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, info->layout);

   struct nvk_push_descriptor_set *push_set =
      nvk_cmd_push_descriptors(cmd, desc, info->set);
   if (unlikely(push_set == NULL))
      return;

   struct nvk_descriptor_set_layout *set_layout =
      vk_to_nvk_descriptor_set_layout(pipeline_layout->set_layouts[info->set]);

   nvk_push_descriptor_set_update(dev, push_set, set_layout,
                                  info->descriptorWriteCount,
                                  info->pDescriptorWrites);

   nvk_cmd_dirty_cbufs_for_descriptors(cmd, info->stageFlags,
                                       info->set, info->set + 1);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdPushDescriptorSet2KHR(VkCommandBuffer commandBuffer,
                             const VkPushDescriptorSetInfoKHR *pPushDescriptorSetInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (pPushDescriptorSetInfo->stageFlags & NVK_VK_GRAPHICS_STAGE_BITS) {
      nvk_push_descriptor_set(cmd, &cmd->state.gfx.descriptors,
                              pPushDescriptorSetInfo);
   }

   if (pPushDescriptorSetInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      nvk_push_descriptor_set(cmd, &cmd->state.cs.descriptors,
                              pPushDescriptorSetInfo);
   }
}

void
nvk_cmd_buffer_flush_push_descriptors(struct nvk_cmd_buffer *cmd,
                                      struct nvk_descriptor_state *desc)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);
   const uint32_t min_cbuf_alignment = nvk_min_cbuf_alignment(&pdev->info);
   VkResult result;

   u_foreach_bit(set_idx, desc->push_dirty) {
      if (desc->sets[set_idx].type != NVK_DESCRIPTOR_SET_TYPE_PUSH) {
#ifdef HAVE_SWITCH_PLATFORM
         /* NFSMW: NVK_SW_M_CBUFS. A set that is no longer a push descriptor set
          * is not uploaded in any flush until it becomes one again, and then its
          * bit is set again (nvk_cmd_push_descriptors
          * and nvk_meta_end_gfx): it is cleared so that it is not walked on every draw.
          */
         if (nvk_sw_activa(cmd, NVK_SW_M_CBUFS))
            desc->push_dirty &= ~BITFIELD_BIT(set_idx);
#endif
         continue;
      }

      struct nvk_push_descriptor_set *push_set = desc->sets[set_idx].push;
      uint32_t upload_size_B = align(push_set->size_B, min_cbuf_alignment);
      if (upload_size_B == 0)
         upload_size_B = sizeof(push_set->data);
      upload_size_B = MIN2(upload_size_B, sizeof(push_set->data));

      uint64_t push_set_addr;
      result = nvk_cmd_buffer_upload_data(cmd, push_set->data,
                                          upload_size_B,
                                          min_cbuf_alignment,
                                          &push_set_addr);
      if (unlikely(result != VK_SUCCESS)) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return;
      }

      struct nvk_buffer_address set_addr = {
         .base_addr = push_set_addr,
         .size = upload_size_B,
      };
      nvk_descriptor_state_set_root(cmd, desc, sets[set_idx], set_addr);
   }
}

void
nvk_cmd_buffer_flush_printf_buffer(struct nvk_cmd_buffer *cmd,
                                   struct nvk_descriptor_state *desc)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);

   if (!NAK_CAN_PRINTF)
      return;

   struct nvkmd_mem *bo = (struct nvkmd_mem *) dev->printf.bo;
   nvk_descriptor_state_set_root(cmd, desc, printf_buffer_addr,
                                 bo->va->addr);
}

bool
nvk_cmd_buffer_get_cbuf_addr(struct nvk_cmd_buffer *cmd,
                             const struct nvk_descriptor_state *desc,
                             const struct nvk_shader *shader,
                             const struct nvk_cbuf *cbuf,
                             struct nvk_buffer_address *addr_out)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   switch (cbuf->type) {
   case NVK_CBUF_TYPE_INVALID:
      *addr_out = (struct nvk_buffer_address) { .size = 0 };
      return true;

   case NVK_CBUF_TYPE_ROOT_DESC:
      UNREACHABLE("The caller should handle root descriptors");
      return false;

   case NVK_CBUF_TYPE_SHADER_DATA:
      *addr_out = (struct nvk_buffer_address) {
         .base_addr = shader->data_addr,
         .size = shader->data_size,
      };
      return true;

   case NVK_CBUF_TYPE_DESC_SET:
      nvk_descriptor_state_get_root(desc, sets[cbuf->desc_set], addr_out);
      return true;

   case NVK_CBUF_TYPE_DYNAMIC_UBO: {
      uint8_t dyn_idx;
      nvk_descriptor_state_get_root(
         desc, set_dynamic_buffer_start[cbuf->desc_set], &dyn_idx);
      dyn_idx += cbuf->dynamic_idx;
      union nvk_buffer_descriptor ubo_desc;
      for (int i = 0; i < 4; i++)
         nvk_descriptor_state_get_root(desc, dynamic_buffers[i][dyn_idx], &ubo_desc.values[i]);
      *addr_out = nvk_ubo_descriptor_addr(pdev, ubo_desc);
      return true;
   }

   case NVK_CBUF_TYPE_UBO_DESC: {
      if (desc->sets[cbuf->desc_set].type != NVK_DESCRIPTOR_SET_TYPE_PUSH)
         return false;

      struct nvk_push_descriptor_set *push = desc->sets[cbuf->desc_set].push;
      if (push == NULL)
         return false;

      assert(cbuf->desc_offset < NVK_PUSH_DESCRIPTOR_SET_SIZE);
      union nvk_buffer_descriptor desc;
      memcpy(&desc, &push->data[cbuf->desc_offset], sizeof(desc));
      *addr_out = nvk_ubo_descriptor_addr(pdev, desc);
      return true;
   }

   default:
      UNREACHABLE("Invalid cbuf type");
   }
}

uint64_t
nvk_cmd_buffer_get_cbuf_descriptor_addr(struct nvk_cmd_buffer *cmd,
                                        const struct nvk_descriptor_state *desc,
                                        const struct nvk_cbuf *cbuf)
{
   assert(cbuf->type == NVK_CBUF_TYPE_UBO_DESC);
   switch (desc->sets[cbuf->desc_set].type) {
   case NVK_DESCRIPTOR_SET_TYPE_SET:
   case NVK_DESCRIPTOR_SET_TYPE_BUFFER: {
      struct nvk_buffer_address set_addr;
      nvk_descriptor_state_get_root(desc, sets[cbuf->desc_set], &set_addr);

      assert(cbuf->desc_offset < set_addr.size);
      return set_addr.base_addr + cbuf->desc_offset;
   }

   default:
      UNREACHABLE("Unknown descriptor set type");
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdPushDescriptorSetWithTemplate2KHR(
   VkCommandBuffer commandBuffer,
   const VkPushDescriptorSetWithTemplateInfoKHR *pPushDescriptorSetWithTemplateInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   VK_FROM_HANDLE(vk_descriptor_update_template, template,
                  pPushDescriptorSetWithTemplateInfo->descriptorUpdateTemplate);
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout,
                  pPushDescriptorSetWithTemplateInfo->layout);
   const uint32_t set = pPushDescriptorSetWithTemplateInfo->set;

   struct nvk_descriptor_state *desc =
      nvk_get_descriptors_state(cmd, template->bind_point);
   struct nvk_push_descriptor_set *push_set =
      nvk_cmd_push_descriptors(cmd, desc, set);
   if (unlikely(push_set == NULL))
      return;

   struct nvk_descriptor_set_layout *set_layout =
      vk_to_nvk_descriptor_set_layout(pipeline_layout->set_layouts[set]);

   nvk_push_descriptor_set_update_template(dev, push_set, set_layout, template,
                                           pPushDescriptorSetWithTemplateInfo->pData);

   /* We don't know the actual set of stages here so assume everything */
   nvk_cmd_dirty_cbufs_for_descriptors(cmd, NVK_VK_GRAPHICS_STAGE_BITS |
                                            VK_SHADER_STAGE_COMPUTE_BIT,
                                       set, set + 1);
}
