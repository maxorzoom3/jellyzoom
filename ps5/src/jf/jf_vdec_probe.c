/*
 * Jellyfin Wheel - hardware decoder probe (Stage 8). Brings a sceVideodec2 decoder up and down for a few codecs
 * and reports every step to klog (VDEC lines). It decodes nothing and changes nothing else.
 *
 * The call sequence and structure layouts are those of EVO Player / Jelly5 (GPL-3.0-or-later,
 * app/engine/media/include/sce/sce_videodec2.h and src/evo_vdec_native.c), which in turn follow
 * ProsperoLight. Skipped when /data/jellyzoom-vdec-off exists.
 */
#include "platform.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define VD_AVC 1u
#define VD_HEVC 974921u
#define VD_RESOURCE_COMPUTE 1u

typedef struct {
   uint64_t size; uint32_t resource_type, codec_type, profile, max_level;
   int32_t max_width, max_height, max_dpb_frames; uint32_t pipeline_depth;
   uint64_t compute_queue, cpu_affinity; int32_t cpu_priority;
   uint32_t optimize_progressive, check_memory_type, reserved;
} vd_config;
typedef struct {
   uint64_t size, cpu_size; void *cpu; uint64_t gpu_size; void *gpu; uint64_t cpu_gpu_size; void *cpu_gpu;
   uint64_t max_frame_size; uint32_t frame_alignment, reserved;
} vd_memory;
typedef struct { uint64_t size; uint16_t pipe_id, queue_id; uint8_t check_memory_type, r0; uint16_t r1; } vd_ccfg;
typedef struct { uint64_t size, cpu_gpu_size; void *cpu_gpu; } vd_cmem;

extern int sceSysmoduleLoadModule(uint16_t id);
extern int64_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
extern int sceKernelReleaseDirectMemory(int64_t, size_t);
extern int sceKernelMunmap(void *, size_t);
extern int sceKernelMapNamedFlexibleMemory(void **, size_t, int, int, const char *);
extern int sceKernelReleaseFlexibleMemory(void *, size_t);
extern int sceKernelAvailableFlexibleMemorySize(size_t *);
extern int sceKernelConfiguredFlexibleMemorySize(size_t *);

extern int32_t sceVideodec2QueryComputeMemoryInfo(vd_cmem *);
extern int32_t sceVideodec2AllocateComputeQueue(const vd_ccfg *, const vd_cmem *, void **);
extern int32_t sceVideodec2ReleaseComputeQueue(void *);
extern int32_t sceVideodec2QueryDecoderMemoryInfo(const vd_config *, vd_memory *);
extern int32_t sceVideodec2CreateDecoder(const vd_config *, const vd_memory *, void **);
extern int32_t sceVideodec2DeleteDecoder(void *);
extern int32_t sceVideodec2Reset(void *);

static size_t align16k(size_t v) { return (v + 0x3fffu) & ~(size_t)0x3fffu; }

struct region { void *addr; int64_t start; size_t size; };

static int grab(struct region *r, size_t size, int64_t limit)
{
   r->addr = NULL; r->start = -1; r->size = size;
   int rc = sceKernelAllocateDirectMemory(0, limit, size, 0x4000, 12, &r->start);
   if (rc == 0)
      rc = sceKernelMapDirectMemory(&r->addr, size, 0x33, 0, r->start, 0x4000);
   return rc;
}

static void drop(struct region *r)
{
   if (r->addr) sceKernelMunmap(r->addr, r->size);
   if (r->start >= 0) sceKernelReleaseDirectMemory(r->start, r->size);
   r->addr = NULL; r->start = -1;
}

