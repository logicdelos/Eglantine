// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/nvdrv/devices/ioctl_serialization.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl_gpu.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "video_core/nx2.h"
#include "core/hle/service/nvdrv/nvdrv.h"

namespace Service::Nvidia::Devices {

namespace {
void ApplyNx2CharacteristicsTail(std::span<u8> block);
} // namespace

nvhost_ctrl_gpu::nvhost_ctrl_gpu(Core::System& system_, EventInterface& events_interface_)
    : nvdevice{system_}, events_interface{events_interface_} {
    error_notifier_event = events_interface.CreateEvent("CtrlGpuErrorNotifier");
    unknown_event = events_interface.CreateEvent("CtrlGpuUnknownEvent");

    // nvgpu fills the ZBC tables at boot (gr_gk20a_load_zbc_default_table): colour entries zero
    // (fmt 1), unorm one (fmt 2, ds 0xffffffff / l2 1.0f), RF32_GF32_BF32_AF32 zero and one (fmt 4)
    // and depth entries 0.0f and 1.0f in FP32 (fmt 1), each with ref_cnt 1. ZBCSetTable appends.
    constexpr u32 One = 0x3f800000; // 1.0f
    zbc_colors = {
        {{0, 0, 0, 0}, {0, 0, 0, 0}, 0x1, 1},
        {{~0u, ~0u, ~0u, ~0u}, {One, One, One, One}, 0x2, 1},
        {{0, 0, 0, 0}, {0, 0, 0, 0}, 0x4, 1},
        {{One, One, One, One}, {One, One, One, One}, 0x4, 1},
    };
    zbc_depths = {{0, 0x1, 1}, {One, 0x1, 1}};
}
nvhost_ctrl_gpu::~nvhost_ctrl_gpu() {
    events_interface.FreeEvent(error_notifier_event);
    events_interface.FreeEvent(unknown_event);
}

NvResult nvhost_ctrl_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                                 std::span<u8> output) {
    switch (command.group) {
    case 'G':
        switch (command.cmd) {
        case 0x1:
            return WrapFixed(this, &nvhost_ctrl_gpu::ZCullGetCtxSize, input, output);
        case 0x2:
            return WrapFixed(this, &nvhost_ctrl_gpu::ZCullGetInfo, input, output);
        case 0x3:
            return WrapFixed(this, &nvhost_ctrl_gpu::ZBCSetTable, input, output);
        case 0x4:
            return WrapFixed(this, &nvhost_ctrl_gpu::ZBCQueryTable, input, output);
        //deviation
        case 0x5:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetCharacteristics1, input, output);
        case 0x6:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetTPCMasks1, input, output);
        case 0x7:
            return WrapFixed(this, &nvhost_ctrl_gpu::FlushL2, input, output);
        case 0x14:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetActiveSlotMask, input, output);
        case 0x15:
            return WrapFixed(this, &nvhost_ctrl_gpu::PmuGetGpuLoad, input, output);
        case 0x1c:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetGpuTime, input, output);
        default:
            break;
        }
        break;
    }
    // still unimplemented
    std::string_view friendly_name = [raw = command.raw]() {
        switch (raw) {
            case 0x0d: return "INVAL_ICACHE";
            case 0x0e: return "SET_MMU_DEBUG_MODE ";
            case 0x0f: return "SET_SM_DEBUG_MODE";
            case 0x10: return "WAIT_FOR_PAUSE";
            case 0x11: return "GET_TPC_EXCEPTION_EN_STATUS";
            case 0x12: return "NUM_VSMS";
            case 0x13: return "VSMS_MAPPING";
            case 0x14: return "ZBC_GET_ACTIVE_SLOT_MASK";
            case 0x15: return "PMU_GET_GPU_LOAD";
            case 0x16: return "SET_CG_CONTROLS";
            case 0x17: return "GET_CG_CONTROLS";
            case 0x18: return "SET_PG_CONTROLS";
            case 0x19: return "GET_PG_CONTROLS";
            case 0x1A: return "PMU_GET_ELPG_RESIDENCY_GATING";
            case 0x1B: return "GET_ERROR_CHANNEL_USER_DATA";
            case 0x1D: return "GET_CPU_TIME_CORRELATION_INFO";
            default: return "UNKNOWN";
        }
    }();
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X} {}", command.raw, friendly_name);
    return NvResult::NotImplemented;
}

