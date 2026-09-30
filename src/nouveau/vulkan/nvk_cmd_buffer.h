/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_CMD_BUFFER_H
#define NVK_CMD_BUFFER_H 1

#include "nvk_private.h"

#include "nv_push.h"
#include "nvk_cmd_pool.h"
#include "nvk_descriptor_set.h"
#include "nvk_image.h"
#include "nvk_shader.h"

#include "util/u_dynarray.h"

#include "vk_command_buffer.h"
#include "clc597.h"

#include <stdio.h>

struct nvk_buffer;
struct nvk_cbuf;
struct nvk_cmd_mem;
struct nvk_cmd_buffer;
struct nvk_cmd_pool;
struct nvk_image_view;
struct nvk_push_descriptor_set;
struct nvk_shader;
struct vk_shader;

/** Root descriptor table.  This gets pushed to the GPU directly */
struct nvk_root_descriptor_table {
   union {
      struct {
         struct {
            uint32_t base_vertex;
            uint32_t base_instance;
         } vs;
         struct {
            uint32_t group_count[3];
         } mesh;
         uint32_t draw_index;
         uint32_t view_index;
         struct nak_sample_location sample_locations[NVK_MAX_SAMPLES];
         struct nak_sample_mask sample_masks[NVK_MAX_SAMPLES];
         uint32_t __padding;
      } draw;
      struct {
         uint32_t base_group[3];
         uint32_t group_count[3];
      } cs;
   };

   /* Descriptor set addresses */
   struct nvk_buffer_address sets[NVK_MAX_SETS];

   /* For each descriptor set, the index in dynamic_buffers where that set's
    * dynamic buffers start.
    */
   uint8_t set_dynamic_buffer_start[NVK_MAX_SETS];

   uint64_t printf_buffer_addr;

   uint64_t interlock_buffer_addr;

   /* Align the following arrays to 0x100 on pre-Pascal GPUs. */
   uint8_t __padding[0x98];

   /*
    * Arrays with dynamic (shader-provided) indices need to fit in a single
    * 256-byte bank for gpus with ROOT_TABLE. We place them here after the
    * padding so they're appropriately aligned.
    */

   /* Dynamic buffer bindings (swizzled form of nvk_buffer_descriptor) */
   uint32_t dynamic_buffers[4][NVK_MAX_DYNAMIC_BUFFERS];

   /* Client push constants */
   uint8_t push[NVK_MAX_PUSH_SIZE];
};

/* helper macro for computing root descriptor byte offsets */
#define nvk_root_descriptor_offset(member)\
   offsetof(struct nvk_root_descriptor_table, member)

/* Push constants should be aligned properly */
static_assert(nvk_root_descriptor_offset(push) % 8 == 0,
              "Push constants should be aligned properly");

#define nvk_hw_root_table_index(member)\
   (nvk_root_descriptor_offset(member) / NVK_HW_ROOT_TABLE_SIZE)
#define nvk_hw_root_table_offset(member)\
   (nvk_root_descriptor_offset(member) % NVK_HW_ROOT_TABLE_SIZE)

static inline bool nvk_use_hw_root_table(const struct nv_device_info *info,
                                         bool is_gfx)
{
   return is_gfx && info->cls_eng3d >= TURING_A;
}

enum ENUM_PACKED nvk_descriptor_set_type {
   NVK_DESCRIPTOR_SET_TYPE_NONE,
   NVK_DESCRIPTOR_SET_TYPE_SET,
   NVK_DESCRIPTOR_SET_TYPE_PUSH,
   NVK_DESCRIPTOR_SET_TYPE_BUFFER,
};

struct nvk_descriptor_set_binding {
   enum nvk_descriptor_set_type type;
   struct nvk_descriptor_set *set;
   struct nvk_push_descriptor_set *push;
};

struct nvk_descriptor_state {
   alignas(16) char root[sizeof(struct nvk_root_descriptor_table)];
   void (*flush_root)(struct nvk_cmd_buffer *cmd,
                      struct nvk_descriptor_state *desc,
                      size_t offset, size_t size);

   struct nvk_descriptor_set_binding sets[NVK_MAX_SETS];
   uint32_t push_dirty;

   /* NFSMW: set_dynamic_buffer_start was already sent to the GPU in this
    * command buffer with the values root holds. Set to false by the
    * state memset (reset and invalidate) and by nvk_cmd_buffer_begin_graphics.
    */
   bool dyn_start_synced;

#ifdef HAVE_SWITCH_PLATFORM
   /* NFSMW: set 4 by differences. Bit d says that the four words
    * dynamic_buffers[k][d] of root were already sent to the GPU in
    * this command buffer with the values root holds. Set to 0 by the
    * state memset (reset and invalidate) and by nvk_cmd_buffer_begin_graphics,
    * same as dyn_start_synced.
    */
   uint64_t dyn_synced;
#endif
};