static int bring(const char *tag, uint32_t codec, uint32_t profile, int level, int w, int h, uint32_t depth)
{
   const char *stage = "start";
   int rc = 0;
   int64_t dm = sceKernelGetDirectMemorySize();
   struct region compute, gpu, cpugpu, input, frame;
   void *queue = NULL, *decoder = NULL, *cpu = NULL;
   size_t cpu_map = 0;
   compute.start = gpu.start = cpugpu.start = input.start = frame.start = -1;
   compute.addr = gpu.addr = cpugpu.addr = input.addr = frame.addr = NULL;
   compute.size = gpu.size = cpugpu.size = input.size = frame.size = 0;
   vd_cmem cm; vd_ccfg cc; vd_config cfg; vd_memory mem;
   memset(&cm, 0, sizeof cm); memset(&cc, 0, sizeof cc); memset(&cfg, 0, sizeof cfg); memset(&mem, 0, sizeof mem);
   cm.size = sizeof cm;

#define STEP(name, call) do { stage = name; rc = (call); if (rc != 0) goto done; } while (0)
   STEP("QueryComputeMemoryInfo", sceVideodec2QueryComputeMemoryInfo(&cm));
   STEP("alloc(compute)", grab(&compute, align16k((size_t)cm.cpu_gpu_size), dm));
   cm.cpu_gpu = compute.addr; cm.cpu_gpu_size = compute.size; cc.size = sizeof cc;
   STEP("AllocateComputeQueue", sceVideodec2AllocateComputeQueue(&cc, &cm, &queue));
   if (!queue) { rc = -1; goto done; }

   cfg.size = sizeof cfg; cfg.resource_type = VD_RESOURCE_COMPUTE; cfg.codec_type = codec; cfg.profile = profile;
   cfg.max_level = (uint32_t)level; cfg.max_width = w; cfg.max_height = h; cfg.max_dpb_frames = -1;
   cfg.pipeline_depth = depth; cfg.compute_queue = (uint64_t)(uintptr_t)queue; cfg.cpu_affinity = 0x3f;
   cfg.cpu_priority = 700; cfg.optimize_progressive = 1;
   mem.size = sizeof mem;
   STEP("QueryDecoderMemoryInfo", sceVideodec2QueryDecoderMemoryInfo(&cfg, &mem));
   say("VDEC: %s needs cpu=%zuK gpu=%zuK cpu_gpu=%zuK frame=%zuK align=%u", tag, (size_t)mem.cpu_size >> 10,
       (size_t)mem.gpu_size >> 10, (size_t)mem.cpu_gpu_size >> 10, (size_t)mem.max_frame_size >> 10,
       (unsigned)mem.frame_alignment);
   cpu_map = align16k((size_t)mem.cpu_size);
   STEP("MapNamedFlexibleMemory", sceKernelMapNamedFlexibleMemory(&cpu, cpu_map, 0x03, 0, "JfVdecProbe"));
   mem.cpu = cpu;
   STEP("alloc(gpu)", grab(&gpu, align16k((size_t)mem.gpu_size), dm));
   mem.gpu = gpu.addr; mem.gpu_size = gpu.size;
   if (mem.cpu_gpu_size) {
      STEP("alloc(cpu_gpu)", grab(&cpugpu, align16k((size_t)mem.cpu_gpu_size), dm));
      mem.cpu_gpu = cpugpu.addr; mem.cpu_gpu_size = cpugpu.size;
   }
   STEP("alloc(input)", grab(&input, (size_t)8 << 20, dm));
   STEP("alloc(frame)", grab(&frame, align16k((size_t)mem.max_frame_size) * 4, dm));
   STEP("CreateDecoder", sceVideodec2CreateDecoder(&cfg, &mem, &decoder));
   if (!decoder) { rc = -1; goto done; }
   STEP("Reset", sceVideodec2Reset(decoder));
   stage = "ok";
done:
   say("VDEC: %s %dx%d: %s at [%s] rc=0x%08x", tag, w, h, rc == 0 ? "UP" : "FAILED", stage, (unsigned)rc);
   if (decoder) sceVideodec2DeleteDecoder(decoder);
   drop(&frame); drop(&input); drop(&cpugpu); drop(&gpu);
   if (cpu) { sceKernelReleaseFlexibleMemory(cpu, cpu_map); sceKernelMunmap(cpu, cpu_map); }
   if (queue) sceVideodec2ReleaseComputeQueue(queue);
   drop(&compute);
   return rc;
}

int jf_vdec_probe(void)
{
   if (access("/data/jellyzoom-vdec-off", F_OK) == 0) {
      say("VDEC: probe skipped (/data/jellyzoom-vdec-off)");
      return -1;
   }
   size_t flex = 0, flexTotal = 0;
   sceKernelAvailableFlexibleMemorySize(&flex);
   sceKernelConfiguredFlexibleMemorySize(&flexTotal);
   say("VDEC: probe starts, flexible memory %zuM of %zuM free, direct memory %lldM", flex >> 20, flexTotal >> 20,
       (long long)(sceKernelGetDirectMemorySize() >> 20));
   const int sm = sceSysmoduleLoadModule(207);
   say("VDEC: sceSysmoduleLoadModule(207) = 0x%08x", (unsigned)sm);
   int ok = 0;
   ok += bring("AVC", VD_AVC, 100, 51, 1920, 1088, 4) == 0;
   ok += bring("HEVC", VD_HEVC, 1, 153, 3840, 2176, 4) == 0;
   ok += bring("HEVC-Main10", VD_HEVC, 2, 153, 1920, 1088, 1) == 0;
   sceKernelAvailableFlexibleMemorySize(&flex);
   say("VDEC: probe ends, %d of 3 decoders came up, flexible memory %zuM free", ok, flex >> 20);
   return ok;
}