NvResult nvhost_ctrl_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                                 std::span<const u8> inline_input, std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_ctrl_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                                 std::span<u8> output, std::span<u8> inline_output) {
    switch (command.group) {
    case 'G':
        switch (command.cmd) {
        case 0x5: {
            const auto result = WrapFixedInlOut(this, &nvhost_ctrl_gpu::GetCharacteristics3,
                                                input, output, inline_output);
            if (nx2_caller) {
                ApplyNx2CharacteristicsTail(inline_output);
            }
            return result;
        }
        case 0x6:
            return WrapFixedInlOut(this, &nvhost_ctrl_gpu::GetTPCMasks3, input, output,
                                   inline_output);
        case 0x13: {
            // NVGPU_GPU_IOCTL_VSMS_MAPPING: an 8-byte argument struct plus a buffer of
            // {gpc_index, tpc_index} byte pairs, one per virtual SM. The SDK treats a pair left at
            // its 0xc0c0 sentinel as failure, so fill every pair: one GPC, two SMs per TPC.
            if (command.raw != 0xc0084713 || input.size() != 8 || output.size() != 8 ||
                inline_output.size() < 2) {
                return NvResult::BadParameter;
            }
            std::memcpy(output.data(), input.data(), output.size());
            for (size_t i = 0; i + 1 < inline_output.size(); i += 2) {
                inline_output[i] = 0;                         // gpc_index
                inline_output[i + 1] = static_cast<u8>(i / 4); // tpc_index
            }
            LOG_DEBUG(Service_NVDRV, "VSMS mapping for {} SMs", inline_output.size() / 2);
            return NvResult::Success;
        }
        default:
            break;
        }
        break;
    default:
        break;
    }
    if (nx2_caller && command.group == 'G' && command.cmd == 0x1F) {
        return Nx2GetEngineInfo(input, output, inline_output);
    }
    if (nx2_caller) {
        // Switch 2 NvRm uses ioctls Switch 1 never had; answer them with a zeroed success so
        // device setup can continue, and log each one.
        RecordPlaceholderReply("nvhost-ctrl-gpu", command.raw, false, "Nx2UnknownIoctl");
        LOG_WARNING(Service_NVDRV, "(STUBBED) Switch 2 ioctl={:08X} group={:01X} command={:01X}",
                    command.raw, command.group, command.cmd);
        std::fill(output.begin(), output.end(), u8{0});
        std::fill(inline_output.begin(), inline_output.end(), u8{0});
        return NvResult::Success;
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}, group={:01X}, command={:01X}", command.raw,
                      command.group, command.cmd);
    return NvResult::NotImplemented;
}

// nvgpu GET_ENGINE_INFO as the Switch 2 NvRm uses it: args {u32 buf_size, u32 reserved,
// u64 buf_addr}, queried once for the size and once with an inline output buffer of 16-byte items
// {u32 engine_id, u32 engine_instance, s32 runlist_id, u32 reserved}. NvRm picks the channel
// runlist from this list. Report the graphics engine (runlist 0) and an async copy engine (1).
NvResult nvhost_ctrl_gpu::Nx2GetEngineInfo(std::span<const u8> input, std::span<u8> output,
                                           std::span<u8> inline_output) {
    struct Item {
        u32 engine_id;
        u32 engine_instance;
        s32 runlist_id;
        u32 reserved;
    };
    static constexpr std::array<Item, 2> items{{
        {0, 0, 0, 0}, // NVGPU_ENGINE_GR
        {2, 0, 1, 0}, // NVGPU_ENGINE_ASYNC_COPY
    }};
    constexpr u32 needed = static_cast<u32>(items.size() * sizeof(Item));

    u32 buf_size{};
    if (input.size() >= sizeof(buf_size)) {
        std::memcpy(&buf_size, input.data(), sizeof(buf_size));
    }
    LOG_WARNING(Service_NVDRV, "(STUBBED) Switch 2 GetEngineInfo buf_size={:#x} inline_out={:#x}",
                buf_size, inline_output.size());
    if (buf_size >= needed && inline_output.size() >= needed) {
        std::memcpy(inline_output.data(), items.data(), needed);
    }
    std::fill(output.begin(), output.end(), u8{0});
    std::copy_n(input.begin(), std::min(input.size(), output.size()), output.begin());
    if (output.size() >= sizeof(needed)) {
        std::memcpy(output.data(), &needed, sizeof(needed));
    }
    return NvResult::Success;
}