#define nvk_descriptor_state_get_root(desc, member, dst) do { \
   const struct nvk_root_descriptor_table *root = \
      (const struct nvk_root_descriptor_table *)(desc)->root; \
   *dst = root->member; \
} while (0)

#define nvk_descriptor_state_get_root_array(desc, member, \
                                            start, count, dst) do { \
   const struct nvk_root_descriptor_table *root = \
      (const struct nvk_root_descriptor_table *)(desc)->root; \
   unsigned _start = start; \
   unsigned _count = count; \
   assert(_start + _count <= ARRAY_SIZE(root->member)); \
   for (unsigned _index = 0; _index < _count; _index++) \
      (dst)[_index] = root->member[_index + _start]; \
} while (0)

#define nvk_descriptor_state_set_root(cmd, desc, member, src) do { \
   struct nvk_descriptor_state *_desc = (desc); \
   struct nvk_root_descriptor_table *root = \
      (struct nvk_root_descriptor_table *)_desc->root; \
   root->member = (src); \
   if (_desc->flush_root != NULL) { \
      size_t offset = (char *)&root->member - (char *)root; \
      _desc->flush_root((cmd), _desc, offset, sizeof(root->member)); \
   } \
} while (0)

#define nvk_descriptor_state_set_root_array(cmd, desc, member, \
                                            start, count, src) do { \
   struct nvk_descriptor_state *_desc = (desc); \
   struct nvk_root_descriptor_table *root = \
      (struct nvk_root_descriptor_table *)_desc->root; \
   unsigned _start = start; \
   unsigned _count = count; \
   assert(_start + _count <= ARRAY_SIZE(root->member)); \
   for (unsigned _index = 0; _index < _count; _index++) \
      root->member[_index + _start] = (src)[_index]; \
   if (_desc->flush_root != NULL) { \
      size_t offset = (char *)&root->member[_start] - (char *)root; \
      _desc->flush_root((cmd), _desc, offset, \
                        _count * sizeof(root->member[0])); \
   } \
} while (0)

struct nvk_attachment {
   VkRenderingAttachmentFlagBitsKHR flags;

   VkFormat vk_format;
   struct nvk_image_view *iview;

   VkResolveModeFlagBits resolve_mode;
   struct nvk_image_view *resolve_iview;

   /* Needed to track the value of storeOp in case we need to copy images for
    * the DRM_FORMAT_MOD_LINEAR case */
   VkAttachmentStoreOp store_op;
};

struct nvk_rendering_state {
   VkRenderingFlagBits flags;

   VkRect2D area;
   uint32_t layer_count;
   uint32_t view_mask;
   uint32_t samples;

   uint32_t color_att_count;
   struct nvk_attachment color_att[NVK_MAX_RTS];
   struct nvk_attachment depth_att;
   struct nvk_attachment stencil_att;
   struct nvk_attachment fsr_att;

   /* True if all the conditions are met to allow rendering to linear */
   bool linear;

};

struct nvk_graphics_state {
   struct nvk_rendering_state render;
   struct nvk_descriptor_state descriptors;

   VkShaderStageFlags shaders_dirty;
   struct nvk_shader *shaders[MESA_SHADER_MESH + 1];

   struct nvk_cbuf_group {
      uint16_t dirty;
#ifdef HAVE_SWITCH_PLATFORM
      /* NFSMW: slots of cbufs[] that are DYNAMIC_UBO and
       * slots that are DESC_SET or UBO_DESC. Kept up to date by
       * nvk_cmd_flush_gfx_shaders every time it copies a map entry
       * (nvk_switch_set4_anotar_tipo), so they always agree with
       * cbufs[], also in the slots left by a previous shader. The
       * state memset sets them to 0 together with cbufs[].
       */
      uint16_t dyn_ubo_slots;
      uint16_t desc_slots;
#endif
      struct nvk_cbuf cbufs[16];
#ifdef HAVE_SWITCH_PLATFORM
      /* NFSMW: the last thing bound in each slot through
       * SET_CONSTANT_BUFFER_SELECTOR (address and already aligned size), for
       * the guard of set 4 by differences (nvk_switch_set4_comprobar).
       */
      uint64_t bound_addr[16];
      uint32_t bound_size[16];
#endif
   } cbuf_groups[5];

#ifdef HAVE_SWITCH_PLATFORM
   /* NFSMW: NVK_SW_M_CBUFS.
    *
    * sw_cbuf_shaders: the shader of each cbuf group, as
    * nvk_cmd_flush_gfx_cbufs looks it up; nvk_cmd_flush_gfx_shaders computes it every time
    * the shaders change. sw_cbufs_limpios: no cbuf in use has its
    * dirty bit set (computed at the end of nvk_cmd_flush_gfx_cbufs and cleared by
    * whoever dirties a cbuf or changes a shader). sw_grupos*: the cbuf groups
    * of a set of stages, with their key. The state memset leaves everything at 0:
    * no map, cleanliness unknown and no cached groups.
    */
   const struct nvk_shader *sw_cbuf_shaders[5];
   bool sw_cbuf_shaders_validos;
   bool sw_cbufs_limpios;
   bool sw_grupos_tarea;
   VkShaderStageFlags sw_grupos_etapas;
   uint32_t sw_grupos;
#endif