void nvhost_ctrl_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {}
void nvhost_ctrl_gpu::OnClose(DeviceFD fd) {}

namespace {

// Switch 2 SDKs ask for a 0xD0-byte characteristics block (Switch 1: 0xA0). Their NvRm only
// accepts chip ids 0x17E, 0x12B and 0x12E and its NVN path rejects the Maxwell ones, so report the
// Switch 2 Ampere chip. Fields past the Switch 1 layout are left zero. Values are best guesses.
constexpr u64 Nx2CharacteristicsSize = 0xD0;

// Switch 2's GPU (T239, GA10F): one GPC of six TPCs, two SMs each (12 SMs, 1536 CUDA cores).
constexpr u32 Nx2TpcPerGpc = 6;

// EDEN_NX2_GPU_CHARS=legacy keeps the pre-documentation Switch 2 block (Maxwell leftovers, 0x40 in
// every *_ioctl_nr_last); anything else reports the values documented on switchbrew NV_services
// ("[S2] Uses Ioctl3", gpu_characteristics). Read once.
bool Nx2LegacyCharacteristics() {
    static const bool legacy = [] {
        const char* value = std::getenv("EDEN_NX2_GPU_CHARS");
        return value != nullptr && std::strcmp(value, "legacy") == 0;
    }();
    return legacy;
}

// Documented s16 interface versions at 0x70..0x7E: gpu, tsg, dbg_gpu, channel, as, three unknown.
constexpr std::array<s16, 8> Nx2IoctlNrLast{0x1F, 0xF, 0x2B, 0x21, 0xD, -1, -1, -1};

template <typename GC>
void ApplyNx2Characteristics(GC& gc) {
    const bool legacy = Nx2LegacyCharacteristics();
    gc.arch = 0x170;               // Ampere
    gc.impl = 0xE;                 // chip id 0x17E as listed in the Switch 2 NvRm chip table
    gc.num_tpc_per_gpc = Nx2TpcPerGpc;
    gc.sm_arch_sm_version = 0x808; // NVN2 only accepts SM 0x808 (its class table lookup)
    gc.sm_arch_spa_version = legacy ? 0x808u : 0x806u;
    gc.threed_class = 0xC797;      // AMPERE_B
    gc.compute_class = 0xC7C0;     // AMPERE_COMPUTE_B
    gc.gpfifo_class = legacy ? 0xC56Fu : 0xC76Fu; // AMPERE_CHANNEL_GPFIFO_A / _B
    if (!legacy) {
        gc.l2_cache_size = 0x100000;
        gc.big_page_size = 0;              // no big pages: the guest asks for 4 KiB-only address spaces
        gc.compression_page_size = 0x10000;
        gc.pde_coverage_bit_count = 0x15;
        gc.available_big_page_sizes = 0;
        gc.sm_arch_warp_count = 0x60;
    }
    gc.dma_copy_class = 0xC7B5;    // AMPERE_DMA_COPY_B
    gc.chipname = legacy ? 0x6631306167ULL : 0x6630316167ULL; // "ga10f" (legacy value spells "ga01f")
    gc.flags |= 0x100;             // NVGPU_GPU_FLAGS_SUPPORT_TSG: NvRm binds channels to a TSG
    gc.flags |= 1ULL << 22;        // NVGPU_GPU_FLAGS_SUPPORT_TSG_SUBCONTEXTS (see below)
    gc.flags |= 1ULL << 26;        // NVGPU_GPU_FLAGS_SUPPORT_SYNCPOINT_ADDRESS: syncpoint shim
    // NVGPU_GPU_FLAGS_SUPPORT_MAPPING_MODIFY: NVN2 gives a texture in a compressible pool its
    // address only after NvRmGpuMappingModify switched the pool's mapping to the texture's kind.
    // Without the flag NvRm fails that call and the texture stays at GPU address 0 (title 1's
    // movie and the passes rendered into compressible pools never showed).
    gc.flags |= 1ULL << 34;
    // NVGPU_GPU_FLAGS_SUPPORT_REMAP: without it NvRm fails every remap (nvnMemoryPoolMapVirtual)
    // before asking the server, so title 1 never maps or unmaps its sparse texture tiles (nx2:gpu
    // 216) and streams its terrain into unmapped sparse memory. EDEN_NX2_SPARSE_REMAP=0 leaves it out.
    static const bool remap = [] {
        const char* value = std::getenv("EDEN_NX2_SPARSE_REMAP");
        return value == nullptr || value[0] != '0';
    }();
    if (remap) {
        gc.flags |= 1ULL << 35;
    }
    // Engine support bits 40-44 (2D, 3D, compute, inline-to-memory, ZBC). The SDK filters its class
    // enumeration by them, and NvRm's newer device path lists a class family in the device info
    // only when its bit is set; without them Fire Emblem: Fortune's Weave aborts at boot and a
    // device opened that way has no compute class (Fire Emblem's CUDA driver reports no device).
    gc.flags |= 0x1FULL << 40;

    if (legacy) {
        // The pre-documentation block: nine s16 of 0x40 at 0x70..0x80 (the Switch 1 layout's
        // max_fbps_count..max_tex_per_tpc bytes). With them NvRm enables every feature gate.
        auto* const raw = reinterpret_cast<u8*>(&gc);
        for (size_t off = 0x70; off <= 0x80; off += 2) {
            const s16 version = 0x40;
            std::memcpy(raw + off, &version, sizeof(version));
        }
    }
}

// Fields of the Switch 2 block past the Switch 1 layout (the guest's buffer is 0xD0 bytes; the
// IoctlGpuCharacteristics copy only covers the first 0xA0). NvRm sizes each TSG's subcontext (VEID)
// allocator from the u32 at 0xb4, but only when the TSG interface version (0x72) is above 0xa and
// flags bits 8 and 22 are set; otherwise every TSG gets one subcontext and a second queue's
// NvRmGpuTSGSubcontextCreate fails (0x11), leaving that NVN queue without command memory.
// Documented layout from 0x70 (packed): s16 nr_last[8], u32 max_fbps_count, fbp_en_mask,
// emc_en_mask, max_ltc_per_fbp, max_lts_per_ltc, max_tex_per_tpc, max_gpc_count,
// rop_l2_en_mask_DEPRECATED_0/1, u64 chipname at 0xa4, u32 unk0..unk8 at 0xac (unk2 = 0x40 = the
// subcontext count above).
void ApplyNx2CharacteristicsTail(std::span<u8> block) {
    constexpr size_t MaxSubcontextsOffset = 0xb4;
    constexpr u32 MaxSubcontexts = 64; // Ampere VEIDs per TSG
    const auto put = [&block](size_t offset, const auto value) {
        if (block.size() >= offset + sizeof(value)) {
            std::memcpy(block.data() + offset, &value, sizeof(value));
        }
    };
    if (!Nx2LegacyCharacteristics()) {
        constexpr size_t NrLastOffset = 0x70;
        if (block.size() >= Nx2CharacteristicsSize) {
            const auto documented = block.subspan(NrLastOffset, Nx2CharacteristicsSize - NrLastOffset);
            std::fill(documented.begin(), documented.end(), u8{0});
        }
        for (size_t i = 0; i < Nx2IoctlNrLast.size(); ++i) {
            put(NrLastOffset + i * sizeof(s16), Nx2IoctlNrLast[i]);
        }
        put(0x80, u32{0});  // max_fbps_count
        put(0x84, u32{1});  // fbp_en_mask
        put(0x88, u32{1});  // emc_en_mask
        put(0x8c, u32{1});  // max_ltc_per_fbp
        put(0x90, u32{4});  // max_lts_per_ltc
        put(0x94, u32{0});  // max_tex_per_tpc
        put(0x98, u32{1});  // max_gpc_count
        put(0xa4, u64{0x6630316167}); // chipname "ga10f"
        put(0xb0, u32{2});  // unk1
        put(0xb8, u32{3});  // unk3
        put(0xbc, u32{7});  // unk4
        put(0xc0, u32{1});  // unk5
        put(0xc4, u32{1});  // unk6
    }
    put(MaxSubcontextsOffset, MaxSubcontexts);
}

void LogNx2CharacteristicsOnce(bool legacy, u32 gpfifo_class, u64 l2_cache_size, u32 big_page_size,
                               u32 compression_page_size, u32 pde_coverage_bit_count,
                               u32 available_big_page_sizes, u32 warp_count, u32 spa_version) {
    static std::once_flag once;
    std::call_once(once, [&] {
        const s16 gpu = legacy ? s16{0x40} : Nx2IoctlNrLast[0];
        const s16 tsg = legacy ? s16{0x40} : Nx2IoctlNrLast[1];
        const s16 dbg = legacy ? s16{0x40} : Nx2IoctlNrLast[2];
        const s16 channel = legacy ? s16{0x40} : Nx2IoctlNrLast[3];
        const s16 as = legacy ? s16{0x40} : Nx2IoctlNrLast[4];
        LOG_INFO(Service_NVDRV,
                 "nx2 gpu_characteristics set={} gpfifo_class={:#x} l2_cache_size={:#x} "
                 "big_page_size={:#x} compression_page_size={:#x} pde_coverage_bit_count={:#x} "
                 "available_big_page_sizes={:#x} sm_arch_warp_count={:#x} sm_arch_spa_version={:#x} "
                 "nr_last gpu={:#x} tsg={:#x} dbg={:#x} channel={:#x} as={:#x} unk=({})",
                 legacy ? "legacy" : "documented", gpfifo_class, l2_cache_size, big_page_size,
                 compression_page_size, pde_coverage_bit_count, available_big_page_sizes,
                 warp_count, spa_version, gpu, tsg, dbg, channel, as,
                 legacy ? "0x40,0x40,0x40,0x40" : "0xffff,0xffff,0xffff");
    });
}

} // namespace