   /* Used for meta save/restore */
   VkDeviceAddressRangeKHR vb0;

   /* Needed by vk_command_buffer::dynamic_graphics_state */
   struct vk_vertex_input_state _dynamic_vi;
   struct vk_sample_locations_state _dynamic_sl;
};

struct nvk_compute_state {
   struct nvk_descriptor_state descriptors;
   struct nvk_shader *shader;
   bool active_compute_invocations_query;
};

#ifdef HAVE_SWITCH_PLATFORM
#include "util/u_atomic.h"

/* NFSMW: set 4 by differences (NVK_SWITCH_DYN_UBO_DELTA).
 * The full explanation is in nvk_cmd_buffer.c, next to nvk_switch_set4.
 */
struct nvk_switch_set4_cuentas {
   uint64_t enlaces_diferencia; /* enlaces con dinamicos por diferencias */
   uint64_t enlaces_completos;  /* enlaces con dinamicos como siempre */
   uint64_t escrituras_raiz;    /* flush_root de dynamic_buffers mandados */
   uint64_t dwords_raiz;        /* palabras de dynamic_buffers mandadas */
   uint64_t cbufs_sucios;       /* cbufs DYNAMIC_UBO ensuciados */
   uint64_t cbufs_ahorrados;    /* cbufs DYNAMIC_UBO que no hacia falta ensuciar */
   uint64_t dibujos;            /* dibujos con el camino nuevo activo */
   uint64_t comprobaciones;     /* cbufs comprobados por la guardia */
   uint64_t diferencias;        /* cbufs mal enlazados que vio la guardia */
};

/* The app (nfsmw_nativo_dibujos.cpp) reads and writes this structure by its
 * name, through a weak symbol: the order and types of the fields are a
 * contract. If they change, bump version.
 */
struct nvk_switch_set4 {
   int32_t version;  /* 1 */
   int32_t pedido;   /* lo escribe la app: 1 si, 0 no; -1 no lo dice */
   int32_t entorno;  /* NVK_SWITCH_DYN_UBO_DELTA: -1 sin leer, 0 no, 1 si */
   int32_t apagado;  /* 1: la guardia vio una DIFERENCIA (para la sesion) */
   struct nvk_switch_set4_cuentas total; /* de los buferes terminados */
};

extern struct nvk_switch_set4 nvk_switch_set4;

int32_t nvk_switch_set4_leer_entorno(void);
void nvk_switch_set4_sumar(struct nvk_cmd_buffer *cmd);

static inline bool
nvk_switch_set4_activo(void)
{
   int32_t entorno = p_atomic_read_relaxed(&nvk_switch_set4.entorno);
   if (unlikely(entorno < 0))
      entorno = nvk_switch_set4_leer_entorno();
   return entorno > 0 &&
          p_atomic_read_relaxed(&nvk_switch_set4.pedido) != 0 &&
          p_atomic_read_relaxed(&nvk_switch_set4.apagado) == 0;
}

/* After copying entry i of a shader's map into group->cbufs[i]. */
static inline void
nvk_switch_set4_anotar_tipo(struct nvk_cbuf_group *group, uint32_t i)
{
   const uint16_t bit = (uint16_t)BITFIELD_BIT(i);
   const enum nvk_cbuf_type tipo = group->cbufs[i].type;

   group->dyn_ubo_slots &= (uint16_t)~bit;
   group->desc_slots &= (uint16_t)~bit;
   if (tipo == NVK_CBUF_TYPE_DYNAMIC_UBO)
      group->dyn_ubo_slots |= bit;
   else if (tipo == NVK_CBUF_TYPE_DESC_SET || tipo == NVK_CBUF_TYPE_UBO_DESC)
      group->desc_slots |= bit;
}
#endif

#ifdef HAVE_SWITCH_PLATFORM
/* NFSMW: per-part measurement of the draw and exact improvements.
 * The full explanation is in nvk_cmd_buffer.c, next to nvk_switch_dibujo.
 */
enum nvk_sw_parte {
   NVK_SW_P_DIBUJO = 0,    /* vkCmdDraw y vkCmdDrawIndexed enteros */
   NVK_SW_P_PUSH_DESC,     /* nvk_cmd_buffer_flush_push_descriptors */
   NVK_SW_P_DINAMICO,      /* nvk_cmd_flush_gfx_dynamic_state */
   NVK_SW_P_TOQUE_SHADERS, /* medir_fallos: leer antes los shaders sucios */
   NVK_SW_P_SHADERS,       /* nvk_cmd_flush_gfx_shaders */
   NVK_SW_P_CBUFS,         /* nvk_cmd_flush_gfx_cbufs */
   NVK_SW_P_EMITIR,        /* la orden del dibujo en el pushbuf */
   NVK_SW_P_PIPE_SHADERS,  /* vkCmdBindPipeline: nvk_cmd_bind_shaders */
   NVK_SW_P_PIPE_TOQUE,    /* medir_fallos: leer antes el estado del pipeline */
   NVK_SW_P_PIPE_COPIA,    /* vkCmdBindPipeline: vk_dynamic_graphics_state_copy */
   NVK_SW_P_SETS,          /* vkCmdBindDescriptorSets de graficos entero */
   NVK_SW_P_SETS_RAIZ,     /* dentro: descriptores dinamicos a la tabla raiz */
   NVK_SW_P_SETS_ENSUCIAR, /* dentro: ensuciar cbufs */
   NVK_SW_P_RAIZ,          /* nvk_cmd_flush_gfx_root_desc, todas las llamadas */
   NVK_SW_P_TROZO,         /* nvk_cmd_buffer_new_push (trozo nuevo del pushbuf) */
   NVK_SW_P_ENLACE_VB,     /* vkCmdBindVertexBuffers */
   NVK_SW_PARTES
};

enum nvk_sw_cuenta {
   NVK_SW_C_DIBUJOS_MEDIDOS = 0, /* dibujos medidos por partes */
   NVK_SW_C_DWORDS_DIBUJO,     /* dwords de pushbuf de esos dibujos (flush y orden) */
   NVK_SW_C_DIN_CON_TRABAJO,   /* de esos, con algun bit dinamico sucio */
   NVK_SW_C_DIN_BITS,          /* bits dinamicos sucios que llevaban */
   NVK_SW_C_SHADERS_SUCIOS,    /* de esos, con shaders sucios */
   NVK_SW_C_CBUFS_REENLAZADOS, /* cbufs reenlazados en esos */
   NVK_SW_C_PIPES_MEDIDOS,     /* enlaces de pipeline medidos */
   NVK_SW_C_PIPES_SIN_EFECTO,  /* de esos, copias que no cambiaron NADA */
   NVK_SW_C_PIPES_BITS_NUEVOS, /* bits sucios nuevos que dejo la copia */
   NVK_SW_C_SETS_MEDIDOS,      /* enlaces de conjuntos medidos */
   NVK_SW_C_RAIZ,              /* subidas a la tabla raiz (todas) */
   NVK_SW_C_RAIZ_DWORDS,       /* sus dwords de datos (todas) */
   NVK_SW_C_TROZOS,            /* trozos nuevos del pushbuf (todos) */
   NVK_SW_CUENTAS
};

enum nvk_sw_mejora {
   NVK_SW_M_EMISION = 0, /* p02: cada orden entera de una vez (mismos bytes) */
   NVK_SW_M_CBUFS,       /* p03: cbufs sin trabajo repetido */
   NVK_SW_M_DINAMICO,    /* p04: estado dinamico por grupos sucios */
   NVK_SW_M_SET4_RAPIDO, /* p05: set 4 por el camino rapido */
   NVK_SW_M_PRECARGA,    /* p06: precarga de pipeline y shaders */
   NVK_SW_MEJORAS
};

/* Each turno counts calls of one type, to measure 1 of every N. */
enum nvk_sw_turno {
   NVK_SW_T_DIBUJO = 0,
   NVK_SW_T_PIPE,
   NVK_SW_T_SETS,
   NVK_SW_T_RAIZ,
   NVK_SW_T_VB,
   NVK_SW_TURNOS
};

struct nvk_sw_parte_total {
   uint64_t veces;
   uint64_t ticks; /* CNTPCT_EL0, a ticks_por_segundo (19,2 MHz en la Switch) */
};

struct nvk_sw_mejora_estado {
   int32_t pedido;         /* lo escribe la app: 1 si, 0 no; -1 no lo dice (= si) */
   int32_t apagado;        /* 1: su guardia vio una DIFERENCIA (para la sesion) */
   uint64_t usos;          /* veces que se decidio por el camino nuevo */
   uint64_t validadas;     /* comprobaciones de la guardia que salieron iguales */
   uint64_t diferencias;   /* comprobaciones que salieron distintas */
   uint64_t sin_comprobar; /* comprobaciones que no se pudieron hacer */
};

/* The app (nfsmw_nativo_dibujos.cpp) reads and writes this structure by its
 * name, through a weak symbol: the order and types of the fields are a
 * contract. If they change, bump version. The totals come from finished
 * command buffers (nvk_EndCommandBuffer).
 */