NvResult nvhost_ctrl_gpu::GetCharacteristics1(IoctlCharacteristics& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.gc.arch = 0x120;
    params.gc.impl = 0xb;
    params.gc.rev = 0xa1;
    params.gc.num_gpc = 0x1;
    params.gc.l2_cache_size = 0x40000;
    params.gc.on_board_video_memory_size = 0x0;
    params.gc.num_tpc_per_gpc = 0x2;
    params.gc.bus_type = 0x20;
    params.gc.big_page_size = 0x20000;
    params.gc.compression_page_size = 0x20000;
    params.gc.pde_coverage_bit_count = 0x1B;
    params.gc.available_big_page_sizes = 0x30000;
    params.gc.gpc_mask = 0x1;
    params.gc.sm_arch_sm_version = 0x503;
    params.gc.sm_arch_spa_version = 0x503;
    params.gc.sm_arch_warp_count = 0x80;
    params.gc.gpu_va_bit_count = 0x28;
    params.gc.reserved = 0x0;
    params.gc.flags = 0x55;
    params.gc.twod_class = 0x902D;
    params.gc.threed_class = 0xB197;
    params.gc.compute_class = 0xB1C0;
    params.gc.gpfifo_class = 0xB06F;
    params.gc.inline_to_memory_class = 0xA140;
    params.gc.dma_copy_class = 0xB0B5;
    params.gc.max_fbps_count = 0x1;
    params.gc.fbp_en_mask = 0x0;
    params.gc.max_ltc_per_fbp = 0x2;
    params.gc.max_lts_per_ltc = 0x1;
    params.gc.max_tex_per_tpc = 0x0;
    params.gc.max_gpc_count = 0x1;
    params.gc.rop_l2_en_mask_0 = 0x21D70;
    params.gc.rop_l2_en_mask_1 = 0x0;
    params.gc.chipname = 0x6230326D67;
    params.gc.gr_compbit_store_base_hw = 0x0;
    params.gpu_characteristics_buf_size = 0xA0;
    params.gpu_characteristics_buf_addr = 0xdeadbeef; // Cannot be 0 (UNUSED)
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetCharacteristics3(
    IoctlCharacteristics& params, std::span<IoctlGpuCharacteristics> gpu_characteristics) {
    LOG_DEBUG(Service_NVDRV, "called, buf_size={:#x}", params.gpu_characteristics_buf_size);
    const bool nx2 = params.gpu_characteristics_buf_size >= Nx2CharacteristicsSize;

    params.gc.arch = 0x120;
    params.gc.impl = 0xb;
    params.gc.rev = 0xa1;
    params.gc.num_gpc = 0x1;
    params.gc.l2_cache_size = 0x40000;
    params.gc.on_board_video_memory_size = 0x0;
    params.gc.num_tpc_per_gpc = 0x2;
    params.gc.bus_type = 0x20;
    params.gc.big_page_size = 0x20000;
    params.gc.compression_page_size = 0x20000;
    params.gc.pde_coverage_bit_count = 0x1B;
    params.gc.available_big_page_sizes = 0x30000;
    params.gc.gpc_mask = 0x1;
    params.gc.sm_arch_sm_version = 0x503;
    params.gc.sm_arch_spa_version = 0x503;
    params.gc.sm_arch_warp_count = 0x80;
    params.gc.gpu_va_bit_count = 0x28;
    params.gc.reserved = 0x0;
    params.gc.flags = 0x55;
    params.gc.twod_class = 0x902D;
    params.gc.threed_class = 0xB197;
    params.gc.compute_class = 0xB1C0;
    params.gc.gpfifo_class = 0xB06F;
    params.gc.inline_to_memory_class = 0xA140;
    params.gc.dma_copy_class = 0xB0B5;
    params.gc.max_fbps_count = 0x1;
    params.gc.fbp_en_mask = 0x0;
    params.gc.max_ltc_per_fbp = 0x2;
    params.gc.max_lts_per_ltc = 0x1;
    params.gc.max_tex_per_tpc = 0x0;
    params.gc.max_gpc_count = 0x1;
    params.gc.rop_l2_en_mask_0 = 0x21D70;
    params.gc.rop_l2_en_mask_1 = 0x0;
    params.gc.chipname = 0x6230326D67;
    params.gc.gr_compbit_store_base_hw = 0x0;
    params.gpu_characteristics_buf_size = 0xA0;
    params.gpu_characteristics_buf_addr = 0xdeadbeef; // Cannot be 0 (UNUSED)
    if (nx2) {
        nx2_caller = true;
        g_nx2_driver = true;
        Tegra::g_nx2_ampere = true;
        ApplyNx2Characteristics(params.gc);
        params.gpu_characteristics_buf_size = Nx2CharacteristicsSize;
        LOG_WARNING(Service_NVDRV, "Switch 2 caller, reporting Ampere chip 0x17E");
        LogNx2CharacteristicsOnce(Nx2LegacyCharacteristics(), params.gc.gpfifo_class,
                                  params.gc.l2_cache_size, params.gc.big_page_size,
                                  params.gc.compression_page_size,
                                  params.gc.pde_coverage_bit_count,
                                  params.gc.available_big_page_sizes,
                                  params.gc.sm_arch_warp_count, params.gc.sm_arch_spa_version);
    }
    if (!gpu_characteristics.empty()) {
        gpu_characteristics.front() = params.gc;
    }
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetTPCMasks1(IoctlGpuGetTpcMasksArgs& params) {
    LOG_DEBUG(Service_NVDRV, "called, mask_buffer_size={:#x}", params.mask_buffer_size);
    if (params.mask_buffer_size != 0) {
        params.tcp_mask = 3;
    }
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetTPCMasks3(IoctlGpuGetTpcMasksArgs& params, std::span<u32> tpc_mask) {
    LOG_DEBUG(Service_NVDRV, "called, mask_buffer_size={:#x}", params.mask_buffer_size);
    if (params.mask_buffer_size != 0) {
        // One bit per TPC of the Switch 2 block's num_tpc_per_gpc (the Switch 1 mask of 3 left
        // two of the six).
        params.tcp_mask = nx2_caller ? (1u << Nx2TpcPerGpc) - 1 : 3;
    }
    if (!tpc_mask.empty()) {
        tpc_mask.front() = params.tcp_mask;
    }
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetActiveSlotMask(IoctlActiveSlotMask& params) {
    LOG_DEBUG(Service_NVDRV, "called");

    params.slot = 0x07;
    params.mask = 0x01;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::PmuGetGpuLoad(IoctlPmuGetLoad& params) {
    LOG_WARNING(Service_NVDRV, "(stubbed) called");
    params.pmu_gpu_load = 100;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::ZCullGetCtxSize(IoctlZcullGetCtxSize& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.size = 0x1;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::ZCullGetInfo(IoctlNvgpuGpuZcullGetInfoArgs& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.width_align_pixels = 0x20;
    params.height_align_pixels = 0x20;
    params.pixel_squares_by_aliquots = 0x400;
    params.aliquot_total = 0x800;
    params.region_byte_multiplier = 0x20;
    params.region_header_size = 0x20;
    params.subregion_header_size = 0xc0;
    params.subregion_width_align_pixels = 0x20;
    params.subregion_height_align_pixels = 0x40;
    params.subregion_count = 0x10;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::ZBCSetTable(IoctlZbcSetTable& params) {
    if (params.type > supported_types) {
        LOG_ERROR(Service_NVDRV, "ZBCSetTable: invalid type {:#x}", params.type);
        return NvResult::BadParameter;
    }

    std::scoped_lock lk(zbc_mutex);

    switch (static_cast<ZBCTypes>(params.type)) {
        case ZBCTypes::color: {
            ZbcColorEntry color_entry{};
            std::copy_n(std::begin(params.color_ds), color_entry.color_ds.size(), color_entry.color_ds.begin());
            std::copy_n(std::begin(params.color_l2), color_entry.color_l2.size(), color_entry.color_l2.begin());
            color_entry.format = params.format;
            color_entry.ref_cnt = 1u;

            auto color_it = std::ranges::find_if(zbc_colors,
                                                 [&](const ZbcColorEntry& color_in_question) {
                                                     return color_entry.format == color_in_question.format &&
                                                            color_entry.color_ds == color_in_question.color_ds &&
                                                            color_entry.color_l2 == color_in_question.color_l2;
                                                 });

            if (color_it != zbc_colors.end()) {
                ++color_it->ref_cnt;
                LOG_DEBUG(Service_NVDRV, "ZBCSetTable: reused color entry fmt={:#x}, ref_cnt={:#x}",
                          params.format, color_it->ref_cnt);
            } else {
                zbc_colors.push_back(color_entry);
                LOG_DEBUG(Service_NVDRV, "ZBCSetTable: added color entry fmt={:#x}, index={:#x}",
                          params.format, zbc_colors.size() - 1);
            }
            break;
        }
        case ZBCTypes::depth: {
            ZbcDepthEntry depth_entry{params.depth, params.format, 1u};

            auto depth_it = std::ranges::find_if(zbc_depths,
                                                 [&](const ZbcDepthEntry& depth_entry_in_question) {
                                                     return depth_entry.format == depth_entry_in_question.format &&
                                                            depth_entry.depth == depth_entry_in_question.depth;
                                                 });

            if (depth_it != zbc_depths.end()) {
                ++depth_it->ref_cnt;
                LOG_DEBUG(Service_NVDRV, "ZBCSetTable: reused depth entry fmt={:#x}, ref_cnt={:#x}",
                          depth_entry.format, depth_it->ref_cnt);
            } else {
                zbc_depths.push_back(depth_entry);
                LOG_DEBUG(Service_NVDRV, "ZBCSetTable: added depth entry fmt={:#x}, index={:#x}",
                          depth_entry.format, zbc_depths.size() - 1);
            }
        }
    }

    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::ZBCQueryTable(IoctlZbcQueryTable& params) {
    if (params.type > supported_types) {
        LOG_ERROR(Service_NVDRV, "ZBCQueryTable: invalid type {:#x}", params.type);
        return NvResult::BadParameter;
    }

    std::scoped_lock lk(zbc_mutex);

    // Switch 2 NvRm walks the tables from index 1 (entry 0 stands for "not ZBC'd"); with unsigned
    // wrap-around an index below `first` fails the size check like one past the end.
    const u32 first = nx2_caller ? 1u : 0u;

    switch (static_cast<ZBCTypes>(params.type)) {
        case ZBCTypes::color: {
            const u32 slot = params.index_size - first;
            if (slot >= zbc_colors.size()) {
                LOG_ERROR(Service_NVDRV, "ZBCQueryTable: invalid color index {:#x}", params.index_size);
                return NvResult::BadParameter;
            }

            const auto& colors = zbc_colors[slot];
            std::copy_n(colors.color_ds.begin(), colors.color_ds.size(), std::begin(params.color_ds));
            std::copy_n(colors.color_l2.begin(), colors.color_l2.size(), std::begin(params.color_l2));
            params.depth = 0;
            params.ref_cnt = colors.ref_cnt;
            params.format = colors.format;
            params.index_size = static_cast<u32>(zbc_colors.size());
            break;
        }
        case ZBCTypes::depth: {
            const u32 slot = params.index_size - first;
            if (slot >= zbc_depths.size()) {
                LOG_ERROR(Service_NVDRV, "ZBCQueryTable: invalid depth index {:#x}", params.index_size);
                return NvResult::BadParameter;
            }

            const auto& depth_entry = zbc_depths[slot];
            std::fill(std::begin(params.color_ds), std::end(params.color_ds), 0);
            std::fill(std::begin(params.color_l2), std::end(params.color_l2), 0);
            params.depth = depth_entry.depth;
            params.ref_cnt = depth_entry.ref_cnt;
            params.format = depth_entry.format;
            params.index_size = static_cast<u32>(zbc_depths.size());
        }
    }

    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::FlushL2(IoctlFlushL2& params) {
    LOG_DEBUG(Service_NVDRV, "called {:#x}", params.flush);
    // if ((params.flush & 0x01) != 0) //l2 flush
    //     /* we dont emulate l2 */;
    // if ((params.flush & 0x04) != 0) //fb flush
    //     /* we dont emulate fb */;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetGpuTime(IoctlGetGpuTime& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.gpu_time = static_cast<u64_le>(system.CoreTiming().GetGlobalTimeNs().count());
    return NvResult::Success;
}

Kernel::KEvent* nvhost_ctrl_gpu::QueryEvent(u32 event_id) {
    switch (event_id) {
    case 1:
        return error_notifier_event;
    case 2:
        return unknown_event;
    default:
        LOG_CRITICAL(Service_NVDRV, "Unknown Ctrl GPU Event {}", event_id);
        return nullptr;
    }
}

} // namespace Service::Nvidia::Devices