struct nvk_switch_dibujo {
   int32_t version;      /* 1 */
   int32_t medir;        /* app: N > 0 potencia de 2 = medir 1 de cada N; si no, no se mide */
   int32_t medir_fallos; /* app: 1 = leer antes lo que se va a usar y medirlo aparte */
   int32_t entorno;      /* NVK_SWITCH_DIBUJO: -1 sin leer, 0 todo apagado, 1 si */
   uint64_t ticks_por_segundo;
   struct nvk_sw_parte_total partes[NVK_SW_PARTES];
   uint64_t cuentas[NVK_SW_CUENTAS];
   struct nvk_sw_mejora_estado mejoras[NVK_SW_MEJORAS];
};

extern struct nvk_switch_dibujo nvk_switch_dibujo;

/* Per-command-buffer data. It lives in nvk_cmd_state: the reset memset
 * zeroes it and nvk_sw_empezar_bufer configures it in vkBeginCommandBuffer.
 */
struct nvk_sw_cmd {
   struct nvk_sw_parte_total partes[NVK_SW_PARTES];
   uint64_t cuentas[NVK_SW_CUENTAS];
   uint64_t usos[NVK_SW_MEJORAS];
   uint64_t validadas[NVK_SW_MEJORAS];
   uint64_t diferencias[NVK_SW_MEJORAS];
   uint64_t sin_comprobar[NVK_SW_MEJORAS];
   uint32_t turno[NVK_SW_TURNOS];
   uint32_t mascara_medir; /* N - 1, o UINT32_MAX si no se mide */
   uint32_t activas;       /* bit m: la mejora m va por el camino nuevo */
   bool medir_fallos;
   bool midiendo;          /* el dibujo en curso se mide por partes */
   bool midiendo_pipe;     /* el vkCmdBindPipeline en curso se mide */
   bool midiendo_sets;     /* el vkCmdBindDescriptorSets en curso se mide */
   uint32_t *dw_inicio;    /* pushbuf al empezar el dibujo medido */
   const struct nvk_cmd_mem *mem_inicio;
   int8_t modo_emision;    /* p02: NVK_SW_DE_SIEMPRE, NVK_SW_RAPIDO o NVK_SW_COMPROBAR */
};
#endif

struct nvk_cmd_push {
   void *map;
   uint64_t addr;
   uint32_t range;
   bool incomplete;
   bool no_prefetch;
};

struct nvk_cmd_buffer {
   struct vk_command_buffer vk;

   struct nvk_cmd_state {
      uint64_t descriptor_buffers[NVK_MAX_SETS];
      struct nvk_graphics_state gfx;
      struct nvk_compute_state cs;
      VkQueryPipelineStatisticFlags inherited_pipeline_statistics;
#ifdef HAVE_SWITCH_PLATFORM
      /* NFSMW: counts of set 4 by differences for this
       * command buffer; nvk_EndCommandBuffer adds them to nvk_switch_set4.
       */
      struct nvk_switch_set4_cuentas set4;
      /* NFSMW: per-part measurement and state of the improvements for
       * this buffer; nvk_EndCommandBuffer adds it to nvk_switch_dibujo.
       */
      struct nvk_sw_cmd sw;
#endif
   } state;

   /** List of nvk_cmd_mem
    *
    * This list exists entirely for ownership tracking.  Everything in here
    * must also be in pushes or bo_refs if it is to be referenced by this
    * command buffer.
    */
   struct list_head owned_mem;
   struct list_head owned_gart_mem;
   struct list_head owned_qmd;

   struct nvk_cmd_mem *upload_mem;
   uint32_t upload_offset;

   struct nvk_cmd_mem *cond_render_mem;
   /** Array of struct nvk_cmd_mem* */
   struct util_dynarray copy_memory_indirect_temps;

   struct nvk_cmd_mem *push_mem;
   uint32_t *push_mem_limit;
   struct nv_push push;

   /** Array of struct nvk_cmd_push
    *
    * This acts both as a BO reference as well as provides a range in the
    * buffer to use as a pushbuf.
    */
   struct util_dynarray pushes;

   uint8_t prev_subc;

#ifdef HAVE_SWITCH_PLATFORM
   bool switch_mme_sync_pending;
#endif
};

VK_DEFINE_HANDLE_CASTS(nvk_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

extern const struct vk_command_buffer_ops nvk_cmd_buffer_ops;

#ifdef HAVE_SWITCH_PLATFORM
/* NFSMW: helpers for the per-part measurement and the improvements. */
static inline uint64_t
nvk_sw_ticks(void)
{
#if defined(__aarch64__)
   uint64_t t;
   __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
   return t;
#else
   return 0;
#endif
}

/* Current tick if the draw in progress is being measured; otherwise 0. */
static inline uint64_t
nvk_sw_t0(const struct nvk_cmd_buffer *cmd)
{
   return unlikely(cmd->state.sw.midiendo) ? nvk_sw_ticks() : 0;
}

/* Adds the time since t to part p and returns the current tick. With t = 0
 * (not measured) it does nothing and returns 0.
 */
static inline uint64_t
nvk_sw_parte(struct nvk_cmd_buffer *cmd, enum nvk_sw_parte p, uint64_t t)
{
   if (likely(t == 0))
      return 0;
   const uint64_t ahora = nvk_sw_ticks();
   cmd->state.sw.partes[p].veces++;
   cmd->state.sw.partes[p].ticks += ahora - t;
   return ahora;
}

/* Is this call due to be measured? 1 of every N, counting per type. */
static inline bool
nvk_sw_toca_medir(struct nvk_cmd_buffer *cmd, enum nvk_sw_turno turno)
{
   const uint32_t m = cmd->state.sw.mascara_medir;
   return unlikely(m != UINT32_MAX) && ((++cmd->state.sw.turno[turno]) & m) == 0;
}

static inline bool
nvk_sw_activa(const struct nvk_cmd_buffer *cmd, enum nvk_sw_mejora m)
{
   return (cmd->state.sw.activas & (1u << m)) != 0;
}

/* Should the guard of improvement m check this use? The first
 * NVK_SW_A_COMPROBAR uses of the process and then 1 of every NVK_SW_CADA, with the
 * process-wide count: a command buffer may never reach NVK_SW_CADA
 * uses (that is what happened to the set 4 guard, which stopped
 * checking after 200,000 draws). The caller has already counted this use.
 */
#define NVK_SW_A_COMPROBAR 20000
#define NVK_SW_CADA 1024

static inline bool
nvk_sw_toca_comprobar(const struct nvk_cmd_buffer *cmd, enum nvk_sw_mejora m)
{
   const uint64_t n =
      p_atomic_read_relaxed(&nvk_switch_dibujo.mejoras[m].usos) +
      cmd->state.sw.usos[m];
   return n <= NVK_SW_A_COMPROBAR || (n & (NVK_SW_CADA - 1)) == 0;
}

void nvk_sw_diferencia(struct nvk_cmd_buffer *cmd, enum nvk_sw_mejora m,
                       const char *que);
void nvk_sw_empezar_bufer(struct nvk_cmd_buffer *cmd);
void nvk_sw_sumar(struct nvk_cmd_buffer *cmd);
void nvk_sw_cmd_set_dynamic_graphics_state(struct vk_command_buffer *vk_cmd,
                                           const struct vk_dynamic_graphics_state *state);
#endif

void
nvk_descriptor_state_fini(struct nvk_cmd_buffer *cmd,
                          struct nvk_descriptor_state *desc);

static inline struct nvk_device *
nvk_cmd_buffer_device(struct nvk_cmd_buffer *cmd)
{
   return (struct nvk_device *)cmd->vk.base.device;
}

static inline struct nvk_cmd_pool *
nvk_cmd_buffer_pool(struct nvk_cmd_buffer *cmd)
{
   return (struct nvk_cmd_pool *)cmd->vk.pool;
}

void nvk_cmd_buffer_new_push(struct nvk_cmd_buffer *cmd);

#define NVK_CMD_BUFFER_MAX_PUSH 512

static inline struct nv_push *
nvk_cmd_buffer_push(struct nvk_cmd_buffer *cmd, uint32_t dw_count)
{
   assert(dw_count <= NVK_CMD_BUFFER_MAX_PUSH);

   /* Compare to the actual limit on our push bo */
   if (unlikely(cmd->push.end + dw_count > cmd->push_mem_limit))
      nvk_cmd_buffer_new_push(cmd);

   cmd->push.limit = cmd->push.end + dw_count;
   
   return &cmd->push;
}

#ifdef HAVE_SWITCH_PLATFORM
/* NFSMW: batched emission (NVK_SW_M_EMISION). See
 * nvk_sw_comprobar_1inc in nvk_cmd_draw.c.
 *
 * A ONEINCR command of the 3D subchannel with its n data words, written at once: the
 * header with the final count (the count field is the same, bits 28:16,
 * in INCR, ONEINCR and METHOD_COUNT of cl906f.h) and the data after it. It leaves the
 * same bytes as P_1INC followed by n P_INLINE_DATA, and nv_push with the same
 * end, last_hdr and last_hdr_dw. The caller has reserved 1 + n dwords.
 */
#define NVK_SW_DE_SIEMPRE 0
#define NVK_SW_RAPIDO 1
#define NVK_SW_COMPROBAR 2
#define NVK_SW_BORRADOR 72

static inline void
nvk_sw_1inc_rapido(struct nv_push *p, uint32_t mthd,
                   const uint32_t *datos, uint32_t n)
{
   uint32_t *d = p->end;
   const uint32_t cabecera = NVC0_FIFO_PKHDR_1I(SUBC_NV9097, mthd, n);
   d[0] = cabecera;
   for (uint32_t i = 0; i < n; i++)
      d[1 + i] = datos[i];
   p->last_hdr = d;
   p->last_hdr_dw = cabecera;
   p->end = d + 1 + n;
}

/* The mode of the upcoming emissions, until the end of the next draw:
 * decided once per draw (when its command ends) so as not to pay for the
 * decision at every site. Both paths give the same bytes: which one is used
 * only changes the cost and whether it is checked.
 */
static inline int
nvk_sw_emision_modo(struct nvk_cmd_buffer *cmd)
{
   if (!nvk_sw_activa(cmd, NVK_SW_M_EMISION))
      return NVK_SW_DE_SIEMPRE;
   cmd->state.sw.usos[NVK_SW_M_EMISION]++;
   return unlikely(nvk_sw_toca_comprobar(cmd, NVK_SW_M_EMISION))
             ? NVK_SW_COMPROBAR : NVK_SW_RAPIDO;
}

/* Guard: compares what the usual code has just emitted into p, from
 * inicio, with the fast command built in a scratch buffer.
 */
void nvk_sw_comprobar_1inc(struct nvk_cmd_buffer *cmd, const struct nv_push *p,
                           const uint32_t *inicio, uint32_t mthd,
                           const uint32_t *datos, uint32_t n);
#endif

void
nvk_cmd_buffer_push_indirect(struct nvk_cmd_buffer *cmd,
                             uint64_t addr, uint32_t dw_count);

#ifdef HAVE_SWITCH_PLATFORM
void
nvk_cmd_buffer_switch_sync_host(struct nvk_cmd_buffer *cmd);

void
nvk_cmd_buffer_switch_mme_consumer(struct nvk_cmd_buffer *cmd);
#endif

void nvk_cmd_buffer_begin_graphics(struct nvk_cmd_buffer *cmd,
                                   const VkCommandBufferBeginInfo *pBeginInfo);
void nvk_cmd_buffer_begin_compute(struct nvk_cmd_buffer *cmd,
                                  const VkCommandBufferBeginInfo *pBeginInfo);

void nvk_cmd_invalidate_graphics_state(struct nvk_cmd_buffer *cmd);
void nvk_cmd_invalidate_compute_state(struct nvk_cmd_buffer *cmd);

void nvk_cmd_bind_shaders(struct vk_command_buffer *vk_cmd,
                          uint32_t stage_count,
                          const mesa_shader_stage *stages,
                          struct vk_shader ** const shaders);

void nvk_cmd_bind_graphics_shader(struct nvk_cmd_buffer *cmd,
                                  const mesa_shader_stage stage,
                                  struct nvk_shader *shader);

void nvk_cmd_bind_compute_shader(struct nvk_cmd_buffer *cmd,
                                 struct nvk_shader *shader);

void nvk_cmd_dirty_cbufs_for_descriptors(struct nvk_cmd_buffer *cmd,
                                         VkShaderStageFlags stages,
                                         uint32_t sets_start, uint32_t sets_end);
void nvk_cmd_bind_vertex_buffer(struct nvk_cmd_buffer *cmd, uint32_t vb_idx,
                                VkDeviceAddressRangeKHR addr_range);

static inline struct nvk_descriptor_state *
nvk_get_descriptors_state(struct nvk_cmd_buffer *cmd,
                          VkPipelineBindPoint bind_point)
{
   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      return &cmd->state.gfx.descriptors;
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      return &cmd->state.cs.descriptors;
   default:
      UNREACHABLE("Unhandled bind point");
   }
}

static inline struct nvk_descriptor_state *
nvk_get_descriptor_state_for_stages(struct nvk_cmd_buffer *cmd,
                                    VkShaderStageFlags stages)
{
   if (stages & VK_SHADER_STAGE_COMPUTE_BIT) {
      assert(stages == VK_SHADER_STAGE_COMPUTE_BIT);
      return &cmd->state.cs.descriptors;
   } else if (stages & NVK_SHADER_STAGE_GRAPHICS_BITS) {
      assert(!(stages & ~NVK_SHADER_STAGE_GRAPHICS_BITS));
      return &cmd->state.gfx.descriptors;
   } else {
      UNREACHABLE("Unknown shader stage");
   }
}

/**
 * Gets the most recently used subchannel in the nvk_cmd_buffer, or a valid
 * default subchannel if none has been used.
 *
 * Note that this isn't guaranteed to be the subchannel that the hardware is
 * actually using (eg. at the beginning of the command buffer or after an
 * indirect) so callers cannot rely on this value for correctness.
 */
static inline uint8_t
nvk_cmd_buffer_last_subchannel(const struct nvk_cmd_buffer *cmd)
{
   if (cmd->push.last_hdr_dw) {
      return NVC0_FIFO_SUBC_FROM_PKHDR(cmd->push.last_hdr_dw);
   } else {
      return cmd->prev_subc;
   }
}

VkQueueFlags nvk_cmd_buffer_queue_flags(struct nvk_cmd_buffer *cmd);

VkResult nvk_cmd_buffer_alloc_mem(struct nvk_cmd_buffer *cmd,
                                  bool force_gart,
                                  struct nvk_cmd_mem **mem_out);

VkResult nvk_cmd_buffer_upload_alloc(struct nvk_cmd_buffer *cmd,
                                     uint32_t size, uint32_t alignment,
                                     uint64_t *addr, void **ptr);

VkResult nvk_cmd_buffer_upload_data(struct nvk_cmd_buffer *cmd,
                                    const void *data, uint32_t size,
                                    uint32_t alignment, uint64_t *addr);

VkResult nvk_cmd_buffer_alloc_qmd(struct nvk_cmd_buffer *cmd,
                                  uint32_t size, uint32_t alignment,
                                  uint64_t *addr, void **ptr);

void nvk_cmd_flush_wait_dep(struct nvk_cmd_buffer *cmd,
                            const VkDependencyInfo *dep,
                            bool wait);

void nvk_cmd_invalidate_deps(struct nvk_cmd_buffer *cmd,
                             uint32_t dep_count,
                             const VkDependencyInfo *deps);

void
nvk_cmd_buffer_flush_push_descriptors(struct nvk_cmd_buffer *cmd,
                                      struct nvk_descriptor_state *desc);

void
nvk_cmd_buffer_flush_printf_buffer(struct nvk_cmd_buffer *cmd,
                                   struct nvk_descriptor_state *desc);

bool
nvk_cmd_buffer_get_cbuf_addr(struct nvk_cmd_buffer *cmd,
                             const struct nvk_descriptor_state *desc,
                             const struct nvk_shader *shader,
                             const struct nvk_cbuf *cbuf,
                             struct nvk_buffer_address *addr_out);
uint64_t
nvk_cmd_buffer_get_cbuf_descriptor_addr(struct nvk_cmd_buffer *cmd,
                                        const struct nvk_descriptor_state *desc,
                                        const struct nvk_cbuf *cbuf);

VkResult nvk_cmd_flush_cs_qmd(struct nvk_cmd_buffer *cmd,
                              const struct nvk_cmd_state *state,
                              uint32_t global_size[3],
                              uint64_t *qmd_addr_out,
                              uint64_t *root_desc_addr_out);

void nvk_cmd_flush_gfx_dynamic_state(struct nvk_cmd_buffer *cmd);
void nvk_cmd_flush_gfx_shaders(struct nvk_cmd_buffer *cmd);
void nvk_cmd_flush_gfx_cbufs(struct nvk_cmd_buffer *cmd);

void nvk_cmd_dispatch_shader(struct nvk_cmd_buffer *cmd,
                             struct nvk_shader *shader,
                             const void *push_data, size_t push_size,
                             uint32_t groupCountX,
                             uint32_t groupCountY,
                             uint32_t groupCountZ);

void nvk_cmd_dispatch_with_root(struct nvk_cmd_buffer *cmd,
                                struct nvk_shader *shader,
                                const void *root,
                                size_t root_size,
                                uint32_t groupCountX,
                                uint32_t groupCountY,
                                uint32_t groupCountZ);

void nvk_cmd_fill_memory_ce(struct nvk_cmd_buffer *cmd,
                            uint64_t dst_addr, uint64_t size,
                            uint32_t data);

void nvk_cmd_copy_buffer_ce(struct nvk_cmd_buffer *cmd,
                            const VkCopyBufferInfo2 *pCopyBufferInfo);
void nvk_cmd_copy_buffer_to_image_ce(struct nvk_cmd_buffer *cmd,
                                     const VkCopyBufferToImageInfo2 *pCopyBufferToImageInfo);
void nvk_cmd_copy_image_to_buffer_ce(struct nvk_cmd_buffer *cmd,
                                     const VkCopyImageToBufferInfo2 *pCopyImageToBufferInfo);
void nvk_cmd_copy_image_ce(struct nvk_cmd_buffer *cmd,
                           const VkCopyImageInfo2 *pCopyImageInfo);

void nvk_meta_resolve_rendering(struct nvk_cmd_buffer *cmd,
                                const VkRenderingInfo *pRenderingInfo);

void nvk_linear_render_copy(struct nvk_cmd_buffer *cmd,
                            const struct nvk_image_view *iview,
                            VkRect2D copy_rect,
                            bool copy_to_tiled_shadow);

#endif
