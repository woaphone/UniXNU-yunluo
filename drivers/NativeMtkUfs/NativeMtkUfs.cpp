// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// MediaTek MT6789 (Xiaomi "yunluo") UFS host driver.
//
// Originally implemented from the local UFS_MT6789_PORT_SPEC and vendor
// register definitions. See README.md for the recovered source provenance,
// tested baseline and the untested-on-device READ recovery addition.
// native_mtkufs_stage names the last completed bring-up stage.
//
// This driver is deliberately polled: no interrupt handler is registered and
// all completion waits are bounded by mach_absolute_time()/clock_interval_to_deadline().
// OS media is read-only. Data/Log have bounded LU2 write paths; this driver
// must not be described as an entirely read-only controller.
//
// Stage map (native_mtkufs_stage names the last completed stage):
//   1 provider matched            6 HCE + UIC interrupts enabled
//   2 MMIO mapped                 7 link startup + UIC 0x16 completed
//   3 clocks enabled              8 device/geometry/unit descriptors read
//   4 reference clock requested   9 UTP read of LBA 0 succeeded
//   5 device + host reset done   10 media published
//
// Evidence/unknown markers use the same grades as the spec: "S" source-confirmed,
// "U" unknown, "B" blocker.  Every value not directly given by the spec is a
// clearly marked TODO rather than an invented constant.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/task.h>
#include "ReadRecovery.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-volatile"
#pragma clang diagnostic ignored "-Wignored-attributes"
#pragma clang diagnostic ignored "-Wextern-c-compat"
extern "C" {
#define BSD_KERNEL_PRIVATE 1
#include <sys/proc_internal.h>
#include <sys/user.h>
#include <sys/attr.h>
#include <sys/vnode.h>
#include <sys/conf.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <miscfs/devfs/devfs.h>
#undef BSD_KERNEL_PRIVATE
}
#pragma clang diagnostic pop

/* yunluo bring-up: pid 1 syscall trace exported from bsd/dev/arm/systemcalls.c. */
extern "C" {
extern volatile uint32_t yunluo_sc_enter;extern volatile uint32_t yunluo_sc_return;
extern volatile uint32_t yunluo_sc_code;
extern volatile uint64_t yunluo_sc_arg0;
extern volatile uint64_t yunluo_sc_arg1;
extern volatile uint64_t yunluo_sc_arg2;
extern volatile uint64_t yunluo_sc_arg3;
extern volatile uint64_t yunluo_sc_arg4;
extern volatile uint64_t yunluo_sc_arg5;
extern volatile char yunluo_write_first[128];
extern volatile char yunluo_write_last[128];
extern volatile char yunluo_write_ring[16][192];
extern volatile uint32_t yunluo_write_ridx;
extern volatile char yunluo_sysctl_str[64];
// Written by this driver once the Cryptex1,SystemOS volume is attached: the
// BSD device name ("disk2s1") the kernel mounts before exec'ing launchd.
char yunluo_cryptex_dev[32];
volatile int yunluo_cryptex_dev_ready;
// Bumped by the secondary (cryptex) instance's bdevsw entry points.  They stay
// zero if APFS never probed the volume at all, which is a different failure
// from probing and rejecting it.
extern volatile uint32_t yunluo_cryptex_opens;
extern volatile uint32_t yunluo_cryptex_ioctls;
extern volatile uint32_t yunluo_cryptex_reads;
extern volatile uint32_t yunluo_cryptex_badcmd;
extern volatile int yunluo_preboot_status;
extern volatile int yunluo_exec_stage;
extern volatile int yunluo_exec_errno;
extern volatile int yunluo_bind_err;
extern volatile int yunluo_cache_visible;
extern char yunluo_cache_magic[17];
extern volatile int yunluo_cache_read_err;
extern char yunluo_preboot_log[192];
extern volatile uint32_t yunluo_sysctl_idx;
extern volatile int32_t yunluo_sysctl_kind[16];
extern volatile int32_t yunluo_sysctl_n[16][4];
extern volatile uint32_t yunluo_sysctl_len[16];
extern volatile int32_t yunluo_sysctl_error[16];
extern volatile char yunluo_sysctl_path[16][48];
extern volatile uint32_t yunluo_write_count;
extern volatile int32_t yunluo_sysctl_name[4];
extern volatile uint32_t yunluo_sysctl_count;
extern volatile uint32_t yunluo_trace_idx;
extern volatile uint32_t yunluo_trace_code[48];
extern volatile uint64_t yunluo_trace_a0[48];
extern volatile uint64_t yunluo_trace_a1[48];
extern volatile uint64_t yunluo_trace_a2[48];
extern const char *volatile yunluo_trace_name[48];
extern volatile char yunluo_arg_str[80];
}

extern "C" {
// Exported for the boot-page / post-mortem inspector.  native_mtkufs_sectors is
// the media capacity in 512-byte sectors (the convention used by the existing
// NativeEmmc and NativeVirtioBlock drivers); native_mtkufs_reads counts
// successfully completed UTP READ commands.
volatile uint32_t native_mtkufs_stage = 0;
volatile uint32_t native_mtkufs_probe_calls = 0;
volatile uint32_t native_mtkufs_matched = 0;
volatile uint32_t native_mtkufs_uic_result = 0;
volatile uint32_t native_mtkufs_uic_step = 0;   /* 1 wait-ready, 2 issued, 3 done, 4 error, 5 timeout */
volatile uint32_t native_mtkufs_uic_opcode = 0;
volatile uint32_t native_mtkufs_uic_status = 0;
volatile uint32_t native_mtkufs_pwr_step = 0;   /* bit0 host power ok, bit1 device power ok */
volatile uint32_t native_mtkufs_refclk_ctrl = 0;
volatile uint32_t native_mtkufs_snap_media = 0;
volatile uint32_t native_mtkufs_snap_uuid = 0;
volatile uint32_t native_mtkufs_snap_guid = 0;
volatile uint32_t native_mtkufs_snap_apfs = 0;
volatile uint32_t native_mtkufs_snap_flags = 0;
volatile uint32_t native_mtkufs_utp_errors = 0;
volatile uint32_t native_mtkufs_reads = 0;
volatile uint32_t native_mtkufs_writes = 0;
volatile uint32_t native_mtkufs_write_errors = 0;
volatile uint32_t native_mtkufs_write_refused = 0;
volatile uint32_t native_mtkufs_syncs = 0;
volatile uint32_t native_mtkufs_hs_rejected = 0;
// Outcome of yunluo_panic_flush(); see the marker struct for the values.
volatile uint32_t yunluo_panic_flush_state = 0;
volatile uint64_t native_mtkufs_sectors = 0;
volatile uint32_t native_mtkufs_version = 0;
volatile uint32_t native_mtkufs_hcs = 0;
volatile uint32_t native_mtkufs_hcap = 0;
volatile uint32_t native_mtkufs_uic_errors[5] = {};
volatile uint32_t native_mtkufs_block_requests = 0;
}

namespace {

// ---------------------------------------------------------------------------
// Physical windows (spec §1.2, §2).  Only these two windows are mapped.
// ---------------------------------------------------------------------------
constexpr uint64_t kUfsBase = 0x11270000ULL;     // UFSHCI host
constexpr IOByteCount kUfsWindow = 0x2300;
// The task specifies a single 0x2000 window at 0x10000000.  It covers
// TOPCKGEN at 0x10000000..0x10000FFF and INFRACFG_AO at 0x10001000..0x10001FFF;
// every offset below is relative to 0x10000000.
constexpr uint64_t kClockBase = 0x10000000ULL;
constexpr IOByteCount kClockWindow = 0x2000;

// Standard UFSHCI registers (spec §4.1).
constexpr unsigned kCcap = 0x00;
constexpr unsigned kVersion = 0x08;
constexpr unsigned kAhit = 0x18;
constexpr unsigned kIs = 0x20;
constexpr unsigned kIe = 0x24;
constexpr unsigned kHcs = 0x30;
constexpr unsigned kHce = 0x34;
constexpr unsigned kUecpa = 0x38;
constexpr unsigned kUecdl = 0x3C;
constexpr unsigned kUecn = 0x40;
constexpr unsigned kUect = 0x44;
constexpr unsigned kUecdme = 0x48;
constexpr unsigned kUtrlBaseLo = 0x50;
constexpr unsigned kUtrlBaseHi = 0x54;
constexpr unsigned kUtrlDoorbell = 0x58;
constexpr unsigned kUtrlRunStop = 0x60;
constexpr unsigned kUtmrBaseLo = 0x70;
constexpr unsigned kUtmrBaseHi = 0x74;
constexpr unsigned kUtmrRunStop = 0x80;
constexpr unsigned kUicCmd = 0x90;
constexpr unsigned kUicArg1 = 0x94;
constexpr unsigned kUicArg2 = 0x98;
constexpr unsigned kUicArg3 = 0x9C;

// MediaTek vendor registers (spec §1.3, §7).
constexpr unsigned kXoufsCtrl = 0x140;
constexpr unsigned kRefClkCtrl = 0x144;
constexpr unsigned kMtkHwVer = 0x2240;

// CCAP fields (spec §1.8).
constexpr uint32_t kCcapNutrs = 0x1FU;
constexpr uint32_t kCcapNutmrs = 0x7U;
constexpr uint32_t kCcapDma64 = 1U << 24;

// HCS fields (spec §4.1).
constexpr uint32_t kHcsDevicePresent = 1U << 0;
constexpr uint32_t kHcsUtrlReady = 1U << 1;
constexpr uint32_t kHcsUtmrReady = 1U << 2;
constexpr uint32_t kHcsUicReady = 1U << 3;
constexpr uint32_t kHcsListsReady = kHcsUtrlReady | kHcsUtmrReady | kHcsUicReady;

// IS bits (spec §4.5).
constexpr uint32_t kIsUtpComplete = 1U << 0;
constexpr uint32_t kIsUicError = 1U << 2;
constexpr uint32_t kIsUicComplete = 1U << 10;
constexpr uint32_t kIsDeviceFatal = 1U << 11;
constexpr uint32_t kIsControllerFatal = 1U << 16;
constexpr uint32_t kIsSysbusFatal = 1U << 17;
constexpr uint32_t kIsCryptoFatal = 1U << 18;
constexpr uint32_t kIsFatalMask = kIsDeviceFatal | kIsControllerFatal |
                                   kIsSysbusFatal | kIsCryptoFatal;
constexpr uint32_t kIsUicEventBits = kIsUicError | (1U << 4) | (1U << 5) |
                                     (1U << 6) | kIsUicComplete;
constexpr uint32_t kUicMask = 0x470;  // UFSHCD_UIC_MASK (spec §4.5)

// XOUFS_CTRL and REFCLK_CTRL bits (spec §1.3).
constexpr uint32_t kXoufsCtrlHcePre = 1U << 7;
constexpr uint32_t kRefClkRequest = 1U << 0;
constexpr uint32_t kRefClkAck = 1U << 1;

// TOPCKGEN offsets relative to 0x10000000 (spec §2.3).
constexpr unsigned kTopCfgUpdate1 = 0x0008;
constexpr unsigned kTopCfg12 = 0x00D0;
constexpr unsigned kTopCfg12Clr = 0x00D8;
constexpr uint32_t kTopMuxGateUfs = 1U << 31;
constexpr uint32_t kTopMuxGateAes = 1U << 23;
constexpr uint32_t kTopMuxUpdateUfs = 1U << 20;
constexpr uint32_t kTopMuxUpdateAes = 1U << 19;

// INFRACFG_AO offsets relative to 0x10000000 (spec §2.4, §2.5).
constexpr unsigned kInfraGateClr = 0x10A8;
constexpr unsigned kInfraGateSta = 0x10AC;
constexpr uint32_t kInfraUfsGates = (1U << 28) | (1U << 29) | (1U << 13) |
                                    (1U << 12) | (1U << 11);  // 0x30003800
constexpr unsigned kHciRstAssert = 0x1130;
constexpr unsigned kHciRstDeassert = 0x1134;
constexpr unsigned kUniproRstAssert = 0x1140;
constexpr unsigned kUniproRstDeassert = 0x1144;
constexpr unsigned kCryptoRstAssert = 0x1150;
constexpr unsigned kCryptoRstDeassert = 0x1154;
constexpr uint32_t kHciResetBit = 1U << 15;
constexpr uint32_t kUniproResetBit = 1U << 7;
constexpr uint32_t kCryptoResetBit = 1U << 21;

// MediaTek SiP / SMCCC (spec §5.2).  a0 = function ID, a1 = command bitmask,
// arguments start at a2.
constexpr uint64_t kMtkSipFunction = 0xC2000276ULL;
constexpr uint64_t kSipDeviceReset = 1ULL << 1;             // 0x02
constexpr uint64_t kSipRefClkNotification = 1ULL << 3;      // 0x08
constexpr uint64_t kSipHostPwrCtrl = 1ULL << 5;             // 0x20
constexpr uint64_t kSipDevicePwrCtrl = 1ULL << 7;           // 0x80
// The whole media is the APFS container window inside super (LU2, 4096-byte LBA).
constexpr uint64_t kSuperWindowBaseLba = 0x48800ULL;

// iOS 17 keeps the system dylibs and the dyld shared cache in the
// Cryptex1,SystemOS APFS container. On this port it is written to the start of
// `userdata` (factory scatter: 0x2BC000000; 806400 x 4096 = 3,303,014,400 B,
// volume UUID 8846FE78-2910-4297-AF97-D59BB6F8FC16). Publishing it as its own
// block device lets the apfs kext attach a second container.
constexpr uint64_t kCryptexWindowBaseLba = 0x2BC000ULL;
constexpr uint64_t kCryptexWindowBlocks = 806400ULL;
constexpr char kCryptexVolumeUUID[] = "8846FE78-2910-4297-AF97-D59BB6F8FC16";
constexpr uint64_t kSuperWindowBytes = 0x220000000ULL;

// UPIU / UTP constants (spec §4.2..§4.4, §4.8).
constexpr uint32_t kUpiuCommand = 0x01;
constexpr uint32_t kUpiuNopOut = 0x00;
constexpr uint32_t kUpiuQueryRequest = 0x16;
constexpr uint32_t kUpiuNopIn = 0x20;
constexpr uint32_t kUpiuQueryResponse = 0x36;
constexpr uint32_t kUpiuQueryFuncRead = 0x01;
constexpr uint32_t kUpiuQueryFuncWrite = 0x81;
constexpr uint32_t kUpiuCmdFlagsRead = 0x40;
// UPIU command flags, ufs.h: UPIU_CMD_FLAGS_WRITE = 0x20, UPIU_CMD_FLAGS_READ = 0x40.
constexpr uint32_t kUpiuCmdFlagsWrite = 0x20;

constexpr uint32_t kQueryReadDesc = 0x01;
constexpr uint32_t kQueryReadFlag = 0x05;
constexpr uint32_t kQuerySetFlag = 0x06;
constexpr uint32_t kFlagDeviceInit = 0x01;  // QUERY_FLAG_IDN_FDEVICEINIT

constexpr uint8_t kDescDevice = 0x00;
constexpr uint8_t kDescUnit = 0x02;
constexpr uint8_t kDescGeometry = 0x07;
// Factory scatter's UFS section: LU0/LU1 are boot regions, LU2 holds the GPT.
constexpr uint8_t kUserLun = 2;

constexpr uint32_t kUicDmeGet = 0x01;
constexpr uint32_t kUicDmeSet = 0x02;
constexpr uint32_t kUicLinkStartup = 0x16;

constexpr uint32_t kUtpNoData = 0x00000000;
constexpr uint32_t kUtpDeviceToHost = 0x04000000;
// ufshci.h: UTP_HOST_TO_DEVICE = 0x02000000, the WRITE direction.
constexpr uint32_t kUtpHostToDevice = 0x02000000;
constexpr uint32_t kUtpReqDescIntCmd = 0x01000000;
constexpr uint32_t kOcsInvalid = 0x0F;
constexpr uint32_t kOcsMask = 0x0F;

constexpr uint16_t kPaLocalVerInfo = 0x15A9;
constexpr uint16_t kVsUniProPowerDownControl = 0xD0A8;
constexpr uint16_t kPaLocalTxLccEnable = 0x155E;
// Unipro power-mode attributes (vendor drivers/scsi/ufs/unipro.h).
constexpr uint16_t kPaActiveTxDataLanes = 0x1560;
constexpr uint16_t kPaActiveRxDataLanes = 0x1580;
constexpr uint16_t kPaTxGear = 0x1568;
constexpr uint16_t kPaRxGear = 0x1583;
constexpr uint16_t kPaTxTermination = 0x1569;
constexpr uint16_t kPaRxTermination = 0x1584;
constexpr uint16_t kPaHsSeries = 0x156A;
constexpr uint16_t kPaPowerMode = 0x1571;
constexpr uint16_t kPaPowerModeUserData0 = 0x15B0;
// Defaults from unipro.h (DL_*TimeOutVal_Default).  Linux sets six of them
// (PA_PWRMODEUSERDATA0..5) before a gear change; an incomplete set leaves the
// link parameters inconsistent and the switch is not taken.
constexpr uint32_t kDlFc0ProtectionTimeout = 8191;
constexpr uint32_t kDlTc0ReplayTimeout = 65535;
constexpr uint32_t kDlAfc0ReqTimeout = 32767;
constexpr uint32_t kDlFc1ProtectionTimeout = 8191;
constexpr uint32_t kDlTc1ReplayTimeout = 65535;
constexpr uint32_t kDlAfc1ReqTimeout = 32767;
// DME local copies of the same three timeouts (unipro.h: DME_Local*, 0xD041..).
constexpr uint16_t kDmeLocalFc0ProtectionTimeout = 0xD041;
constexpr uint16_t kDmeLocalTc0ReplayTimeout = 0xD042;
constexpr uint16_t kDmeLocalAfc0ReqTimeout = 0xD043;
constexpr uint32_t kFastMode = 1;
constexpr uint32_t kHsModeA = 1;
// HCS[10:8] is UPMCRS, the power-mode-change result.  PWR_OK is the only value
// that means the link actually moved; PWR_LOCAL means "requested, not done".
constexpr uint32_t kHcsUpmcrsShift = 8;
constexpr uint32_t kHcsUpmcrsMask = 0x7;
constexpr uint32_t kPwrOk = 0x0;
constexpr uint32_t kPwrLocal = 0x1;
constexpr uint16_t kVsSavePowerControl = 0xD0A6;
constexpr uint16_t kVsDebugClockEnable = 0xD0A1;

// Host memory layout (spec §3.4, §4.3, §4.4).
constexpr unsigned kTrdOffset = 0;
constexpr unsigned kTrdSize = 32;
constexpr unsigned kUtmrOffset = 1024;     // nutrs <= 32 => TRD array is <= 1024 bytes
constexpr unsigned kUcdOffset = 2048;      // 1024-aligned, clear of the UTMRD array
constexpr unsigned kUcdPayload = 1024;     // command UPIU (512) + response UPIU (512)
constexpr unsigned kUcdPrdOffset = 1024;   // PRDT follows the response UPIU
constexpr unsigned kUcdDirtyBytes = kUcdPayload + 16;  // headers plus one PRDT entry
constexpr unsigned kUcdStride = 2048;
constexpr unsigned kUpiuResponseOffset = 512;
constexpr unsigned kUpiuResponseHeader = 32;  // GENERAL_UPIU_REQUEST_SIZE
constexpr unsigned kUpiuSizeDwords = kUpiuResponseOffset / 4;   // 128
constexpr unsigned kUpiuOffsetDwords = kUpiuResponseOffset / 4; // 128
constexpr unsigned kPrdOffsetDwords = kUcdPrdOffset / 4;        // 256

constexpr uint32_t kDataBytes = 128 * 1024;  // one PRDT entry, well under 256 KiB
[[maybe_unused]] constexpr uint32_t kBlockSize512 = 512;

// Descriptor offsets (vendor include/scsi/ufs/ufs.h, spec §3.8).
constexpr unsigned kDeviceDescNumLu = 0x06;
constexpr unsigned kDeviceDescSpecVersion = 0x10;
constexpr unsigned kDeviceDescManufacturer = 0x18;
constexpr unsigned kDeviceDescQueueDepth = 0x21;
constexpr unsigned kGeometryDescCapacity = 0x04;  // qTotalRawDeviceCapacity, 512-byte units
constexpr unsigned kGeometryDescMaxLu = 0x0C;
constexpr unsigned kGeometryDescMinBlock = 0x12;
constexpr unsigned kUnitDescBlockSize = 0x0A;     // bLogicalBlockSize (power of two)
constexpr unsigned kUnitDescBlockCount = 0x0B;    // qLogicalBlockCount, big endian
constexpr unsigned kUnitDescType = 0x01;
constexpr unsigned kUnitDescIndex = 0x02;
constexpr unsigned kUnitDescEnable = 0x03;
constexpr unsigned kUnitDescBootLun = 0x04;
constexpr unsigned kUnitDescWriteProtect = 0x05;  // bLUWriteProtect
constexpr unsigned kUnitDescQueueDepth = 0x06;    // bLUQueueDepth

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
inline uint32_t readRegister(uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

inline void writeRegister(uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

inline void dmaBarrier()
{
  __asm__ volatile("dsb sy" ::: "memory");
}

// The DMA buffers are ordinary cached kernel memory (the MTK DT node has no
// "dma-coherent"), so descriptors are cleaned before the doorbell and device
// results are invalidated afterwards.
void cleanDcache(const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t>(start) & ~static_cast<uintptr_t>(63);
  uintptr_t end = reinterpret_cast<uintptr_t>(start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

void invalidateDcache(const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t>(start) & ~static_cast<uintptr_t>(63);
  uintptr_t end = reinterpret_cast<uintptr_t>(start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

uint64_t deadlineAfterUs(uint32_t microseconds)
{
  uint64_t deadline = 0;
  clock_interval_to_deadline(microseconds, kMicrosecondScale, &deadline);
  return deadline;
}

uint64_t deadlineAfterMs(uint32_t milliseconds)
{
  uint64_t deadline = 0;
  clock_interval_to_deadline(milliseconds, kMillisecondScale, &deadline);
  return deadline;
}

inline bool
isUsableUserUnitDescriptor(const uint8_t *unit)
{
  return unit[kUnitDescType] == kDescUnit &&
         unit[kUnitDescIndex] == kUserLun &&
         unit[kUnitDescEnable] != 0 &&
         unit[kUnitDescBootLun] == 0;
}

uint16_t getBe16(const uint8_t *at)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(at[0]) << 8) | at[1]);
}

uint32_t getBe32(const uint8_t *at)
{
  return (static_cast<uint32_t>(at[0]) << 24) |
         (static_cast<uint32_t>(at[1]) << 16) |
         (static_cast<uint32_t>(at[2]) << 8) | at[3];
}

uint64_t getBe64(const uint8_t *at)
{
  return (static_cast<uint64_t>(getBe32(at)) << 32) | getBe32(at + 4);
}

uint32_t getLe32(const uint8_t *at)
{
  return static_cast<uint32_t>(at[0]) |
         (static_cast<uint32_t>(at[1]) << 8) |
         (static_cast<uint32_t>(at[2]) << 16) |
         (static_cast<uint32_t>(at[3]) << 24);
}

uint64_t getLe64(const uint8_t *at)
{
  return static_cast<uint64_t>(getLe32(at)) |
         (static_cast<uint64_t>(getLe32(at + 4)) << 32);
}

void putLe32(uint8_t *at, uint32_t value)
{
  at[0] = static_cast<uint8_t>(value);
  at[1] = static_cast<uint8_t>(value >> 8);
  at[2] = static_cast<uint8_t>(value >> 16);
  at[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t gptCrc32(const uint8_t *data, uint32_t length)
{
  uint32_t crc = 0xFFFFFFFFU;
  while (length--) {
    crc ^= *data++;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

void putBe32(uint8_t *at, uint32_t value)
{
  at[0] = static_cast<uint8_t>(value >> 24);
  at[1] = static_cast<uint8_t>(value >> 16);
  at[2] = static_cast<uint8_t>(value >> 8);
  at[3] = static_cast<uint8_t>(value);
}

// SMCCC call with the arm64 convention: x0 is both the function selector and the
// return value; x1..x7 carry the arguments.  This is the same shape as the
// kernel's arm_smccc_smc(); no Linux framework is used.
static inline uint64_t mtkSmccc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
  register uint64_t x0 __asm__("x0") = a0;
  register uint64_t x1 __asm__("x1") = a1;
  register uint64_t x2 __asm__("x2") = a2;
  register uint64_t x3 __asm__("x3") = a3;
  register uint64_t x4 __asm__("x4") = a4;
  register uint64_t x5 __asm__("x5") = a5;
  register uint64_t x6 __asm__("x6") = a6;
  register uint64_t x7 __asm__("x7") = a7;
  __asm__ volatile("smc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x6), "r"(x7)
                   : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                     "x16", "x17", "memory");
  return x0;
}

// MediaTek SiP entry point: a0 = 0xC2000276, a1 = command bit, a2/a3 = args.
uint64_t mtkSip(uint64_t command, uint64_t arg0, uint64_t arg1)
{
  return mtkSmccc(kMtkSipFunction, command, arg0, arg1, 0, 0, 0, 0);
}

[[maybe_unused]] bool compatibleContains(IOService *provider, const char *wanted)
{
  OSData *data = OSDynamicCast(OSData, provider->getProperty("compatible"));
  if (!data) {
    return false;
  }
  const char *text = static_cast<const char *>(data->getBytesNoCopy());
  const unsigned length = data->getLength();
  unsigned wantedLength = 0;
  while (wanted[wantedLength]) {
    ++wantedLength;
  }
  if (!text || length < wantedLength) {
    return false;
  }
  // The provider property may be one NUL-terminated string or a list; compare
  // every byte offset so both forms match without assuming alignment.
  for (unsigned offset = 0; offset + wantedLength <= length; ++offset) {
    if (!memcmp(text + offset, wanted, wantedLength)) {
      return true;
    }
  }
  return false;
}

}  // namespace

class NativeMtkUfs : public IOService {
  OSDeclareDefaultStructors(NativeMtkUfs)

  IOMemoryMap *ufsMapping = nullptr;
  IOMemoryMap *clockMapping = nullptr;
  IOBufferMemoryDescriptor *hostMemory = nullptr;
  IOBufferMemoryDescriptor *dataMemory = nullptr;
  IOLock *gate = nullptr;

  uintptr_t ufs = 0;
  uintptr_t clock = 0;

  uint8_t *hostBytesPtr = nullptr;
  uint64_t hostPhysical = 0;
  uint32_t hostBytes = 0;
  uint8_t *dataBytesPtr = nullptr;
  uint64_t dataPhysical = 0;

  uint32_t nutrs = 0;
  uint32_t nutmrs = 0;
  uint64_t mediaSectors = 0;     // 512-byte units
  uint32_t deviceBlockSize = 0;  // UFS logical block size from the unit descriptor
  uint64_t deviceBlocks = 0;     // blocks in this instance's published window
  uint64_t deviceCapacityBlocks = 0;  // blocks in the whole LUN (bounds for reads)
  // bLUWriteProtect of the selected LU.  Non-zero means the device rejects
  // WRITE(10) with sense data, so the expdb log writer must not even try.
  uint8_t unitWriteProtect = 0xFF;
  // Media block i lives at LU2 LBA windowBaseLba + i.  The primary instance
  // serves the `super` window (the OS root APFS container); the secondary one
  // serves the Cryptex1,SystemOS container written to userdata.
  uint64_t windowBaseLba = kSuperWindowBaseLba;
  bool secondaryWindow = false;
  IOService *ufsProvider = nullptr;
  uint32_t hwMajor = 2;
  bool utpReady = false;
  bool readRecoveryEnabled = false;
  bool lastReadTransportFailure = false;
  unsigned readRecoveryAttempts = 0;
  bool recoverReadTransport();
  bool readDeviceBlocksRecovering(uint64_t lba, uint32_t blocks, void *out);
  bool online = false;
  uint8_t lastScsiSenseKey = 0xFF;
  bool installBlockMediaDispatch();
  bool prepareApfsCompatibility();
  unsigned setApfsMediaProperties();
  uint64_t windowBytes() const {
    return mediaSectors * 512ULL;
  }

  void patchMediaSectors(uint64_t sector, uint32_t count, void *out) const;

  uint32_t readUfs(unsigned offset) const;
  void writeUfs(unsigned offset, uint32_t value);
  uint32_t readClock(unsigned offset) const;
  void writeClock(unsigned offset, uint32_t value);

  bool waitUfsMask(unsigned offset, uint32_t mask, uint32_t value,
                   uint32_t timeoutUs, uint32_t stepUs);
  bool waitClockMask(unsigned offset, uint32_t mask, uint32_t value,
                     uint32_t timeoutUs, uint32_t stepUs);

  bool mapRegisters();
  void readCapabilities();
  bool enableClocks();
  bool requestReferenceClock();
  bool resetDeviceAndHost();
  bool allocateHostMemory();
  bool enableHostController();
  bool enableController();
  bool linkStartup();
  bool preLink();
  void postLink();
  bool makeOperational();
  bool gearUpHighSpeed();

  bool uicCommand(uint8_t opcode, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                  uint32_t *result, uint32_t *value);
  bool dmeSet(uint16_t idn, uint8_t selector, uint32_t value);
  bool dmeGet(uint16_t idn, uint8_t selector, uint32_t *value);
  void drainUicInterrupts();

  bool runUtp(unsigned tag, uint32_t commandType, uint32_t direction,
              uint32_t prdBytes, uint32_t timeoutMs, uint32_t *ocs);
  bool sendNop();
  bool sendQuery(bool write, uint8_t opcode, uint8_t idn, uint8_t index,
                 uint8_t selector, uint16_t length, uint32_t value,
                 uint32_t *responseValue, uint8_t *descriptor,
                 uint32_t descriptorMax, uint32_t *descriptorLength);
  bool readDescriptor(uint8_t idn, uint8_t index, uint8_t *out,
                      uint32_t outMax, uint32_t *outLength);
  bool readDescriptors();
  bool proveLbaZero();
  bool readDeviceBlocks(uint64_t lba, uint32_t blocks, void *out);
  bool readDeviceBlocksLocked(uint64_t lba, uint32_t blocks, void *out);
  bool writeDeviceBlocks(uint64_t lba, uint32_t blocks, const void *in);
  bool writeDeviceBlocksLocked(uint64_t lba, uint32_t blocks, const void *in);
  IOReturn readSectors(uint64_t sector, uint32_t count, void *out);

  uint8_t *utrdAt(unsigned tag) const;
  uint8_t *ucdAt(unsigned tag) const;
  uint64_t ucdPhysical(unsigned tag) const;
  uint32_t utpCommandType(bool deviceManagement) const;

public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  void free() override;
  unsigned retryApfsMatch() { return setApfsMediaProperties(); }
  void kickApfsMatch();
  bool readWindowBlock(uint64_t lba, void *out) { return readDeviceBlocks(lba, 1, out); }
  bool publishSecondaryWindow();
  void dumpGptEntries();
  // expdb console log window, bounded to [kLogWindowFirstLba, kLogWindowLastLba].
  // Public because the native_ufs_log_* C entry points drive them from outside
  // the class.
  bool logWindowReady() const { return utpReady && deviceBlockSize == 4096; }
  bool logWindowWriteProtected() const { return unitWriteProtect != 0; }
  bool logWriteBlocks(uint64_t lba, uint32_t blocks, const void *in);
  bool logWriteSnapshotLocked(uint64_t *outTotal);
  bool logWritePanicMarkerLocked(uint32_t state);
  bool syncDeviceCache();
  bool syncDeviceCacheLocked();
  // fb143: the on-disk Data store hooks (free functions below) need raw block
  // access; readDeviceBlocks/writeDeviceBlocks themselves are protected.
  bool storeReadBlocks(uint64_t lba, uint32_t blocks, void *out) { return readDeviceBlocks(lba, blocks, out); }
  bool storeWriteBlocks(uint64_t lba, uint32_t blocks, const void *in) { return writeDeviceBlocks(lba, blocks, in); }
  IOReturn doEjectMedia() { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const { return 0; }
  char *getVendorString() { return const_cast<char *>("MediaTek"); }
  char *getProductString() { return const_cast<char *>("UFS LU2 read-only"); }
  char *getRevisionString() { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() {
    return const_cast<char *>("MT6789 UFS, polled UIC/UTP");
  }
  IOReturn reportBlockSize(UInt64 *value) {
    *value = deviceBlockSize;
    return kIOReturnSuccess;
  }
  IOReturn reportEjectability(bool *value) {
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportMaxValidBlock(UInt64 *value) {
    if (!deviceBlocks) {
      return kIOReturnNotReady;
    }
    *value = deviceBlocks - 1;
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState(bool *present, bool *changed) {
    *present = online;
    if (changed) {
      *changed = false;
    }
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) {
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportWriteProtection(bool *value) {
    *value = true;
    return kIOReturnSuccess;
  }
  IOReturn doSynchronizeCache() {
    return online ? kIOReturnSuccess : kIOReturnNotReady;
  }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) {
    return doSynchronizeCache();
  }
  IOReturn getWriteCacheState(bool *value) {
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn setWriteCacheState(bool) { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
                            UInt64 count, IOStorageAttributes *,
                            IOStorageCompletion *completion);
};

OSDefineMetaClassAndStructors(NativeMtkUfs, IOService)

namespace {

NativeMtkUfs *gBlockMedia;
[[maybe_unused]] NativeMtkUfs *gCryptexMedia;
bool gCryptexPublished = false;
// Both published windows share one UTP tag and one bounce buffer, so all
// device reads are serialized through this lock.
IOLock *gUtpGate = nullptr;

/* UTP gate observability + writer handoff.  The data-path lock is not fair:
 * a daemon-spawn read storm can starve the log writer's blocking acquisition
 * forever (observed on fb135: rounds froze at 11 while the console ran on). */
volatile uint64_t gUtpGateOwner = 0;        /* thread id inside the gate */
volatile uint64_t gUtpGateSince = 0;        /* mach_absolute_time at acquire */
volatile uint32_t gLogWritePending = 0;     /* writer wants the gate now */
char gLogNote[96] = {0};

/* fb146 read insurance: every media chunk is re-read and compared; a block
 * that disagrees gets a third read and majority vote.  The observed failure
 * (fb145/expdbxnu21: ONE bad page-in of a shared framework -> dozens of
 * daemons crash-loop on the cached zero page; sampling 1/32 missed it) is
 * converted into a logged, corrected event.  yunluo-read-verify=0 disables. */
static uint8_t gVerifyExpect[4096];
static uint8_t gVerifyChunk[128 * 1024];
static uint64_t gVerifyChecked = 0;
static uint64_t gVerifyMismatch = 0;
static uint64_t gVerifyFixed = 0;

static bool readVerifyEnabled()
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 1;
    if (!PE_parse_boot_argn("yunluo-read-verify", &value, sizeof(value)) || value > 1) {
      value = 1;
    }
    cached = static_cast<int>(value);
  }
  return cached != 0;
}

static void utpGateLockTracked(void)
{
  /* Let a pending snapshot write go first: it is a few blocks, while the
   * read storm behind us is minutes.  Bounded, so a stuck flag cannot stop
   * the storage path outright. */
  for (int spin = 0; spin < 400 && gLogWritePending; spin++) {
    IOSleep(10);
  }
  IOLockLock(gUtpGate);
  gUtpGateSince = mach_absolute_time();
  gUtpGateOwner = thread_tid(current_thread());
}

static void utpGateUnlockTracked(void)
{
  gUtpGateOwner = 0;
  IOLockUnlock(gUtpGate);
}

static uint64_t utpGateHeldMs(void)
{
  uint64_t since = gUtpGateSince;
  if (!gUtpGateOwner || !since) {
    return 0;
  }
  uint64_t now = mach_absolute_time();
  if (now <= since) {
    return 0;
  }
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(now - since, &ns);
  return ns / 1000000ULL;
}


// ---------------------------------------------------------------------------
// expdb console log window
// ---------------------------------------------------------------------------
// The stock scatter (MT6789_Android_scatter.xml, UFS_LU2) puts expdb at byte
// 0x488000 = LBA 0x488 with 128 MiB and is_download=false, so a flash never
// touches it and a log survives reflashing boot.img.  Its tail is where MTK
// keeps the live last_kmsg/crash dump, so the window starts 16 MiB in and stops
// 32 MiB short of the end:
//
//   [0x488 .. 0x1488)   first 16 MiB   left alone
//   [0x1488 .. 0x6488)  80 MiB         this log ring (16384 blocks of 4096)
//   [0x6488 .. 0x8488)  last 32 MiB    MTK's own data, left alone
//
// Block 0 of the window is a header written *after* the payload blocks, so a
// reader never trusts bytes the header does not count yet (the same ordering
// kits/yunluo/BootLog.c uses on the sheng port).
constexpr uint64_t kLogWindowFirstLba = 0x1488ULL;
constexpr uint64_t kLogWindowBlocks = 20480ULL;   // 80 MiB at 4096 B
// A write is refused above this so a bug cannot spill past the window into
// expdb's tail or into the neighbouring `frp` partition at LBA 0x8488.
constexpr uint64_t kLogWindowLastLba =
    kLogWindowFirstLba + kLogWindowBlocks - 1ULL;  // 0x6487

uint64_t gLogFirstLba = 0;
uint64_t gLogBlocks = 0;
bool gLogOpen = false;
volatile uint32_t native_mtkufs_log_open_attempts = 0;
volatile uint32_t native_mtkufs_log_writes = 0;
volatile uint32_t native_mtkufs_log_errors = 0;
const OSMetaClass *gBlockMediaMeta;
uintptr_t gBlockMediaDispatch[190];
uint8_t gGptHeader[4096];
uint8_t gGptEntries[65536];
uint64_t gGptEntriesLba;
uint32_t gGptEntriesBytes;
uint32_t gGptEntriesBlocks;
bool gGptCompatReady;

constexpr uint8_t kAppleApfsType[16] = {
  0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11,
  0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC
};

// Two instances share this dispatch table, so every call must use the device
// I/O Kit passed in (the provider of the storage driver), not a global.
NativeMtkUfs *blockDevice(IOService *service) {
  return static_cast<NativeMtkUfs *>(service);
}

IOReturn blockEjectMedia(IOService *) { return kIOReturnUnsupported; }
IOReturn blockFormatMedia(IOService *, UInt64) { return kIOReturnNotWritable; }
UInt32 blockFormatCapacities(const IOService *, UInt64 *, UInt32) { return 0; }
IOReturn blockSynchronizeCache(IOService *service) { return blockDevice(service)->doSynchronizeCache(); }
char *blockVendorString(IOService *service) { return blockDevice(service)->getVendorString(); }
char *blockProductString(IOService *service) { return blockDevice(service)->getProductString(); }
char *blockRevisionString(IOService *service) { return blockDevice(service)->getRevisionString(); }
char *blockAdditionalInfo(IOService *service) { return blockDevice(service)->getAdditionalDeviceInfoString(); }
IOReturn blockReportBlockSize(IOService *service, UInt64 *value) {
  return blockDevice(service)->reportBlockSize(value);
}
IOReturn blockReportEjectability(IOService *service, bool *value) {
  return blockDevice(service)->reportEjectability(value);
}
IOReturn blockReportMaxValidBlock(IOService *service, UInt64 *value) {
  return blockDevice(service)->reportMaxValidBlock(value);
}
IOReturn blockReportMediaState(IOService *service, bool *present, bool *changed) {
  return blockDevice(service)->reportMediaState(present, changed);
}
IOReturn blockReportRemovability(IOService *service, bool *value) {
  return blockDevice(service)->reportRemovability(value);
}
IOReturn blockReportWriteProtection(IOService *service, bool *value) {
  return blockDevice(service)->reportWriteProtection(value);
}
IOReturn blockGetWriteCacheState(IOService *service, bool *value) {
  return blockDevice(service)->getWriteCacheState(value);
}
IOReturn blockSetWriteCacheState(IOService *service, bool value) {
  return blockDevice(service)->setWriteCacheState(value);
}
IOReturn blockAsyncReadWrite(IOService *service, IOMemoryDescriptor *buffer,
                             UInt64 block, UInt64 count,
                             IOStorageAttributes *attributes,
                             IOStorageCompletion *completion) {
  const uint32_t request =
      __atomic_fetch_add(&native_mtkufs_block_requests, 1, __ATOMIC_RELAXED) + 1;
  if (request <= 24) {
    kprintf("NativeMtkUfs: block request %u lba=%llu blocks=%llu\n",
            request, static_cast<unsigned long long>(block),
            static_cast<unsigned long long>(count));
  }
  return blockDevice(service)->doAsyncReadWrite(buffer, block, count, attributes, completion);
}
IOReturn blockRequestIdle(IOService *) { return kIOReturnUnsupported; }
IOReturn blockUnmap(IOService *, IOBlockStorageDeviceExtent *, UInt32, IOStorageUnmapOptions) {
  return kIOReturnUnsupported;
}
IOReturn blockSetPriority(IOService *, IOBlockStorageDeviceExtent *, UInt32,
                          IOStoragePriority) {
  return kIOReturnUnsupported;
}
IOReturn blockSynchronize(IOService *service, UInt64, UInt64, IOStorageSynchronizeOptions) {
  return blockDevice(service)->doSynchronizeCache();
}
IOReturn blockGetProvisionStatus(IOService *, UInt64, UInt64, UInt32 *,
                                 IOBlockStorageProvisionDeviceExtent *,
                                 IOStorageGetProvisionStatusOptions) {
  return kIOReturnUnsupported;
}

#define NATIVE_PROC_ALLPROCLIST (1U << 0)

extern "C" {
// Does the mounted root actually carry this path?  The dyld failure ("no such
// file" for /usr/lib/libSystem.B.dylib) may mean the root volume is missing
// the system tree, or that the shared cache is absent; both are vnode lookups.
}

// Report what the IOKit registry actually holds for the cryptex window.  The
// counters alone cannot tell "the media was never matched" apart from "APFS
// looked and refused", and those need completely different fixes.
void probeCryptexMediaRegistry(unsigned round);

void mediaSnapshotThread(void *, wait_result_t)
{
  // One compact line per round: no probes, no IOKit matching, no device I/O.
  // Those take VFS/registry locks and have wedged the system during mount.
  for (unsigned round = 1; round <= 60; ++round) {
    IOSleep(5000);
    kprintf("NativeMtkUfs: r=%u preboot=%d exec=%d/%d bind=%d vis=%d "
            "reads=%u errs=%u breq=%u\n",
            round, yunluo_preboot_status, yunluo_exec_stage, yunluo_exec_errno,
            yunluo_bind_err, yunluo_cache_visible,
            native_mtkufs_reads, native_mtkufs_utp_errors,
            native_mtkufs_block_requests);
    // Did APFS ever reach the cryptex window?  opens/ioctls/reads are bumped
    // only by the secondary instance's own bdevsw entry points, so a row of
    // zeros means the volume was never probed, not that probing failed.
    kprintf("NativeMtkUfs: r=%u cryptex open=%u ioctl=%u read=%u badcmd=%u dev='%s'\n",
            round, yunluo_cryptex_opens, yunluo_cryptex_ioctls,
            yunluo_cryptex_reads, yunluo_cryptex_badcmd, yunluo_cryptex_dev);
    // The decisive one while cryptex does not attach: what does the registry
    // actually hold for that window?  Counts every IOMedia whose Size matches
    // the secondary window, how many carry the APFS content hint, and how many
    // have an AppleAPFS* client attached.
    if (round <= 6) {
      probeCryptexMediaRegistry(round);
    }
  }
}

// Walk every IOMedia and report the ones that could be the cryptex container.
// Prints one line per round: how many media have the cryptex window's Size, how
// many of those carry the APFS content hint, and how many have an AppleAPFS*
// client attached.  That separates the two failure modes:
//   size=1 hint=0 -> our property patch never stuck, so APFS has no reason to
//                    look (fix: setApfsMediaProperties / kickApfsMatch)
//   size=1 hint=1 client=0 -> APFS looked and did not attach (fix: apfs side)
void probeCryptexMediaRegistry(unsigned round)
{
  const uint64_t wantSize = (kCryptexWindowBlocks * 4096ULL);
  OSDictionary *m = IOService::serviceMatching("IOMedia");
  if (!m) {
    return;
  }
  OSIterator *it = IOService::getMatchingServices(m);
  m->release();
  if (!it) {
    return;
  }
  unsigned sizeMatch = 0, hintMatch = 0, clientMatch = 0;
  char clientName[40] = "";
  while (OSObject *o = it->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, o);
    if (!media) {
      continue;
    }
    OSNumber *size = OSDynamicCast(OSNumber, media->getProperty("Size"));
    if (!size || size->unsigned64BitValue() != wantSize) {
      continue;
    }
    sizeMatch++;
    OSString *hint = OSDynamicCast(OSString, media->getProperty("Content Hint"));
    if (hint && strcmp(hint->getCStringNoCopy(),
                       "7C3457EF-0000-11AA-AA11-00306543ECAC") == 0) {
      hintMatch++;
    }
    const OSSymbol *cat = OSSymbol::withCString("IOStorage");
    if (cat) {
      IOService *holder = media->copyClientWithCategory(cat);
      if (holder) {
        clientMatch++;
        if (!clientName[0] && holder->getMetaClass()) {
          const char *cn = holder->getMetaClass()->getClassName();
          unsigned k = 0;
          for (; cn && cn[k] && k < sizeof(clientName) - 1; ++k) {
            clientName[k] = cn[k];
          }
          clientName[k] = 0;
        }
        holder->release();
      }
      cat->release();
    }
  }
  it->release();
  kprintf("NativeMtkUfs: r=%u cryptex registry size=%u hint=%u client=%u '%s'\n",
          round, sizeMatch, hintMatch, clientMatch, clientName);
}

}  // namespace

// ---------------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------------
uint32_t NativeMtkUfs::readUfs(unsigned offset) const
{
  return readRegister(ufs, offset);
}

void NativeMtkUfs::writeUfs(unsigned offset, uint32_t value)
{
  writeRegister(ufs, offset, value);
  dmaBarrier();
}

uint32_t NativeMtkUfs::readClock(unsigned offset) const
{
  return readRegister(clock, offset);
}

void NativeMtkUfs::writeClock(unsigned offset, uint32_t value)
{
  writeRegister(clock, offset, value);
  dmaBarrier();
}

bool NativeMtkUfs::waitUfsMask(unsigned offset, uint32_t mask, uint32_t value,
                               uint32_t timeoutUs, uint32_t stepUs)
{
  const uint64_t deadline = deadlineAfterUs(timeoutUs);
  for (;;) {
    if ((readUfs(offset) & mask) == value) {
      return true;
    }
    if (mach_absolute_time() >= deadline) {
      return false;
    }
    IODelay(stepUs);
  }
}

bool NativeMtkUfs::waitClockMask(unsigned offset, uint32_t mask, uint32_t value,
                                 uint32_t timeoutUs, uint32_t stepUs)
{
  const uint64_t deadline = deadlineAfterUs(timeoutUs);
  for (;;) {
    if ((readClock(offset) & mask) == value) {
      return true;
    }
    if (mach_absolute_time() >= deadline) {
      return false;
    }
    IODelay(stepUs);
  }
}

uint8_t *NativeMtkUfs::utrdAt(unsigned tag) const
{
  return hostBytesPtr + kTrdOffset + tag * kTrdSize;
}

uint8_t *NativeMtkUfs::ucdAt(unsigned tag) const
{
  return hostBytesPtr + kUcdOffset + tag * kUcdStride;
}

uint64_t NativeMtkUfs::ucdPhysical(unsigned tag) const
{
  return hostPhysical + kUcdOffset + tag * kUcdStride;
}

// The vendor driver uses UTRD command type 1 (UFS storage) for both SCSI and
// device-management commands once UFSHCI is newer than 1.1; types 0/2 are only
// used with HCI <= 1.1 (spec §4.3).  REG_UFS_VERSION is read at runtime.
uint32_t NativeMtkUfs::utpCommandType(bool deviceManagement) const
{
  if (native_mtkufs_version > 0x110U) {
    return 1U;  // UTP_CMD_TYPE_UFS_STORAGE
  }
  return deviceManagement ? 2U : 0U;
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------
IOService *NativeMtkUfs::probe(IOService *provider, SInt32 *score)
{
	native_mtkufs_probe_calls = native_mtkufs_probe_calls + 1;
	(void)provider;
	native_mtkufs_matched = 1;
	if (score) {
		*score = 1000;
	}
	/* Attached to IOResources/IOBSD so the console already exists. */
	kprintf("NativeMtkUfs: stage 1 probe reached\n");
	return this;
}

// ---------------------------------------------------------------------------
// Stage 2: map the two windows and read the controller identity
// ---------------------------------------------------------------------------
bool NativeMtkUfs::mapRegisters()
{
  IODeviceMemory *ufsMemory = IODeviceMemory::withRange(kUfsBase, kUfsWindow);
  IODeviceMemory *clockMemory = IODeviceMemory::withRange(kClockBase, kClockWindow);
  if (!ufsMemory || !clockMemory) {
    OSSafeReleaseNULL(clockMemory);
    OSSafeReleaseNULL(ufsMemory);
    kprintf("NativeMtkUfs: IODeviceMemory::withRange failed\n");
    return false;
  }
  ufsMapping = ufsMemory->map(kIOMapAnywhere | kIOMapInhibitCache);
  clockMapping = clockMemory->map(kIOMapAnywhere | kIOMapInhibitCache);
  ufsMemory->release();
  clockMemory->release();
  if (!ufsMapping || !clockMapping) {
    kprintf("NativeMtkUfs: MMIO map failed (ufs=%p clock=%p)\n",
            static_cast<void *>(ufsMapping), static_cast<void *>(clockMapping));
    return false;
  }
  ufs = static_cast<uintptr_t>(ufsMapping->getVirtualAddress());
  clock = static_cast<uintptr_t>(clockMapping->getVirtualAddress());
  native_mtkufs_stage = 2;  // stage 2: MMIO mapped
  kprintf("NativeMtkUfs: stage 2 mapped UFS 0x%llx at %p, clock/reset 0x%llx at %p\n",
          static_cast<unsigned long long>(kUfsBase), reinterpret_cast<void *>(ufs),
          static_cast<unsigned long long>(kClockBase), reinterpret_cast<void *>(clock));
  return true;
}

void NativeMtkUfs::readCapabilities()
{
  const uint32_t cap = readUfs(kCcap);
  const uint32_t version = readUfs(kVersion);
  const uint32_t hcs = readUfs(kHcs);
  const uint32_t hwVersion = readUfs(kMtkHwVer);

  native_mtkufs_hcap = cap;
  native_mtkufs_version = version;
  native_mtkufs_hcs = hcs;
  nutrs = (cap & kCcapNutrs) + 1U;
  nutmrs = ((cap >> 16) & kCcapNutmrs) + 1U;

  kprintf("NativeMtkUfs: UFSHCI version=0x%04x CCAP=0x%08x HCS=0x%08x MTK_HW_VER=0x%08x "
          "nutrs=%u nutmrs=%u dma64=%u\n",
          version, cap, hcs, hwVersion, nutrs, nutmrs,
          (cap & kCcapDma64) ? 1U : 0U);
  // TODO(bring-up): CCAP, REG_UFS_VERSION and REG_UFS_MTK_HW_VER are all "U" in
  // the spec (§1.8, §6 #1); only the values above are authoritative.  The debug
  // mux writes at 0x22C0/0x22D0..0x22DC (spec §3.10) are intentionally omitted:
  // they are diagnostic only and not needed to reach stage 9.
}

// ---------------------------------------------------------------------------
// Stage 3: raw clock-gate writes
// ---------------------------------------------------------------------------
bool NativeMtkUfs::enableClocks()
{
  const uint32_t configuration = readClock(kTopCfg12);
  const uint32_t ufsParent = (configuration >> 24) & 0x7U;
  const uint32_t aesParent = (configuration >> 16) & 0x7U;

  // TODO(spec §2.6, §6 #2): the bootloader-selected mux parents and the state of
  // the selected PLL are unknown.  Do not change the parent and do not touch the
  // PLLs; only the two mux gates are enabled.  If a cold boot leaves the selected
  // PLL off, this gate-only sequence is insufficient.
  writeClock(kTopCfg12Clr, kTopMuxGateUfs | kTopMuxGateAes);
  writeClock(kTopCfgUpdate1, kTopMuxUpdateUfs | kTopMuxUpdateAes);
  const bool muxGatesOk = waitClockMask(kTopCfg12, kTopMuxGateUfs | kTopMuxGateAes,
                                        0, 10000, 10);

  // Five INFRACFG_AO gates: ifrao_ufs(28), ifrao_u_aes(29), UFS_SAP_BCLK(13),
  // unipro_tick(12), unipro_sysclk(11).  Enable is a write-1-to-CLR (spec §2.4).
  writeClock(kInfraGateClr, kInfraUfsGates);
  const bool gatesOk = waitClockMask(kInfraGateSta, kInfraUfsGates, 0, 10000, 10);

  kprintf("NativeMtkUfs: stage 3 clocks: ufs_sel=%u aes_ufsfde_sel=%u "
          "mux-gates=%s infra-gates=%s (CLK_CFG_12=0x%08x STA=0x%08x)\n",
          ufsParent, aesParent, muxGatesOk ? "ok" : "not-clear",
          gatesOk ? "ok" : "not-clear", readClock(kTopCfg12),
          readClock(kInfraGateSta));
  if (ufsParent == 7 || aesParent == 7) {
    // Index 7 is reserved in both parent tables (spec §1.4).  Do not program a
    // replacement parent from here; report it so a later stage can derive the
    // correct PLL/mux setup.
    kprintf("NativeMtkUfs: WARNING reserved TOPCKGEN parent index (ufs=%u aes=%u)\n",
            ufsParent, aesParent);
  }
  // TODO(spec §2.4, §6 #10): no settling time is documented.  The status poll is
  // advisory; a gate that still reports gated is logged but does not stop
  // bring-up, because the gate status semantics itself is not hardware-tested.
  native_mtkufs_stage = 3;  // stage 3: clocks enabled
  return true;
}

// ---------------------------------------------------------------------------
// Stage 4: reference clock request (SiP notify + REFCLK_CTRL handshake)
// ---------------------------------------------------------------------------
bool NativeMtkUfs::requestReferenceClock()
{
  const uint64_t smcResult = mtkSip(kSipRefClkNotification, 1, 0);
  if (smcResult != 0) {
    // TODO(spec §5.2, §6 #4): the EL3 implementation of 0xC2000276 is not in the
    // tree.  The kernel side of the vendor driver does not check this return
    // value either; the ACK handshake below is the real gate.
    kprintf("NativeMtkUfs: REF_CLK_NOTIFICATION SMC returned 0x%llx (EL3 support unknown)\n",
            static_cast<unsigned long long>(smcResult));
  }

  writeUfs(kRefClkCtrl, readUfs(kRefClkCtrl) | kRefClkRequest);
  if (!waitUfsMask(kRefClkCtrl, kRefClkRequest | kRefClkAck,
                   kRefClkRequest | kRefClkAck, 3000, 100)) {
    kprintf("NativeMtkUfs: stage 4 reference clock ACK timeout REFCLK_CTRL=0x%08x\n",
            readUfs(kRefClkCtrl));
    return false;
  }
  // TODO(spec §3.10): ref_clk_ungating_wait_us is selected by the device vendor
  // quirk and is not in this tree.  Use zero until measured on hardware.
  native_mtkufs_refclk_ctrl = readUfs(kRefClkCtrl);
  native_mtkufs_stage = 4;  // stage 4: reference clock requested
  kprintf("NativeMtkUfs: stage 4 reference clock requested/acked (SMC=0x%llx ctrl=0x%08x)\n",
          static_cast<unsigned long long>(smcResult), readUfs(kRefClkCtrl));
  return true;
}

// ---------------------------------------------------------------------------
// Stage 5: device reset pulse + host resets
// ---------------------------------------------------------------------------
bool NativeMtkUfs::resetDeviceAndHost()
{
  // M-PHY / device power lives behind EL3 on this design (no phys node in the
  // device tree): issue both SiP power controls before resetting the device.
  const uint64_t hostPwr = mtkSip(kSipHostPwrCtrl, 1, 0);
  const uint64_t devPwr = mtkSip(kSipDevicePwrCtrl, 1, 0);
  native_mtkufs_pwr_step = ((hostPwr == 0) ? 1U : 0U) | ((devPwr == 0) ? 2U : 0U);
  kprintf("NativeMtkUfs: SiP power host=0x%llx device=0x%llx\n",
          static_cast<unsigned long long>(hostPwr),
          static_cast<unsigned long long>(devPwr));

  // Stop the host first (spec §3.5 step 1).
  writeUfs(kHce, 0);
  if (!waitUfsMask(kHce, 1, 0, 100000, 100)) {
    kprintf("NativeMtkUfs: stage 5 HBA did not stop (HCE=0x%08x)\n", readUfs(kHce));
    return false;
  }

  // SiP DEVICE_RESET: reset is active low; 10 us low, then 10 ms settle
  // (spec §3.5).  The vendor driver does not check res.a0; we log it.
  const uint64_t lowResult = mtkSip(kSipDeviceReset, 0, 0);
  IODelay(10);
  const uint64_t highResult = mtkSip(kSipDeviceReset, 1, 0);
  IOSleep(100);
  kprintf("NativeMtkUfs: device reset SMC low=0x%llx high=0x%llx\n",
          static_cast<unsigned long long>(lowResult),
          static_cast<unsigned long long>(highResult));
  // TODO(spec §5.2, §6 #4/#11): EL3 may not implement DEVICE_RESET at all.  The
  // boot chain normally leaves the UFS device powered, so continue with the host
  // reset and let link startup report the truth.

  // Host resets: assert hci, crypto, unipro; 100 us; deassert in reverse order
  // (spec §2.5).  STATUS_NONE on this SoC, so there is nothing to poll.
  writeClock(kHciRstAssert, kHciResetBit);
  writeClock(kCryptoRstAssert, kCryptoResetBit);
  writeClock(kUniproRstAssert, kUniproResetBit);
  IODelay(100);
  writeClock(kUniproRstDeassert, kUniproResetBit);
  writeClock(kCryptoRstDeassert, kCryptoResetBit);
  writeClock(kHciRstDeassert, kHciResetBit);

  native_mtkufs_stage = 5;  // stage 5: device reset + host reset done
  kprintf("NativeMtkUfs: stage 5 device+host resets done\n");
  return true;
}

// ---------------------------------------------------------------------------
// Stage 6: host memory layout + HCE + UIC interrupts
// ---------------------------------------------------------------------------
bool NativeMtkUfs::allocateHostMemory()
{
  if (nutrs < 2 || nutrs > 32 || nutmrs < 1 || nutmrs > 8) {
    kprintf("NativeMtkUfs: unusable queue geometry nutrs=%u nutmrs=%u\n", nutrs, nutmrs);
    return false;
  }

  // TODO(blocker): no IOMMU/SMMU programming is performed.  This driver assumes
  // the UFS host DMA reaches the physical addresses returned by
  // getPhysicalSegment() (identity/pass-through).  If MT6789 routes UFS through
  // an SMMU, DMA will fault here; the port spec does not describe such a window.
  hostBytes = (kUcdOffset + nutrs * kUcdStride + 4095U) & ~4095U;
  hostMemory = IOBufferMemoryDescriptor::inTaskWithOptions(
      kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut) |
          static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
      hostBytes, 4096);
  dataMemory = IOBufferMemoryDescriptor::inTaskWithOptions(
      kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut) |
          static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
      kDataBytes, 4096);
  if (!hostMemory || !dataMemory) {
    kprintf("NativeMtkUfs: DMA buffer allocation failed\n");
    return false;
  }

  IOByteCount hostLength = 0;
  IOByteCount dataLength = 0;
  hostPhysical = hostMemory->getPhysicalSegment(0, &hostLength);
  dataPhysical = dataMemory->getPhysicalSegment(0, &dataLength);
  hostBytesPtr = static_cast<uint8_t *>(hostMemory->getBytesNoCopy());
  dataBytesPtr = static_cast<uint8_t *>(dataMemory->getBytesNoCopy());
  if (!hostBytesPtr || !dataBytesPtr || !hostPhysical || !dataPhysical ||
      hostLength < hostBytes || dataLength < kDataBytes ||
      (hostPhysical & 4095U) || (dataPhysical & 4095U)) {
    kprintf("NativeMtkUfs: DMA buffer unusable host=%p/0x%llx/%llu data=%p/0x%llx/%llu\n",
            static_cast<void *>(hostBytesPtr),
            static_cast<unsigned long long>(hostPhysical),
            static_cast<unsigned long long>(hostLength),
            static_cast<void *>(dataBytesPtr),
            static_cast<unsigned long long>(dataPhysical),
            static_cast<unsigned long long>(dataLength));
    return false;
  }
  if (!(native_mtkufs_hcap & kCcapDma64) &&
      (hostPhysical + hostBytes > 0x100000000ULL ||
       dataPhysical + kDataBytes > 0x100000000ULL)) {
    kprintf("NativeMtkUfs: CCAP reports 32-bit DMA but buffers are above 4 GiB\n");
    return false;
  }

  bzero(hostBytesPtr, hostBytes);
  bzero(dataBytesPtr, kDataBytes);

  // Pre-fill every UTRD with its UCD DMA address, response UPIU offset/length
  // and PRDT offset, exactly as ufshcd_host_memory_configure() does (spec §3.4).
  for (unsigned tag = 0; tag < nutrs; ++tag) {
    uint32_t *words = reinterpret_cast<uint32_t *>(utrdAt(tag));
    const uint64_t ucd = ucdPhysical(tag);
    words[0] = 0;
    words[1] = 0;
    words[2] = kOcsInvalid;
    words[3] = 0;
    words[4] = static_cast<uint32_t>(ucd);
    words[5] = static_cast<uint32_t>(ucd >> 32);
    words[6] = kUpiuSizeDwords | (kUpiuOffsetDwords << 16);
    words[7] = kPrdOffsetDwords << 16;
  }

  cleanDcache(hostBytesPtr, hostBytes);
  cleanDcache(dataBytesPtr, kDataBytes);
  kprintf("NativeMtkUfs: host memory %u bytes phys=0x%llx, data %u bytes phys=0x%llx "
          "(nutrs=%u nutmrs=%u)\n",
          hostBytes, static_cast<unsigned long long>(hostPhysical), kDataBytes,
          static_cast<unsigned long long>(dataPhysical), nutrs, nutmrs);
  return true;
}

bool NativeMtkUfs::enableHostController()
{
  for (unsigned outer = 0; outer < 3; ++outer) {
    if (readUfs(kHce) & 1U) {
      writeUfs(kHce, 0);
      if (!waitUfsMask(kHce, 1, 0, 100000, 1000)) {
        kprintf("NativeMtkUfs: HCE did not clear (attempt %u HCE=0x%08x)\n",
                outer, readUfs(kHce));
        continue;
      }
    }

    // MTK HCE PRE callback: XOUFS_CTRL bit 7 before every HCE enable
    // (spec §3.6).  HCE is written as 1 (no crypto) because
    // CONFIG_SCSI_UFS_CRYPTO on the reference kernel is unknown (§6 #12).
    // The vendor PRE also re-pulses the three host resets; this driver pulses
    // them once in stage 5 and relies on the controller's internal reset here
    // (including the link-startup retry path).
    writeUfs(kXoufsCtrl, readUfs(kXoufsCtrl) | kXoufsCtrlHcePre);
    writeUfs(kIs, 0xFFFFFFFFU);
    writeUfs(kHce, 1);
    IODelay(600);  // hba_enable_delay_us = 600 (spec §3.6)

    if (waitUfsMask(kHce, 1, 1, 50000, 1000)) {
      // Spec §3.6/§4.5: UFSHCD_UIC_MASK.  This version has no IRQ handler;
      // UIC completion and UTP completion are polled.  Assign the mask rather
      // than OR-ing it so a stale UTP/error enable left by firmware cannot make
      // the controller assert an interrupt nobody drains.  If GIC SPI 137 is
      // routed and a stuck level interrupt is observed, this is the first
      // register to revisit.
      writeUfs(kIe, kUicMask);
      return true;
    }
    kprintf("NativeMtkUfs: HCE did not set (attempt %u HCE=0x%08x HCS=0x%08x)\n",
            outer, readUfs(kHce), readUfs(kHcs));
  }
  return false;
}

bool NativeMtkUfs::enableController()
{
  if (!allocateHostMemory()) {
    return false;
  }
  if (!enableHostController()) {
    kprintf("NativeMtkUfs: stage 6 controller enable failed\n");
    return false;
  }
  native_mtkufs_hcs = readUfs(kHcs);
  native_mtkufs_stage = 6;  // stage 6: HCE + UIC interrupts enabled
  kprintf("NativeMtkUfs: stage 6 HCE=0x%08x HCS=0x%08x IE=0x%08x\n",
          readUfs(kHce), native_mtkufs_hcs, readUfs(kIe));
  return true;
}

// ---------------------------------------------------------------------------
// Stage 7: UniPro link startup, post-link, make operational
// ---------------------------------------------------------------------------
bool NativeMtkUfs::uicCommand(uint8_t opcode, uint32_t arg1, uint32_t arg2,
                              uint32_t arg3, uint32_t *result, uint32_t *value)
{
  native_mtkufs_uic_opcode = opcode;
  native_mtkufs_uic_step = 1;
  if (!waitUfsMask(kHcs, kHcsUicReady, kHcsUicReady, 100000, 10)) {
    kprintf("NativeMtkUfs: UIC 0x%02x not ready HCS=0x%08x\n", opcode, readUfs(kHcs));
    native_mtkufs_uic_result = 0xFFFFFFFFU;
    return false;
  }

  native_mtkufs_uic_step = 2;
  writeUfs(kIs, kIsUicComplete | kIsUicError);
  writeUfs(kUicArg1, arg1);
  writeUfs(kUicArg2, arg2);
  writeUfs(kUicArg3, arg3);
  dmaBarrier();
  writeUfs(kUicCmd, opcode & 0xFFU);
  dmaBarrier();

  const uint64_t deadline = deadlineAfterMs(5000);  // UIC_CMD_TIMEOUT
  bool completed = false;
  bool uicError = false;
  uint32_t observedStatus = 0;
  for (;;) {
    const uint32_t status = readUfs(kIs);
    observedStatus |= status;
    native_mtkufs_uic_status = observedStatus;
    if (status & kIsUicError) {
      // UE is an asynchronous layer event, not the UIC command's result.
      // Preserve the read-to-clear error registers, acknowledge UE, then wait
      // for UCCS/ARG2 as the vendor core does (e.g. a startup LINERESET).
      native_mtkufs_uic_errors[0] = readUfs(kUecpa);
      native_mtkufs_uic_errors[1] = readUfs(kUecdl);
      native_mtkufs_uic_errors[2] = readUfs(kUecn);
      native_mtkufs_uic_errors[3] = readUfs(kUect);
      native_mtkufs_uic_errors[4] = readUfs(kUecdme);
      uicError = true;
      writeUfs(kIs, kIsUicError);
    }
    if (status & kIsUicComplete) {
      completed = true;
      break;
    }
    if (mach_absolute_time() >= deadline) {
      break;
    }
    IODelay(10);
  }

  const uint32_t readArg2 = readUfs(kUicArg2);
  const uint32_t readArg3 = readUfs(kUicArg3);
  if (completed) {
    writeUfs(kIs, kIsUicComplete);
  }
  if (uicError) {
    writeUfs(kIs, kIsUicError);
  }
  drainUicInterrupts();
  if (result) {
    *result = readArg2 & 0xFFU;
  }
  if (value) {
    *value = readArg3;
  }
  native_mtkufs_uic_result = completed ? (readArg2 & 0xFFU) : 0xFFFFFFFFU;
  if (completed) {
    native_mtkufs_uic_step = (readArg2 & 0xFFU) ? 4 : 3;
  } else {
    native_mtkufs_uic_step = 5;
  }

  if (uicError) {
    kprintf("NativeMtkUfs: UIC layer event opcode=0x%02x IS=0x%08x "
            "PA=%08x DL=%08x N=%08x T=%08x DME=%08x complete=%u result=%02x\n",
            opcode, observedStatus, native_mtkufs_uic_errors[0],
            native_mtkufs_uic_errors[1], native_mtkufs_uic_errors[2],
            native_mtkufs_uic_errors[3], native_mtkufs_uic_errors[4],
            completed ? 1U : 0U, readArg2 & 0xFFU);
  }
  if (!completed) {
    kprintf("NativeMtkUfs: UIC 0x%02x timeout (IS=0x%08x HCS=0x%08x)\n",
            opcode, observedStatus, readUfs(kHcs));
    return false;
  }
  if (readArg2 & 0xFFU) {
    kprintf("NativeMtkUfs: UIC 0x%02x error (arg2=0x%08x)\n", opcode, readArg2);
    return false;
  }
  return true;
}

void NativeMtkUfs::drainUicInterrupts()
{
  // No IRQ handler is registered.  UIC event bits enabled by kUicMask are
  // status-only; clearing them keeps a level-triggered SPI from staying
  // asserted.  Fatal bits are deliberately left for the caller.
  const uint32_t status = readUfs(kIs);
  if (status & kIsUicError) {
    kprintf("NativeMtkUfs: draining UIC error IS=0x%08x HCS=0x%08x\n",
            status, readUfs(kHcs));
  }
  const uint32_t clear = status & kIsUicEventBits;
  if (clear) {
    writeUfs(kIs, clear);
  }
}

bool NativeMtkUfs::dmeSet(uint16_t idn, uint8_t selector, uint32_t value)
{
  uint32_t result = 0xFF;
  if (!uicCommand(static_cast<uint8_t>(kUicDmeSet),
                  (static_cast<uint32_t>(idn) << 16) | selector, 0, value,
                  &result, nullptr)) {
    return false;
  }
  if (result != 0) {
    kprintf("NativeMtkUfs: DME_SET 0x%04x sel %u result 0x%02x\n", idn, selector, result);
    return false;
  }
  return true;
}

bool NativeMtkUfs::dmeGet(uint16_t idn, uint8_t selector, uint32_t *value)
{
  uint32_t result = 0xFF;
  uint32_t readValue = 0;
  if (!uicCommand(static_cast<uint8_t>(kUicDmeGet),
                  (static_cast<uint32_t>(idn) << 16) | selector, 0, 0,
                  &result, &readValue)) {
    return false;
  }
  if (result != 0) {
    kprintf("NativeMtkUfs: DME_GET 0x%04x sel %u result 0x%02x\n", idn, selector, result);
    return false;
  }
  if (value) {
    *value = readValue;
  }
  return true;
}

bool NativeMtkUfs::preLink()
{
  uint32_t uniproVersion = 0;
  if (!dmeGet(kPaLocalVerInfo, 0, &uniproVersion)) {
    return false;
  }
  hwMajor = (uniproVersion >= 5) ? 3 : 2;  // UFS_UNIPRO_VER_1_8 == 5

  // Force non-LPM, disable host TX LCC and clear "deep stall" before link
  // startup (spec §3.7 PRE_CHANGE).
  if (!dmeSet(kVsUniProPowerDownControl, 0, 0)) {
    return false;
  }
  if (!dmeSet(kPaLocalTxLccEnable, 0, 0)) {
    return false;
  }
  uint32_t savePower = 0;
  if (!dmeGet(kVsSavePowerControl, 0, &savePower)) {
    return false;
  }
  savePower &= ~(1U << 6);
  if (!dmeSet(kVsSavePowerControl, 0, savePower)) {
    return false;
  }

  kprintf("NativeMtkUfs: pre-link PA_LOCALVERINFO=0x%04x (UniPro major %u)\n",
          uniproVersion, hwMajor);
  return true;
}

void NativeMtkUfs::postLink()
{
  // UniPro clock gating (spec §3.7 POST_CHANGE).
  uint32_t savePower = 0;
  if (dmeGet(kVsSavePowerControl, 0, &savePower)) {
    savePower |= (1U << 0) | (1U << 2) | (1U << 3);
    if (!dmeSet(kVsSavePowerControl, 0, savePower)) {
      kprintf("NativeMtkUfs: post-link VS_SAVEPOWERCONTROL failed\n");
    }
  } else {
    kprintf("NativeMtkUfs: post-link VS_SAVEPOWERCONTROL read failed\n");
  }

  uint32_t debugClock = 0;
  if (dmeGet(kVsDebugClockEnable, 0, &debugClock)) {
    debugClock &= ~(1U << 5);
    if (!dmeSet(kVsDebugClockEnable, 0, debugClock)) {
      kprintf("NativeMtkUfs: post-link VS_DEBUGCLOCKENABLE failed\n");
    }
  } else {
    kprintf("NativeMtkUfs: post-link VS_DEBUGCLOCKENABLE read failed\n");
  }

  // Read UECPA once to clear a possible LINERESET error (spec §3.7).  The
  // auto-hibernate idle timer value (0xC05) is written after the descriptors
  // are read, exactly as the vendor device-init sequence does (spec §3.8).
  (void)readUfs(kUecpa);
  kprintf("NativeMtkUfs: post-link clock gating configured, UECPA cleared\n");
}

// HS gear-up: the link currently runs PWM-G1, which caps a polled read at a
// few MB/s - too slow for the 3.3 GB dyld cache. Sequence follows
// ufshcd_change_power_mode (vendor drivers/scsi/ufs/ufshcd.c): set the lane,
// gear, termination and series attributes, then DME_SET(PA_PWRMODE) with
// rx/tx = FAST_MODE.  Failure is not fatal: the link stays at PWM.
bool NativeMtkUfs::gearUpHighSpeed()
{
  uint32_t lanes = 1;
  if (!dmeGet(kPaActiveTxDataLanes, 0, &lanes) || lanes == 0) {
    lanes = 1;
  }
  if (lanes > 2) {
    lanes = 2;
  }

  // Attribute-by-attribute, each reported: a single rejected attribute used to
  // abort the whole set with only "UIC 0x02 error" to go on, and the link then
  // stayed at PWM with no record of which write the device disliked.
  // HS-G1, not G2.  MT6789 drives CLK_TOP_UFS_SEL from tck_26m_mx9_ck
  // (clk-mt6789.c ufs_parents[0]); HS-G2 wants a 52 MHz reference and the
  // device kept PA_PWRMODE at 0x55 even though the host reported UPMCRS=PWR_OK,
  // which is exactly what an over-geared request looks like from here.  G1 is
  // still roughly 8x PWM-G1, which is what the dyld cache read needs.
  constexpr uint32_t kTargetGear = 1;
  struct { uint16_t idn; const char *name; uint32_t value; } attrs[] = {
    { kPaRxGear,                        "PA_RXGEAR",           kTargetGear },
    { kPaActiveRxDataLanes,             "PA_ACTIVERXDATALANES", lanes },
    { kPaRxTermination,                 "PA_RXTERMINATION",    1 },
    { kPaTxGear,                        "PA_TXGEAR",           kTargetGear },
    { kPaActiveTxDataLanes,             "PA_ACTIVETXDATALANES", lanes },
    { kPaTxTermination,                 "PA_TXTERMINATION",    1 },
    { kPaHsSeries,                      "PA_HSSERIES",         kHsModeA },
    { kPaPowerModeUserData0 + 0,        "PA_PWRMODEUSERDATA0", kDlFc0ProtectionTimeout },
    { kPaPowerModeUserData0 + 1,        "PA_PWRMODEUSERDATA1", kDlTc0ReplayTimeout },
    { kPaPowerModeUserData0 + 2,        "PA_PWRMODEUSERDATA2", kDlAfc0ReqTimeout },
    { kPaPowerModeUserData0 + 3,        "PA_PWRMODEUSERDATA3", kDlFc1ProtectionTimeout },
    { kPaPowerModeUserData0 + 4,        "PA_PWRMODEUSERDATA4", kDlTc1ReplayTimeout },
    { kPaPowerModeUserData0 + 5,        "PA_PWRMODEUSERDATA5", kDlAfc1ReqTimeout },
    { kDmeLocalFc0ProtectionTimeout,    "DME_LocalFC0ProtectionTimeOutVal", kDlFc0ProtectionTimeout },
    { kDmeLocalTc0ReplayTimeout,        "DME_LocalTC0ReplayTimeOutVal",     kDlTc0ReplayTimeout },
    { kDmeLocalAfc0ReqTimeout,          "DME_LocalAFC0ReqTimeOutVal",       kDlAfc0ReqTimeout },
  };
  for (const auto &a : attrs) {
    if (!dmeSet(a.idn, 0, a.value)) {
      // Keep going: the gear attributes are what matter, and a timeout value
      // the device does not expose is not a reason to stay at PWM.
      kprintf("NativeMtkUfs: attribute %s (0x%04x) rejected, continuing\n",
              a.name, a.idn);
      native_mtkufs_hs_rejected = native_mtkufs_hs_rejected + 1;
    }
  }

  const uint32_t mode = (kFastMode << 4) | kFastMode;   // rx and tx
  uint32_t result = 0xFF;
  const bool sent = uicCommand(static_cast<uint8_t>(kUicDmeSet),
                               static_cast<uint32_t>(kPaPowerMode) << 16, 0,
                               mode, &result, nullptr);
  if (!sent || result != 0) {
    kprintf("NativeMtkUfs: DME_SET(PA_PWRMODE 0x%x) failed sent=%d result=0x%02x\n",
            mode, sent ? 1 : 0, result);
    return false;
  }

  // A completed UIC command is not a completed gear change.  UPMCRS in HCS
  // reports the outcome: PWR_OK means the link moved, PWR_LOCAL means the
  // request was accepted but the UniPro layer has not finished.  Wait for it
  // instead of trusting the command completion, and re-read PA_PWRMODE after.
  const uint64_t deadline = deadlineAfterMs(200);
  uint32_t upmcrs = kPwrLocal;
  for (;;) {
    upmcrs = (readUfs(kHcs) >> kHcsUpmcrsShift) & kHcsUpmcrsMask;
    if (upmcrs == kPwrOk || mach_absolute_time() >= deadline) {
      break;
    }
    __asm__ volatile("yield" ::: "memory");
  }
  if (upmcrs != kPwrOk) {
    kprintf("NativeMtkUfs: power mode change not taken, UPMCRS=0x%x HCS=0x%08x\n",
            upmcrs, readUfs(kHcs));
    return false;
  }

  uint32_t now = 0;
  if (dmeGet(kPaPowerMode, 0, &now)) {
    kprintf("NativeMtkUfs: link power mode now 0x%x (lanes=%u, want 0x%x)\n",
            now, lanes, mode);
    if (now != mode) {
      kprintf("NativeMtkUfs: WARNING link still not at HS after UPMCRS=PWR_OK\n");
    }
  }
  return true;
}

bool NativeMtkUfs::makeOperational()
{
  // TODO(spec §3.9): HS gear-up (PA_PWRMODE 0x11, PA_TXHSADAPTTYPE, the
  // PA_PWRMODEUSERDATA0..5 and DME timeout attributes) is not implemented.
  // The link stays at the default PWM-G1 rate; reads are correct but slow.
  const uint64_t utmrPhysical = hostPhysical + kUtmrOffset;
  writeUfs(kUtrlBaseLo, static_cast<uint32_t>(hostPhysical));
  writeUfs(kUtrlBaseHi, static_cast<uint32_t>(hostPhysical >> 32));
  writeUfs(kUtmrBaseLo, static_cast<uint32_t>(utmrPhysical));
  writeUfs(kUtmrBaseHi, static_cast<uint32_t>(utmrPhysical >> 32));
  dmaBarrier();

  // UTRLRDY, UTMRLRDY and UCRDY must all be set before run/stop (spec §4.6).
  if (!waitUfsMask(kHcs, kHcsListsReady, kHcsListsReady, 100000, 100)) {
    kprintf("NativeMtkUfs: controller not ready for requests HCS=0x%08x\n", readUfs(kHcs));
    return false;
  }
  writeUfs(kUtmrRunStop, 1);
  writeUfs(kUtrlRunStop, 1);
  dmaBarrier();

  // TODO(spec §4.6): with a real IRQ handler this is where IE would gain
  // UFSHCD_ENABLE_INTRS and UTRIACR would be programmed.  This driver polls IS
  // and has no interrupt source registered, so only the UIC mask stays enabled.
  utpReady = true;
  native_mtkufs_hcs = readUfs(kHcs);
  kprintf("NativeMtkUfs: UTP lists operational HCS=0x%08x UTRL=0x%llx UTMR=0x%llx\n",
          native_mtkufs_hcs,
          static_cast<unsigned long long>(hostPhysical),
          static_cast<unsigned long long>(utmrPhysical));
  return true;
}

bool NativeMtkUfs::linkStartup()
{
  // DME_LINKSTARTUP_RETRIES is 3 in the vendor core: one initial attempt plus
  // up to three HCE restarts (spec §3.7 step 4).
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    if (attempt > 0) {
      kprintf("NativeMtkUfs: power-cycling the device before link startup attempt %u\n", attempt + 1);
      (void)mtkSip(kSipDevicePwrCtrl, 0, 0);
      IODelay(1000);
      (void)mtkSip(kSipDevicePwrCtrl, 1, 0);
      (void)mtkSip(kSipDeviceReset, 0, 0);
      IODelay(10);
      (void)mtkSip(kSipDeviceReset, 1, 0);
      IOSleep(20);
      kprintf("NativeMtkUfs: re-enabling HCE before link startup attempt %u\n", attempt + 1);
      if (!enableHostController()) {
        kprintf("NativeMtkUfs: HCE restart failed during link retry\n");
        return false;
      }
    }

    if (!preLink()) {
      kprintf("NativeMtkUfs: pre-link DME sequence failed (attempt %u)\n", attempt + 1);
      continue;
    }

    uint32_t result = 0xFFFFFFFFU;
    const bool completed = uicCommand(static_cast<uint8_t>(kUicLinkStartup),
                                      0, 0, 0, &result, nullptr);
    if (completed) {
      native_mtkufs_uic_result = result;
    } else {
      native_mtkufs_uic_result = 0xFFFFFFFFU;
    }
    const uint32_t hcs = readUfs(kHcs);
    native_mtkufs_hcs = hcs;
    kprintf("NativeMtkUfs: link startup attempt %u complete=%u result=0x%02x HCS=0x%08x\n",
            attempt + 1, completed ? 1U : 0U, result, hcs);

    if (completed && result == 0 && (hcs & kHcsDevicePresent)) {
      postLink();
      if (!makeOperational()) {
        return false;
      }
      // Link is up at PWM: try HS before the descriptors and the first UTP
      // read, so every later transfer runs at the higher gear.
      (void)gearUpHighSpeed();
      native_mtkufs_stage = 7;  // stage 7: link startup + UIC command complete
      kprintf("NativeMtkUfs: stage 7 link up, DEVICE_PRESENT set\n");
      return true;
    }
  }

  kprintf("NativeMtkUfs: stage 7 link startup failed after all retries "
          "(last UIC result 0x%02x)\n",
          native_mtkufs_uic_result);
  return false;
}

// ---------------------------------------------------------------------------
// UTP submission
// ---------------------------------------------------------------------------
bool NativeMtkUfs::runUtp(unsigned tag, uint32_t commandType, uint32_t direction,
                          uint32_t prdBytes, uint32_t timeoutMs, uint32_t *ocs)
{
  if (!utpReady || tag >= nutrs || !hostBytesPtr) {
    return false;
  }
  if (prdBytes > kDataBytes) {
    return false;
  }

  uint8_t *utrd = utrdAt(tag);
  uint8_t *ucd = ucdAt(tag);
  uint32_t *words = reinterpret_cast<uint32_t *>(utrd);

  words[0] = (commandType << 28) | direction | kUtpReqDescIntCmd;
  words[1] = 0;
  words[2] = kOcsInvalid;
  words[3] = 0;
  words[4] = static_cast<uint32_t>(ucdPhysical(tag));
  words[5] = static_cast<uint32_t>(ucdPhysical(tag) >> 32);
  words[6] = kUpiuSizeDwords | (kUpiuOffsetDwords << 16);
  words[7] = (prdBytes ? 1U : 0U) | (kPrdOffsetDwords << 16);

  if (prdBytes) {
    uint32_t *prd = reinterpret_cast<uint32_t *>(ucd + kUcdPrdOffset);
    prd[0] = static_cast<uint32_t>(dataPhysical);
    prd[1] = static_cast<uint32_t>(dataPhysical >> 32);
    prd[2] = 0;
    prd[3] = prdBytes - 1;  // PRDT size is data length minus one
  }

  cleanDcache(utrd, kTrdSize);
  cleanDcache(ucd, kUcdDirtyBytes);  // command/response UPIUs and the PRDT entry
  if (prdBytes) {
    cleanDcache(dataBytesPtr, prdBytes);
  }
  dmaBarrier();

  writeUfs(kIs, kIsUtpComplete);
  dmaBarrier();
  writeUfs(kUtrlDoorbell, 1U << tag);
  dmaBarrier();

  const uint64_t deadline = deadlineAfterMs(timeoutMs);
  bool completed = false;
  bool interruptSeen = false;
  uint32_t fatal = 0;
  uint32_t ocsValue = kOcsInvalid;
  for (;;) {
    const uint32_t status = readUfs(kIs);
    if (status & kIsUtpComplete) {
      interruptSeen = true;
    }
    fatal = status & kIsFatalMask;

    // The UTRD OCS field is the completion record in host memory.  Polling it
    // does not depend on IE/IS being enabled, and this driver deliberately has
    // no interrupt handler.  The cache line is invalidated before each read
    // because the DMA buffers are cached on this design.
    invalidateDcache(utrd, kTrdSize);
    ocsValue = *reinterpret_cast<const volatile uint32_t *>(utrd + 8) & kOcsMask;
    if (ocsValue != kOcsInvalid) {
      completed = true;
      break;
    }
    if (fatal) {
      break;
    }
    if (mach_absolute_time() >= deadline) {
      break;
    }
    // Spin without stalling: a completion takes a few microseconds, and a
    // 100 us IODelay per poll made a 3.3 GB dyld cache read take hours.
    __asm__ volatile("yield" ::: "memory");
  }

  // Publish device-written data before the CPU reads it.
  invalidateDcache(utrd, kTrdSize);
  invalidateDcache(ucd, kUcdDirtyBytes);
  if (prdBytes) {
    invalidateDcache(dataBytesPtr, prdBytes);
  }
  if (interruptSeen || completed) {
    writeUfs(kIs, kIsUtpComplete);
  }

  // Re-read after the final invalidation so the reported OCS is the final one.
  ocsValue = *reinterpret_cast<const volatile uint32_t *>(utrd + 8) & kOcsMask;
  if (ocs) {
    *ocs = ocsValue;
  }
  drainUicInterrupts();

  if (fatal) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    utpReady = false;  // controller state is no longer trustworthy
    kprintf("NativeMtkUfs: UTP tag %u fatal IS=0x%08x OCS=0x%x HCS=0x%08x\n",
            tag, fatal, ocsValue, readUfs(kHcs));
    return false;
  }
  if (!completed) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    // TODO(bring-up): no timeout recovery is implemented.  Quarantine the
    // transport instead of reusing a possibly still-outstanding tag; a later
    // version should stop HBA, re-enable HCE and re-run makeOperational().
    utpReady = false;
    kprintf("NativeMtkUfs: UTP tag %u timeout OCS=0x%x IS=0x%08x HCS=0x%08x\n",
            tag, ocsValue, readUfs(kIs), readUfs(kHcs));
    return false;
  }
  return true;
}

bool NativeMtkUfs::sendNop()
{
  const unsigned tag = nutrs - 1;
  uint8_t *ucd = ucdAt(tag);
  bzero(ucd, kUcdPayload);

  // NOP OUT is a device-management transaction; the device answers with NOP IN.
  ucd[0] = static_cast<uint8_t>(kUpiuNopOut);
  ucd[1] = 0;
  ucd[2] = 0;
  ucd[3] = static_cast<uint8_t>(tag);

  // NOP OUT / NOP IN has a 50 ms timeout in the vendor core (spec §3.8).
  uint32_t ocs = kOcsInvalid;
  if (!runUtp(tag, utpCommandType(true), kUtpNoData, 0, 50, &ocs)) {
    return false;
  }
  if (ocs != 0) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    kprintf("NativeMtkUfs: NOP OUT OCS=0x%x\n", ocs);
    return false;
  }
  const uint8_t *response = ucd + kUpiuResponseOffset;
  if (response[0] != kUpiuNopIn || response[6] != 0) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    kprintf("NativeMtkUfs: unexpected NOP response type=0x%02x code=0x%02x\n",
            response[0], response[6]);
    return false;
  }
  return true;
}

bool NativeMtkUfs::sendQuery(bool write, uint8_t opcode, uint8_t idn,
                             uint8_t index, uint8_t selector, uint16_t length,
                             uint32_t value, uint32_t *responseValue,
                             uint8_t *descriptor, uint32_t descriptorMax,
                             uint32_t *descriptorLength)
{
  const unsigned tag = nutrs - 1;
  uint8_t *ucd = ucdAt(tag);
  bzero(ucd, kUcdPayload);

  ucd[0] = static_cast<uint8_t>(kUpiuQueryRequest);
  ucd[1] = 0;
  ucd[2] = 0;
  ucd[3] = static_cast<uint8_t>(tag);
  ucd[5] = static_cast<uint8_t>(write ? kUpiuQueryFuncWrite : kUpiuQueryFuncRead);
  ucd[12] = opcode;
  ucd[13] = idn;
  ucd[14] = index;
  ucd[15] = selector;
  ucd[18] = static_cast<uint8_t>(length >> 8);  // utp_upiu_query.length, big endian
  ucd[19] = static_cast<uint8_t>(length);
  if (write) {
    putBe32(ucd + 20, value);
  }

  // Query transactions use QUERY_REQ_TIMEOUT = 1500 ms in the vendor core.
  uint32_t ocs = kOcsInvalid;
  if (!runUtp(tag, utpCommandType(true), kUtpNoData, 0, 1500, &ocs)) {
    return false;
  }
  if (ocs != 0) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    kprintf("NativeMtkUfs: query opcode 0x%02x idn 0x%02x OCS=0x%x\n",
            opcode, idn, ocs);
    return false;
  }

  const uint8_t *response = ucd + kUpiuResponseOffset;
  if (response[0] != kUpiuQueryResponse) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    kprintf("NativeMtkUfs: query opcode 0x%02x idn 0x%02x unexpected type 0x%02x\n",
            opcode, idn, response[0]);
    return false;
  }
  if (response[6] != 0) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    kprintf("NativeMtkUfs: query opcode 0x%02x idn 0x%02x response 0x%02x\n",
            opcode, idn, response[6]);
    return false;
  }

  if (responseValue) {
    *responseValue = getBe32(response + 20);  // upiu_res.value
  }
  if (descriptor && descriptorLength) {
    // Descriptor payload starts after the 32-byte response UPIU header; its
    // length is the low 16 bits of response header dword 2 (spec §3.8).
    const uint32_t segmentLength =
        (static_cast<uint32_t>(response[10]) << 8) | response[11];
    if (segmentLength > descriptorMax ||
        segmentLength > (512U - kUpiuResponseHeader)) {
      native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
      kprintf("NativeMtkUfs: descriptor idn 0x%02x length %u exceeds buffer %u\n",
              idn, segmentLength, descriptorMax);
      return false;
    }
    memcpy(descriptor, response + kUpiuResponseHeader, segmentLength);
    *descriptorLength = segmentLength;
  }
  return true;
}

bool NativeMtkUfs::readDescriptor(uint8_t idn, uint8_t index, uint8_t *out,
                                  uint32_t outMax, uint32_t *outLength)
{
  if (!out || !outLength || outMax == 0) {
    return false;
  }
  *outLength = 0;
  // Ask for the maximum UFS descriptor size (255 bytes, spec §4.8); the response
  // length determines how many bytes are copied.  The vendor core retries a
  // descriptor query up to three times (spec §3.8 step 3).
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (sendQuery(false, static_cast<uint8_t>(kQueryReadDesc), idn, index, 0,
                  0x00FF, 0, nullptr, out, outMax, outLength)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Stage 8: NOP/fDeviceInit and the three descriptors
// ---------------------------------------------------------------------------
bool NativeMtkUfs::readDescriptors()
{
  bool nopDone = false;
  for (unsigned attempt = 0; attempt < 10; ++attempt) {
    if (sendNop()) {
      nopDone = true;
      break;
    }
    IODelay(1000);
  }
  if (!nopDone) {
    kprintf("NativeMtkUfs: stage 8 NOP OUT/IN failed\n");
    return false;
  }
  kprintf("NativeMtkUfs: NOP OUT/IN ok\n");

  // fDeviceInit: SET_FLAG (retried like ufshcd_query_flag_retry), then poll
  // READ_FLAG until it clears (1500 ms).
  bool flagSet = false;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (sendQuery(true, static_cast<uint8_t>(kQuerySetFlag),
                  static_cast<uint8_t>(kFlagDeviceInit), 0, 0, 0, 0,
                  nullptr, nullptr, 0, nullptr)) {
      flagSet = true;
      break;
    }
  }
  if (!flagSet) {
    kprintf("NativeMtkUfs: fDeviceInit SET_FLAG failed\n");
    return false;
  }
  const uint64_t initDeadline = deadlineAfterMs(1500);
  bool initialized = false;
  for (;;) {
    uint32_t flagValue = 0;
    if (!sendQuery(false, static_cast<uint8_t>(kQueryReadFlag),
                   static_cast<uint8_t>(kFlagDeviceInit), 0, 0, 0, 0,
                   &flagValue, nullptr, 0, nullptr)) {
      kprintf("NativeMtkUfs: fDeviceInit READ_FLAG failed\n");
      return false;
    }
    if ((flagValue & 0xFFU) == 0) {
      initialized = true;
      break;
    }
    if (mach_absolute_time() >= initDeadline) {
      break;
    }
    IOSleep(10);
  }
  if (!initialized) {
    kprintf("NativeMtkUfs: fDeviceInit did not clear within 1500 ms\n");
    return false;
  }
  kprintf("NativeMtkUfs: fDeviceInit complete\n");

  uint8_t device[0x100];
  uint8_t geometry[0x100];
  uint8_t unit[0x100];
  uint32_t deviceLength = 0;
  uint32_t geometryLength = 0;
  uint32_t unitLength = 0;
  bzero(device, sizeof(device));
  bzero(geometry, sizeof(geometry));
  bzero(unit, sizeof(unit));

  const bool haveDevice = readDescriptor(kDescDevice, 0, device,
                                         static_cast<uint32_t>(sizeof(device)),
                                         &deviceLength) &&
                          deviceLength >= 0x1A;
  if (!haveDevice) {
    kprintf("NativeMtkUfs: device descriptor read failed (length=%u)\n", deviceLength);
    return false;
  }

  const bool haveGeometry = readDescriptor(kDescGeometry, 0, geometry,
                                           static_cast<uint32_t>(sizeof(geometry)),
                                           &geometryLength) &&
                            geometryLength >= 0x0D;
  if (!haveGeometry) {
    // Raw geometry spans the device; it is not the capacity of a selected LU.
    kprintf("NativeMtkUfs: geometry descriptor unavailable (length=%u)\n",
            geometryLength);
  }

  const bool haveUnit = readDescriptor(kDescUnit, kUserLun, unit,
                                       static_cast<uint32_t>(sizeof(unit)),
                                       &unitLength) &&
                        unitLength >= 0x13;
  if (!haveUnit) {
    kprintf("NativeMtkUfs: LUN %u unit descriptor read failed (length=%u)\n",
            kUserLun, unitLength);
    return false;
  }
  if (!isUsableUserUnitDescriptor(unit)) {
    kprintf("NativeMtkUfs: refuse non-user LU: type=%u index=%u enabled=%u boot=%u\n",
            unit[1], unit[2], unit[3], unit[4]);
    return false;
  }

  // bLUWriteProtect (unit descriptor offset 0x5, UFS spec / Linux
  // UNIT_DESC_PARAM_LU_WR_PROTECT) says whether the LU refuses writes.  The
  // earlier bring-up only needed reads, so this byte was never looked at; the
  // expdb log window does need it, and a non-zero value means every WRITE(10)
  // would come back with sense data instead of doing anything.
  unitWriteProtect = unit[kUnitDescWriteProtect];
  kprintf("NativeMtkUfs: LUN %u unit desc write-protect=0x%02x (%s) qdepth=%u\n",
          kUserLun, unitWriteProtect,
          unitWriteProtect == 0 ? "writable" : "REFUSES WRITES",
          unit[kUnitDescQueueDepth]);

  const uint32_t specVersion = getBe16(device + kDeviceDescSpecVersion);
  const uint32_t manufacturer = getBe16(device + kDeviceDescManufacturer);
  const uint32_t numLu = device[kDeviceDescNumLu];
  const uint32_t queueDepth = (deviceLength > kDeviceDescQueueDepth)
                                  ? device[kDeviceDescQueueDepth]
                                  : 0;
  kprintf("NativeMtkUfs: device descriptor len=%u spec=0x%04x manf=0x%04x "
          "numLU=%u queueDepth=%u\n",
          deviceLength, specVersion, manufacturer, numLu, queueDepth);

  uint64_t rawCapacity512 = 0;
  if (haveGeometry) {
    rawCapacity512 = getBe64(geometry + kGeometryDescCapacity);
    kprintf("NativeMtkUfs: geometry rawCapacity512=%llu maxLU=%u minAddrBlock=%u\n",
            static_cast<unsigned long long>(rawCapacity512),
            static_cast<unsigned>(geometry[kGeometryDescMaxLu]),
            static_cast<unsigned>(geometry[kGeometryDescMinBlock]));
  }

  const uint8_t blockExponent = unit[kUnitDescBlockSize];
  const uint64_t logicalBlocks = getBe64(unit + kUnitDescBlockCount);
  if (blockExponent < 9 || blockExponent > 20) {
    kprintf("NativeMtkUfs: LUN %u invalid bLogicalBlockSize exponent %u\n",
            kUserLun, blockExponent);
    return false;
  }
  const uint32_t blockSize = 1U << blockExponent;
  uint64_t unitBytes = 0;
  if (logicalBlocks != 0) {
    if (logicalBlocks > (~static_cast<uint64_t>(0)) / blockSize) {
      kprintf("NativeMtkUfs: LUN %u capacity overflow\n", kUserLun);
      return false;
    }
    unitBytes = logicalBlocks * blockSize;
  } else {
    kprintf("NativeMtkUfs: LUN %u is empty; refusing whole-device capacity fallback\n",
            kUserLun);
    return false;
  }

  uint64_t geometryBytes = 0;
  if (rawCapacity512 && rawCapacity512 <= (~static_cast<uint64_t>(0)) / 512ULL) {
    geometryBytes = rawCapacity512 * 512ULL;
  }
  // Plan A: the whole media is the super APFS window; no partition scheme.
  const uint64_t mediaBytes = (unitBytes > kSuperWindowBytes) ? kSuperWindowBytes : unitBytes;
  if (mediaBytes == 0 || mediaBytes > (~static_cast<uint64_t>(0)) - 511ULL) {
    kprintf("NativeMtkUfs: unusable media size\n");
    return false;
  }

  deviceBlockSize = blockSize;
  deviceCapacityBlocks = logicalBlocks ? logicalBlocks : (unitBytes / deviceBlockSize);
  deviceBlocks = mediaBytes / deviceBlockSize;
  mediaSectors = mediaBytes / 512ULL;
  native_mtkufs_sectors = mediaSectors;
  if (mediaSectors == 0 || deviceBlocks == 0) {
    kprintf("NativeMtkUfs: media geometry is empty\n");
    return false;
  }

  // TODO(spec §3.10): device-specific quirks (Samsung PA_TACTIVATE/H8 time,
  // Micron/H9HQ15 broken-VCC handling, WDC RX lane limit) are not applied.
  // They affect HS gear-up and power management; the PWM-G1 read path below
  // does not depend on them.
  kprintf("NativeMtkUfs: LUN %u blockSize=%u blocks=%llu (%llu bytes), "
          "geometry=%llu bytes, media=%llu 512-byte sectors\n",
          kUserLun, deviceBlockSize, static_cast<unsigned long long>(logicalBlocks),
          static_cast<unsigned long long>(unitBytes),
          static_cast<unsigned long long>(geometryBytes),
          static_cast<unsigned long long>(mediaSectors));

  // The APFS container header declares its own block count. The container was
  // written contiguously from the "super" start, so it can be larger than the
  // GPT entry; follow the container and stay inside the LUN.
  if (readDeviceBlocks(kSuperWindowBaseLba, 1, dataBytesPtr)) {
    const uint8_t *nx = dataBytesPtr;
    if (nx[0x20] == 'N' && nx[0x21] == 'X' && nx[0x22] == 'S' && nx[0x23] == 'B') {
      const uint32_t nxBlockSize = getLe32(nx + 0x24);
      const uint64_t nxBlocks = getLe64(nx + 0x28);
      if (nxBlockSize == deviceBlockSize && nxBlocks > deviceBlocks &&
          nxBlocks <= logicalBlocks) {
        deviceBlocks = nxBlocks;
        mediaSectors = (nxBlocks * deviceBlockSize) / 512ULL;
        native_mtkufs_sectors = mediaSectors;
        kprintf("NativeMtkUfs: media follows container to %llu blocks (%llu bytes)\n",
                static_cast<unsigned long long>(nxBlocks),
                static_cast<unsigned long long>(nxBlocks * deviceBlockSize));
      }
    }
  }

  // TODO(spec §3.8): configuration (1), interconnect (4), power (8) and health
  // (9) descriptors are not read; stage 8 only requires device/geometry/unit.
  // bRefClkFreq (attribute 0x0A, ufs.h QUERY_ATTR_IDN_REF_CLK_FREQ).  The
  // device refuses a gear change while this says "19.2 MHz / whatever the
  // vendor left", because a link at HS needs its reference clock described
  // correctly.  MT6789's CLK_TOP_UFS_SEL parent 0 is tck_26m_mx9_ck
  // (clk-mt6789.c ufs_parents[]), so the value is REF_CLK_FREQ_26_MHZ = 1.
  // Read first: the log then always says what the device believed.
  {
    constexpr uint8_t kAttrRefClkFreq = 0x0A;
    constexpr uint32_t kRefClk26Mhz = 1;   // ufs.h REF_CLK_FREQ_26_MHZ
    uint32_t current = 0xFF;
    const bool readOk = sendQuery(false, 0x03 /* READ_ATTR */, kAttrRefClkFreq,
                                  0, 0, 4, 0, &current, nullptr, 0, nullptr);
    bool writeOk = false;
    if (readOk) {
      if (current == kRefClk26Mhz) {
        writeOk = true;   // already correct, nothing to update
      } else {
        writeOk = sendQuery(false, 0x04 /* WRITE_ATTR */, kAttrRefClkFreq,
                            0, 0, 4, kRefClk26Mhz, nullptr, nullptr, 0, nullptr);
      }
    }
    uint32_t after = 0xFF;
    const bool reread = sendQuery(false, 0x03 /* READ_ATTR */, kAttrRefClkFreq,
                                  0, 0, 4, 0, &after, nullptr, 0, nullptr);
    kprintf("NativeMtkUfs: bRefClkFreq read=%s value=%u write=%s after=%u\n",
            readOk ? "ok" : "FAIL", current, writeOk ? "ok" : "FAIL",
            reread ? after : 0xFFFFU);
  }

  // AHIT: fb144 disables auto-hibernate (0x18 = 0, the vendor's explicit
  // disable-AH8 value).  fb142 caught one single-block read returning
  // different bytes on two consecutive reads (VERIFY MISMATCH lba=397534,
  // 1 in ~434k), and the code's own suspicion has always been requests that
  // land around a hibernate exit.  Cost: a little idle power, no function.
  writeUfs(kAhit, 0);
  kprintf("NativeMtkUfs: auto-hibernate disabled (AHIT=0x%08x)\n", readUfs(kAhit));

  native_mtkufs_stage = 8;  // stage 8: descriptors read
  kprintf("NativeMtkUfs: stage 8 descriptors read\n");
  return true;
}

// ---------------------------------------------------------------------------
// Stage 9: prove the UTP path with LBA 0
// ---------------------------------------------------------------------------
bool NativeMtkUfs::readDeviceBlocks(uint64_t lba, uint32_t blocks, void *out)
{
  // The cryptex window has no host controller state of its own: it borrows the
  // primary instance's UTRD/UCD and bounce buffers through this forward.
  if (secondaryWindow && gBlockMedia && gBlockMedia != this) {
    return gBlockMedia->readDeviceBlocks(lba, blocks, out);
  }
  if (!gUtpGate) {
    return readDeviceBlocksRecovering(lba, blocks, out);
  }
  utpGateLockTracked();
  const bool ok = readDeviceBlocksRecovering(lba, blocks, out);
  utpGateUnlockTracked();
  return ok;
}

bool NativeMtkUfs::readDeviceBlocksRecovering(uint64_t lba, uint32_t blocks, void *out)
{
  return native_mtkufs::readWithRecovery(readRecoveryEnabled,
      [this, lba, blocks, out]() {
        if (readDeviceBlocksLocked(lba, blocks, out))
          return native_mtkufs::ReadResult::success;
        if (lastReadTransportFailure) return native_mtkufs::ReadResult::transport;
        if (lastScsiSenseKey == 0x06) return native_mtkufs::ReadResult::unitAttention;
        return native_mtkufs::ReadResult::failure;
      }, [this]() { return recoverReadTransport(); });
}

// Called only for a failed READ while the shared transport gate is held.
// HelioDarwin 0186 motivates bounded retries; this port requires a completed
// HCE reset, link/NOP and unchanged LU geometry before any descriptor reuse.
// Never invoke resetDeviceAndHost(), WRITE_ATTR or WRITE(10) from this path.
bool NativeMtkUfs::recoverReadTransport()
{
  if (secondaryWindow || !readRecoveryEnabled || readRecoveryAttempts >= 2 ||
      !lastReadTransportFailure || !deviceCapacityBlocks) return false;
  ++readRecoveryAttempts;
  utpReady = false;
  kprintf("YUNLUO-UFS recover-read attempt=%u HCE=%x DBR=%x\n",
          readRecoveryAttempts, readUfs(kHce), readUfs(kUtrlDoorbell));
  writeUfs(kUtrlRunStop, 0);
  writeUfs(kUtmrRunStop, 0);
  dmaBarrier();
  writeUfs(kHce, 0);
  dmaBarrier();
  if (!waitUfsMask(kHce, 1, 0, 100000, 1000) ||
      !waitUfsMask(kUtrlDoorbell, 0xffffffffU, 0, 100000, 1000)) {
    kprintf("YUNLUO-UFS recovery refused: controller did not quiesce\n");
    return false;
  }
  // Only the acknowledged disabled controller makes DMA memory reusable.
  bzero(hostBytesPtr, hostBytes);
  cleanDcache(hostBytesPtr, hostBytes);
  if (!enableHostController() || !linkStartup() || !sendNop()) {
    utpReady = false;
    return false;
  }
  writeUfs(kAhit, 0);  // retain the verified non-hibernating read policy
  uint8_t unit[0xFF] = {};
  uint32_t length = 0;
  if (!readDescriptor(kDescUnit, kUserLun, unit, sizeof(unit), &length) ||
      length < 0x13 || !isUsableUserUnitDescriptor(unit) ||
      unit[kUnitDescBlockSize] < 9 || unit[kUnitDescBlockSize] > 20 ||
      (1U << unit[kUnitDescBlockSize]) != deviceBlockSize ||
      getBe64(unit + kUnitDescBlockCount) != deviceCapacityBlocks ||
      unit[kUnitDescWriteProtect] != unitWriteProtect) {
    utpReady = false;
    kprintf("YUNLUO-UFS recovery refused: LU identity/geometry changed\n");
    return false;
  }
  kprintf("YUNLUO-UFS read transport restored; retry original READ once\n");
  return true;
}

bool NativeMtkUfs::readDeviceBlocksLocked(uint64_t lba, uint32_t blocks, void *out)
{
  lastScsiSenseKey = 0xFF;
  lastReadTransportFailure = false;
  if (!utpReady || !deviceBlockSize || blocks == 0 || blocks > 0xFFFFU) {
    return false;
  }
  // READ(10) carries a 32-bit LBA.  Media larger than 2 TiB would need READ(16),
  // which this first version does not implement.
  if (lba > 0xFFFFFFFFULL || blocks - 1ULL > 0xFFFFFFFFULL - lba) {
    kprintf("NativeMtkUfs: READ(10) LBA 0x%llx exceeds the 32-bit CDB field\n",
            static_cast<unsigned long long>(lba));
    return false;
  }
  if (deviceCapacityBlocks &&
      (lba >= deviceCapacityBlocks || blocks > deviceCapacityBlocks - lba)) {
    return false;
  }
  const uint64_t bytes64 = static_cast<uint64_t>(blocks) * deviceBlockSize;
  if (bytes64 == 0 || bytes64 > kDataBytes || bytes64 > 0xFFFFFFFFULL) {
    return false;
  }
  const uint32_t bytes = static_cast<uint32_t>(bytes64);

  // Single or multi-block READ(10) through one UTP transfer request.  The PRDT
  // describes the contiguous bounce buffer; the caller copies out afterwards.
  const unsigned tag = 0;
  uint8_t *ucd = ucdAt(tag);
  bzero(ucd, kUcdPayload);

  ucd[0] = static_cast<uint8_t>(kUpiuCommand);
  ucd[1] = static_cast<uint8_t>(kUpiuCmdFlagsRead);
  ucd[2] = kUserLun;
  ucd[3] = static_cast<uint8_t>(tag);
  putBe32(ucd + 12, bytes);  // expected data transfer length, big endian
  uint8_t *cdb = ucd + 16;
  cdb[0] = 0x28;  // READ(10)
  cdb[1] = 0;
  cdb[2] = static_cast<uint8_t>(lba >> 24);
  cdb[3] = static_cast<uint8_t>(lba >> 16);
  cdb[4] = static_cast<uint8_t>(lba >> 8);
  cdb[5] = static_cast<uint8_t>(lba);
  cdb[6] = 0;
  cdb[7] = static_cast<uint8_t>(blocks >> 8);
  cdb[8] = static_cast<uint8_t>(blocks);
  cdb[9] = 0;

  uint32_t ocs = kOcsInvalid;
  if (!runUtp(tag, utpCommandType(false), kUtpDeviceToHost, bytes, 5000, &ocs)) {
    lastReadTransportFailure = true;
    return false;
  }
  if (ocs != 0) {
    lastReadTransportFailure = true;
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    kprintf("NativeMtkUfs: READ(10) lba=%llu blocks=%u OCS=0x%x\n",
            static_cast<unsigned long long>(lba), blocks, ocs);
    return false;
  }

  const uint8_t *response = ucd + kUpiuResponseOffset;
  if (response[0] != 0x21 || response[2] != kUserLun || response[3] != tag) {
    kprintf("NativeMtkUfs: READ(10) response identity mismatch type=%02x lun=%u tag=%u\n",
            response[0], response[2], response[3]);
    return false;
  }
  const uint32_t responseCode = response[6];
  const uint32_t scsiStatus = response[7];
  if (responseCode != 0 || scsiStatus != 0) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    kprintf("NativeMtkUfs: READ(10) lba=%llu blocks=%u response=0x%02x scsi=0x%02x\n",
            static_cast<unsigned long long>(lba), blocks, responseCode, scsiStatus);
    // utp_cmd_rsp: 12-byte UPIU header, residual + four reserved dwords,
    // then big-endian sense length at 32 and sense bytes at 34.
    const uint32_t senseLength = getBe16(response + 32);
    if (senseLength >= 4 && senseLength <= 18) {
      const uint8_t *sense = response + 34;
      const uint8_t code = sense[0] & 0x7FU;
      uint8_t key = 0xFF, asc = 0xFF, ascq = 0xFF;
      if ((code == 0x70 || code == 0x71) && senseLength >= 14) {
        key = sense[2] & 0xFU;
        asc = sense[12];
        ascq = sense[13];
      } else if (code == 0x72 || code == 0x73) {
        key = sense[1] & 0xFU;
        asc = sense[2];
        ascq = sense[3];
      }
      kprintf("NativeMtkUfs: LUN %u sense len=%u code=%02x key=%02x ASC=%02x ASCQ=%02x\n",
              kUserLun, senseLength, code, key, asc, ascq);
      lastScsiSenseKey = key;
    }
    return false;
  }

  // Do not write the device-owned cache lines back over the read data.
  if (out) {
    memmove(out, dataBytesPtr, bytes);
  }
  native_mtkufs_reads = native_mtkufs_reads + 1;
  return true;
}

// ---------------------------------------------------------------------------
// WRITE(10): the expdb console log window
// ---------------------------------------------------------------------------
// Same UTP transfer request as the read path with three differences, all of
// which the ABI pins down: CDB opcode 0x0A instead of 0x28, UPIU command
// flags 0x20 (WRITE) instead of 0x40 (READ), and a host-to-device UTRD
// direction of 0x02000000 instead of 0x04000000.  runUtp() already cleans the
// data buffer before the doorbell, which is what a write direction needs, and
// invalidates the UTRD while polling, which stays correct.
//
// Refuses when the LU reported bLUWriteProtect, so a mis-set window can never
// turn into a stream of sense errors.
bool NativeMtkUfs::writeDeviceBlocks(uint64_t lba, uint32_t blocks, const void *in)
{
  if (!gUtpGate) {
    return writeDeviceBlocksLocked(lba, blocks, in);
  }
  utpGateLockTracked();
  const bool ok = writeDeviceBlocksLocked(lba, blocks, in);
  utpGateUnlockTracked();
  return ok;
}

// The log window is the only caller, and it is the only writer this driver
// accepts.  lba is an absolute LU2 LBA; the range check here is the last line
// of defence against a log-writer bug spilling out of expdb.
bool NativeMtkUfs::logWriteBlocks(uint64_t lba, uint32_t blocks, const void *in)
{
  if (lba < kLogWindowFirstLba || lba > kLogWindowLastLba ||
      blocks > kLogWindowLastLba - lba + 1ULL) {
    native_mtkufs_log_errors = native_mtkufs_log_errors + 1;
    kprintf("NativeMtkUfs: log write refused lba=%llu blocks=%u (window %llu..%llu)\n",
            static_cast<unsigned long long>(lba), blocks,
            static_cast<unsigned long long>(kLogWindowFirstLba),
            static_cast<unsigned long long>(kLogWindowLastLba));
    return false;
  }
  return writeDeviceBlocks(lba, blocks, in);
}

bool NativeMtkUfs::writeDeviceBlocksLocked(uint64_t lba, uint32_t blocks, const void *in)
{
  lastScsiSenseKey = 0xFF;
  if (!utpReady || !deviceBlockSize || !in || blocks == 0 || blocks > 0xFFFFU) {
    return false;
  }
  if (unitWriteProtect != 0) {
    native_mtkufs_write_refused = native_mtkufs_write_refused + 1;
    return false;
  }
  if (lba > 0xFFFFFFFFULL || blocks - 1ULL > 0xFFFFFFFFULL - lba) {
    kprintf("NativeMtkUfs: WRITE(10) LBA 0x%llx exceeds the 32-bit CDB field\n",
            static_cast<unsigned long long>(lba));
    return false;
  }
  if (deviceCapacityBlocks &&
      (lba >= deviceCapacityBlocks || blocks > deviceCapacityBlocks - lba)) {
    return false;
  }
  const uint64_t bytes64 = static_cast<uint64_t>(blocks) * deviceBlockSize;
  if (bytes64 == 0 || bytes64 > kDataBytes || bytes64 > 0xFFFFFFFFULL) {
    return false;
  }
  const uint32_t bytes = static_cast<uint32_t>(bytes64);

  // Copy into the DMA bounce buffer before runUtp cleans it.
  memcpy(dataBytesPtr, in, bytes);

  const unsigned tag = 0;
  uint8_t *ucd = ucdAt(tag);
  bzero(ucd, kUcdPayload);

  ucd[0] = static_cast<uint8_t>(kUpiuCommand);
  ucd[1] = static_cast<uint8_t>(kUpiuCmdFlagsWrite);
  ucd[2] = kUserLun;
  ucd[3] = static_cast<uint8_t>(tag);
  putBe32(ucd + 12, bytes);  // expected data transfer length, big endian
  uint8_t *cdb = ucd + 16;
  // 0x2A is WRITE(10); 0x0A is WRITE(6), whose CDB is 6 bytes, and the device
  // answered the 10-byte form with ILLEGAL REQUEST / INVALID FIELD IN CDB
  // (key 05 ASC 24) when this said 0x0A.
  cdb[0] = 0x2A;  // WRITE(10)
  // No FUA here: the same UFS class of device rejected it.  Durability comes
  // from an explicit SYNCHRONIZE CACHE(10) after the payload instead.
  cdb[1] = 0;
  cdb[2] = static_cast<uint8_t>(lba >> 24);
  cdb[3] = static_cast<uint8_t>(lba >> 16);
  cdb[4] = static_cast<uint8_t>(lba >> 8);
  cdb[5] = static_cast<uint8_t>(lba);
  cdb[6] = 0;
  cdb[7] = static_cast<uint8_t>(blocks >> 8);
  cdb[8] = static_cast<uint8_t>(blocks);
  cdb[9] = 0;

  uint32_t ocs = kOcsInvalid;
  if (!runUtp(tag, utpCommandType(false), kUtpHostToDevice, bytes, 5000, &ocs)) {
    return false;
  }
  if (ocs != 0) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    native_mtkufs_write_errors = native_mtkufs_write_errors + 1;
    kprintf("NativeMtkUfs: WRITE(10) lba=%llu blocks=%u OCS=0x%x\n",
            static_cast<unsigned long long>(lba), blocks, ocs);
    return false;
  }

  const uint8_t *response = ucd + kUpiuResponseOffset;
  if (response[0] != 0x21 || response[2] != kUserLun || response[3] != tag) {
    kprintf("NativeMtkUfs: WRITE(10) response identity mismatch type=%02x lun=%u tag=%u\n",
            response[0], response[2], response[3]);
    return false;
  }
  const uint32_t responseCode = response[6];
  const uint32_t scsiStatus = response[7];
  if (responseCode != 0 || scsiStatus != 0) {
    native_mtkufs_utp_errors = native_mtkufs_utp_errors + 1;
    native_mtkufs_write_errors = native_mtkufs_write_errors + 1;
    const uint32_t senseLength = getBe16(response + 32);
    uint8_t key = 0xFF, asc = 0xFF, ascq = 0xFF;
    if (senseLength >= 4 && senseLength <= 18) {
      const uint8_t *sense = response + 34;
      const uint8_t code = sense[0] & 0x7FU;
      if ((code == 0x70 || code == 0x71) && senseLength >= 14) {
        key = sense[2] & 0xFU;
        asc = sense[12];
        ascq = sense[13];
      } else if (code == 0x72 || code == 0x73) {
        key = sense[1] & 0xFU;
        asc = sense[2];
        ascq = sense[3];
      }
      lastScsiSenseKey = key;
    }
    kprintf("NativeMtkUfs: WRITE(10) lba=%llu blocks=%u response=0x%02x scsi=0x%02x "
            "sense key=%02x ASC=%02x ASCQ=%02x\n",
            static_cast<unsigned long long>(lba), blocks, responseCode, scsiStatus,
            key, asc, ascq);
    return false;
  }

  native_mtkufs_writes = native_mtkufs_writes + 1;
  return true;
}

// SYNCHRONIZE CACHE(10) over the whole LU: everything written so far reaches
// the medium.  The log writer calls this once per round after the payload and
// header are out, so a reset cannot leave the header naming bytes that are
// still in the device's write cache.
bool NativeMtkUfs::syncDeviceCache()
{
  if (!gUtpGate) {
    return syncDeviceCacheLocked();
  }
  utpGateLockTracked();
  const bool ok = syncDeviceCacheLocked();
  utpGateUnlockTracked();
  return ok;
}

bool NativeMtkUfs::syncDeviceCacheLocked()
{
  if (!utpReady) {
    return false;
  }
  const unsigned tag = 0;
  uint8_t *ucd = ucdAt(tag);
  bzero(ucd, kUcdPayload);

  ucd[0] = static_cast<uint8_t>(kUpiuCommand);
  ucd[1] = 0;  // no data
  ucd[2] = kUserLun;
  ucd[3] = static_cast<uint8_t>(tag);
  putBe32(ucd + 12, 0);
  uint8_t *cdb = ucd + 16;
  cdb[0] = 0x35;  // SYNCHRONIZE CACHE(10), whole LUN
  // cdb[1..9] stay zero: no IMMED, LBA 0, length 0 (means "to the end").

  uint32_t ocs = kOcsInvalid;
  if (!runUtp(tag, utpCommandType(false), kUtpNoData, 0, 5000, &ocs)) {
    return false;
  }
  if (ocs != 0) {
    kprintf("NativeMtkUfs: SYNCHRONIZE CACHE OCS=0x%x\n", ocs);
    return false;
  }
  const uint8_t *response = ucd + kUpiuResponseOffset;
  if (response[6] != 0 || response[7] != 0) {
    kprintf("NativeMtkUfs: SYNCHRONIZE CACHE response=0x%02x scsi=0x%02x\n",
            response[6], response[7]);
    return false;
  }
  native_mtkufs_syncs = native_mtkufs_syncs + 1;
  return true;
}

bool NativeMtkUfs::proveLbaZero()
{
  bool read = false;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (readDeviceBlocks(0, 1, dataBytesPtr)) {
      read = true;
      break;
    }
    if (lastScsiSenseKey != 0x06) {
      break;
    }
    // A reset may leave UNIT ATTENTION pending. The failed READ consumes it;
    // retry only that explicit condition, never an arbitrary CHECK CONDITION.
    kprintf("NativeMtkUfs: LUN %u UNIT ATTENTION, retrying read (%u/3)\n",
            kUserLun, attempt + 1);
    IOSleep(10);
  }
  if (!read) {
    // Sense above distinguishes NOT READY, UNIT ATTENTION and illegal requests.
    // Do not guess a power-state transition or silently ignore CHECK CONDITION.
    kprintf("NativeMtkUfs: stage 9 LBA 0 read failed\n");
    return false;
  }
  const uint8_t *first = dataBytesPtr;
  native_mtkufs_stage = 9;  // stage 9: UTP read of LBA 0 succeeded
  kprintf("NativeMtkUfs: stage 9 LBA 0 read ok, first bytes %02x %02x %02x %02x\n",
          static_cast<unsigned>(first[0]), static_cast<unsigned>(first[1]),
          static_cast<unsigned>(first[2]), static_cast<unsigned>(first[3]));
  return true;
}

// 512-byte sector view on top of the device's logical block size.  Handles
// unaligned requests by reading whole device blocks through the bounce buffer.
IOReturn NativeMtkUfs::readSectors(uint64_t sector, uint32_t count, void *out)
{
  if (!utpReady || !deviceBlockSize || !out || count == 0) {
    return kIOReturnNotReady;
  }
  if (sector >= mediaSectors || count > mediaSectors - sector) {
    return kIOReturnBadArgument;
  }

  const uint64_t byteStart = sector * 512ULL;
  const uint64_t byteEnd = byteStart + static_cast<uint64_t>(count) * 512ULL;
  const uint32_t maxBlocks = kDataBytes / deviceBlockSize;
  if (maxBlocks == 0) {
    return kIOReturnBadArgument;
  }

  uint8_t *destination = static_cast<uint8_t *>(out);
  uint64_t cursor = byteStart;
  while (cursor < byteEnd) {
    const uint64_t lba = windowBaseLba + cursor / deviceBlockSize;
    const uint32_t inBlock = static_cast<uint32_t>(cursor % deviceBlockSize);
    const uint64_t remaining = byteEnd - cursor;
    const uint64_t needed =
        (static_cast<uint64_t>(inBlock) + remaining + deviceBlockSize - 1) /
        deviceBlockSize;
    const uint32_t take =
        (needed > maxBlocks) ? maxBlocks : static_cast<uint32_t>(needed);

    const uint32_t available = take * deviceBlockSize - inBlock;
    const uint32_t copyBytes =
        (available > remaining) ? static_cast<uint32_t>(remaining) : available;
    /* fb147: same shared-bounce race as doAsyncReadWrite -- the copy out of
     * the bounce must not race another window's DMA, so read + copy happen
     * in one UTP-gate hold via the locked variant. */
    NativeMtkUfs *io = (secondaryWindow && gBlockMedia && gBlockMedia != this) ? gBlockMedia : this;
    if (gUtpGate) {
      utpGateLockTracked();
    }
    const bool ok = io->readDeviceBlocksLocked(lba, take, dataBytesPtr);
    if (ok) {
      memcpy(destination, dataBytesPtr + inBlock, copyBytes);
    }
    if (gUtpGate) {
      utpGateUnlockTracked();
    }
    if (!ok) {
      return kIOReturnIOError;
    }
    destination += copyBytes;
    cursor += copyBytes;
  }
  patchMediaSectors(sector, count, out);
  return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Start / stop / free
// ---------------------------------------------------------------------------
// Dump the device GPT: what partitions exist besides `super`, and which of
// them start with an APFS container superblock (the SystemOS cryptex would).
void NativeMtkUfs::dumpGptEntries()
{
  static uint8_t hdr[4096];
  static uint8_t entries[128 * 128];
  static uint8_t probe[4096];

  if (!readDeviceBlocks(1, 1, hdr) || memcmp(hdr, "EFI PART", 8) != 0) {
    kprintf("NativeMtkUfs: gpt header unreadable\n");
    return;
  }
  const uint32_t count = getLe32(hdr + 80);
  const uint32_t esz = getLe32(hdr + 84);
  const uint64_t entriesLba = getLe64(hdr + 72);
  if (count == 0 || count > 128 || esz < 128 || esz > 512) {
    kprintf("NativeMtkUfs: gpt geometry bad count=%u esz=%u\n", count, esz);
    return;
  }
  const uint32_t bytes = count * esz;
  const uint32_t blocks = (bytes + deviceBlockSize - 1) / deviceBlockSize;
  if (blocks * deviceBlockSize > sizeof(entries) ||
      !readDeviceBlocks(entriesLba, blocks, entries)) {
    kprintf("NativeMtkUfs: gpt entries unreadable lba=%llu blocks=%u\n",
            static_cast<unsigned long long>(entriesLba), blocks);
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *e = entries + i * esz;
    const uint64_t first = getLe64(e + 32);
    const uint64_t last = getLe64(e + 40);
    if (first == 0 || last < first) {
      continue;
    }
    char name[37];
    unsigned n = 0;
    for (unsigned k = 0; k < 36 && e[56 + 2 * k] != 0; ++k) {
      name[n++] = static_cast<char>(e[56 + 2 * k]);
    }
    name[n] = 0;
    bool apfs = false;
    if (readDeviceBlocks(first, 1, probe)) {
      apfs = memcmp(probe + 0x20, "NXSB", 4) == 0;
    }
    kprintf("NativeMtkUfs: gpt[%u] %s first=%llu last=%llu mib=%llu apfs=%d\n",
            i + 1, name,
            static_cast<unsigned long long>(first),
            static_cast<unsigned long long>(last),
            static_cast<unsigned long long>(((last - first + 1) * deviceBlockSize) >> 20),
            apfs ? 1 : 0);
  }
}

bool NativeMtkUfs::prepareApfsCompatibility()
{
  if (!readDeviceBlocks(0, 1, gGptHeader) ||
      gGptHeader[510] != 0x55 || gGptHeader[511] != 0xAA) {
    kprintf("NativeMtkUfs: APFS shim: protective MBR unavailable\n");
    return false;
  }
  if (!readDeviceBlocks(1, 1, gGptHeader) ||
      memcmp(gGptHeader, "EFI PART", 8) != 0) {
    kprintf("NativeMtkUfs: APFS shim: primary GPT header unavailable\n");
    return false;
  }

  const uint32_t headerBytes = getLe32(gGptHeader + 12);
  const uint64_t entriesLba = getLe64(gGptHeader + 72);
  const uint32_t entriesCount = getLe32(gGptHeader + 80);
  const uint32_t entryBytes = getLe32(gGptHeader + 84);
  const uint32_t entriesCrc = getLe32(gGptHeader + 88);
  if (headerBytes < 92 || headerBytes > sizeof(gGptHeader) ||
      entriesCount == 0 || entryBytes < 128 || entryBytes > 4096 ||
      entriesCount > sizeof(gGptEntries) / entryBytes) {
    kprintf("NativeMtkUfs: APFS shim: invalid GPT geometry hdr=%u count=%u entry=%u\n",
            headerBytes, entriesCount, entryBytes);
    return false;
  }
  const uint32_t entriesBytes = entriesCount * entryBytes;

  const uint32_t blocks = (entriesBytes + deviceBlockSize - 1) / deviceBlockSize;
  if (!readDeviceBlocks(entriesLba, blocks, dataBytesPtr) ||
      gptCrc32(dataBytesPtr, entriesBytes) != entriesCrc) {
    kprintf("NativeMtkUfs: APFS shim: GPT entries read/CRC failed lba=%llu blocks=%u\n",
            static_cast<unsigned long long>(entriesLba), blocks);
    return false;
  }
  memcpy(gGptEntries, dataBytesPtr, entriesBytes);

  bool found = false;
  for (uint32_t offset = 0; offset + entryBytes <= entriesBytes; offset += entryBytes) {
    const uint8_t *name = gGptEntries + offset + 56;
    const bool isSuper = name[1] == 0 && name[3] == 0 && name[5] == 0 &&
                         name[7] == 0 && name[9] == 0 &&
                         (name[0] == 's' || name[0] == 'S') &&
                         (name[2] == 'u' || name[2] == 'U') &&
                         (name[4] == 'p' || name[4] == 'P') &&
                         (name[6] == 'e' || name[6] == 'E') &&
                         (name[8] == 'r' || name[8] == 'R');
    if (!isSuper) {
      continue;
    }
    memcpy(gGptEntries + offset, kAppleApfsType, sizeof(kAppleApfsType));
    found = true;
    kprintf("NativeMtkUfs: APFS shim: super entry=%u start=%llu end=%llu\n",
            offset / entryBytes + 1,
            static_cast<unsigned long long>(getLe64(gGptEntries + offset + 32)),
            static_cast<unsigned long long>(getLe64(gGptEntries + offset + 40)));
    break;
  }
  if (!found) {
    kprintf("NativeMtkUfs: APFS shim: no super partition in GPT\n");
    return false;
  }

  putLe32(gGptHeader + 88, gptCrc32(gGptEntries, entriesBytes));
  putLe32(gGptHeader + 16, 0);
  putLe32(gGptHeader + 16, gptCrc32(gGptHeader, headerBytes));
  gGptEntriesLba = entriesLba;
  gGptEntriesBytes = entriesBytes;
  gGptEntriesBlocks = blocks;
  gGptCompatReady = true;
  kprintf("NativeMtkUfs: APFS shim ready, GPT exposed with super as Apple APFS\n");
  return true;
}

void NativeMtkUfs::patchMediaSectors(uint64_t sector, uint32_t count, void *out) const
{
  (void)sector; (void)count; (void)out;
  return;  // Plan A: the media is the APFS window; the GPT shim is not presented.
  if (!gGptCompatReady || !out || count == 0) {
    return;
  }
  if (sector == 0) {
    // Present a canonical protective MBR to IOGUIDPartitionScheme. Some
    // Android GPT layouts do not put the 0xEE entry where Darwin expects it.
    uint8_t *block = static_cast<uint8_t *>(out);
    bzero(block + 446, 64);
    block[446 + 4] = 0xEE;
    block[510] = 0x55;
    block[511] = 0xAA;
  }
  const uint64_t end = sector + count;
  if (sector <= 1 && end > 1) {
    memcpy(static_cast<uint8_t *>(out) + (1 - sector) * deviceBlockSize,
           gGptHeader, sizeof(gGptHeader));
  }
  const uint64_t entriesEnd = gGptEntriesLba + gGptEntriesBlocks;
  if (end > gGptEntriesLba && sector < entriesEnd) {
    const uint64_t first = sector > gGptEntriesLba ? sector : gGptEntriesLba;
    const uint64_t last = end < entriesEnd ? end : entriesEnd;
    memcpy(static_cast<uint8_t *>(out) + (first - sector) * deviceBlockSize,
           gGptEntries + (first - gGptEntriesLba) * deviceBlockSize,
           (last - first) * deviceBlockSize);
  }
}

bool NativeMtkUfs::installBlockMediaDispatch()
{
  auto name = OSSymbol::withCStringNoCopy("IOBlockStorageDevice");
  gBlockMediaMeta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
  if (!gBlockMediaMeta) {
    kprintf("NativeMtkUfs: IOBlockStorageDevice metaclass unavailable\n");
    return false;
  }

  // The object was attached before this point, so its registry class still
  // names NativeMtkUfs. Make that real metaclass a child of the concrete
  // storage provider class; matching then uses the normal I/O Kit IS-A path.
  constexpr unsigned kMetaSuperLinkOffset = 16;
  auto *meta = reinterpret_cast<OSMetaClass *>(&gMetaClass);
  *reinterpret_cast<const OSMetaClass **>(
      reinterpret_cast<uint8_t *>(meta) + kMetaSuperLinkOffset) = gBlockMediaMeta;

  if (!secondaryWindow) {
    gBlockMedia = this;
  }  uintptr_t *nativeDispatch = *reinterpret_cast<uintptr_t **>(this);
  for (unsigned slot = 0; slot < 168; ++slot) {
    gBlockMediaDispatch[slot] = nativeDispatch[slot];
  }
  gBlockMediaDispatch[168] = reinterpret_cast<uintptr_t>(&blockEjectMedia);
  gBlockMediaDispatch[169] = reinterpret_cast<uintptr_t>(&blockFormatMedia);
  gBlockMediaDispatch[170] = reinterpret_cast<uintptr_t>(&blockFormatCapacities);
  gBlockMediaDispatch[171] = reinterpret_cast<uintptr_t>(&blockSynchronizeCache);
  gBlockMediaDispatch[172] = reinterpret_cast<uintptr_t>(&blockVendorString);
  gBlockMediaDispatch[173] = reinterpret_cast<uintptr_t>(&blockProductString);
  gBlockMediaDispatch[174] = reinterpret_cast<uintptr_t>(&blockRevisionString);
  gBlockMediaDispatch[175] = reinterpret_cast<uintptr_t>(&blockAdditionalInfo);
  gBlockMediaDispatch[176] = reinterpret_cast<uintptr_t>(&blockReportBlockSize);
  gBlockMediaDispatch[177] = reinterpret_cast<uintptr_t>(&blockReportEjectability);
  gBlockMediaDispatch[178] = reinterpret_cast<uintptr_t>(&blockReportMaxValidBlock);
  gBlockMediaDispatch[179] = reinterpret_cast<uintptr_t>(&blockReportMediaState);
  gBlockMediaDispatch[180] = reinterpret_cast<uintptr_t>(&blockReportRemovability);
  gBlockMediaDispatch[181] = reinterpret_cast<uintptr_t>(&blockReportWriteProtection);
  gBlockMediaDispatch[182] = reinterpret_cast<uintptr_t>(&blockGetWriteCacheState);
  gBlockMediaDispatch[183] = reinterpret_cast<uintptr_t>(&blockSetWriteCacheState);
  gBlockMediaDispatch[184] = reinterpret_cast<uintptr_t>(&blockAsyncReadWrite);
  gBlockMediaDispatch[185] = reinterpret_cast<uintptr_t>(&blockRequestIdle);
  gBlockMediaDispatch[186] = reinterpret_cast<uintptr_t>(&blockUnmap);
  gBlockMediaDispatch[187] = reinterpret_cast<uintptr_t>(&blockSetPriority);
  gBlockMediaDispatch[188] = reinterpret_cast<uintptr_t>(&blockSynchronize);
  gBlockMediaDispatch[189] = reinterpret_cast<uintptr_t>(&blockGetProvisionStatus);
  *reinterpret_cast<uintptr_t **>(this) = gBlockMediaDispatch;
  if (!setProperty(kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric)) {
    kprintf("NativeMtkUfs: cannot publish Generic block-device type\n");
    return false;
  }
  kprintf("NativeMtkUfs: block dispatch installed, meta=%p\n", gBlockMediaMeta);
  return true;
}

// The media object itself is owned by IOBlockStorageDriver, so it is created
// without the properties an APFS container provider matches on. Add them and
// re-run matching: registerService() -> startMatching() wakes the APFS waiter,
// which then re-checks the media's current properties.
IOService *gPatchedMedia[8];
unsigned gPatchedMediaCount = 0;

unsigned NativeMtkUfs::setApfsMediaProperties()
{
  const uint64_t windowSize = windowBytes();
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  if (!matching) {
    return 0;
  }
  OSIterator *iterator = IOService::getMatchingServices(matching);
  matching->release();
  if (!iterator) {
    return 0;
  }
  unsigned patched = 0;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) {
      continue;
    }
    // Only this instance's media: IOMedia -> IOBlockStorageDriver -> device.
    // Unknown chains are left to the size check below (legacy behaviour).
    {
      IOService *drv = media->getProvider();
      IOService *dev = drv ? drv->getProvider() : nullptr;
      if (dev && dev->getMetaClass() == &gMetaClass && dev != this) {
        continue;
      }
    }
    OSNumber *size = OSDynamicCast(OSNumber, media->getProperty("Size"));
    if (!size || size->unsigned64BitValue() != windowSize) {
      continue;
    }
    // Never touch the same media twice: terminating the IOStorage client or
    // re-registering it while apfs is mounting corrupts its busy count.
    bool seen = false;
    for (unsigned i = 0; i < gPatchedMediaCount; ++i) {
      if (gPatchedMedia[i] == media) {
        seen = true;
        break;
      }
    }
    if (seen) {
      continue;
    }
    // Already carrying our APFS mark - remember it and leave it alone.
    OSString *hintNow = OSDynamicCast(OSString, media->getProperty("Content Hint"));
    if (hintNow && strcmp(hintNow->getCStringNoCopy(),
        "7C3457EF-0000-11AA-AA11-00306543ECAC") == 0) {
      if (gPatchedMediaCount < 8) {
        gPatchedMedia[gPatchedMediaCount++] = media;
      }
      continue;
    }
    // A driver already attached in the IOStorage match category keeps every
    // later personality (AppleAPFSContainerScheme) out of the category. Drop
    // the idle partition-scheme client first, then rematch.
    const OSSymbol *storageCategory = OSSymbol::withCString("IOStorage");
    if (storageCategory) {
      IOService *holder = media->copyClientWithCategory(storageCategory);
      if (holder) {
        const char *holderClass = holder->getMetaClass() ?
            holder->getMetaClass()->getClassName() : "";
        // apfs already owns this media: leaving it alone is the whole point.
        if (holderClass && strncmp(holderClass, "AppleAPFS", 9) == 0) {
          kprintf("NativeMtkUfs: media already held by %s, not touching it\n", holderClass);
          if (gPatchedMediaCount < 8) {
            gPatchedMedia[gPatchedMediaCount++] = media;
          }
          holder->release();
          storageCategory->release();
          continue;
        }
        kprintf("NativeMtkUfs: IOStorage held by %s, terminating\n", holderClass);
        holder->terminate(kIOServiceSynchronous);
        holder->release();
      }
      storageCategory->release();
    }
    // Present a partition-media shape: whole disks belong to the GPT
    // partition schemes (IOMatchCategory IOStorage), and whichever one
    // probes first would keep AppleAPFSContainerScheme out of the category.
    media->setProperty("Whole", kOSBooleanFalse);
    media->setProperty("Leaf", kOSBooleanTrue);
    media->setProperty("Content", "Apple_APFS_Container");
    media->setProperty("Content Hint", "7C3457EF-0000-11AA-AA11-00306543ECAC");
    media->registerService();
    if (gPatchedMediaCount < 8) {
      gPatchedMedia[gPatchedMediaCount++] = media;
    }
    ++patched;
  }
  iterator->release();
  kprintf("NativeMtkUfs: APFS media properties patched=%u\n", patched);
  return patched;
}

// Re-register media that already carry our APFS mark. No property change and no
// client termination: this only gives the apfs matcher another chance when its
// first probe lost the race against the media being published.
[[maybe_unused]] void NativeMtkUfs::kickApfsMatch()
{
  const uint64_t windowSize = windowBytes();
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  if (!matching) {
    return;
  }
  OSIterator *iterator = IOService::getMatchingServices(matching);
  matching->release();
  if (!iterator) {
    return;
  }
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) {
      continue;
    }
    {
      IOService *drv = media->getProvider();
      IOService *dev = drv ? drv->getProvider() : nullptr;
      if (dev && dev->getMetaClass() == &gMetaClass && dev != this) {
        continue;
      }
    }
    OSNumber *size = OSDynamicCast(OSNumber, media->getProperty("Size"));
    OSString *hint = OSDynamicCast(OSString, media->getProperty("Content Hint"));
    if (!size || size->unsigned64BitValue() != windowSize || !hint) {
      continue;
    }
    if (strcmp(hint->getCStringNoCopy(), "7C3457EF-0000-11AA-AA11-00306543ECAC") == 0) {
      media->registerService();
    }
  }
  iterator->release();
}

// ---------------------------------------------------------------------------
// Console log writer: serial_console.c ring -> expdb
// ---------------------------------------------------------------------------
// serial_console.c keeps a 512 KiB ring of every console byte (see
// yunluo_log_append).  This thread drains it onto the expdb window every two
// seconds, and the very first round also carries the bytes the kernel printed
// before this driver existed.
//
// Ordering matters for the reader: the payload blocks go out first and the
// header block last, so a header that names N bytes always has all N of them
// already on the medium.  A write failure stops the thread instead of retrying
// forever, and the reason is left in native_mtkufs_log_errors.
extern "C" uint64_t yunluo_log_position(void);
extern "C" uint32_t yunluo_log_copy(uint64_t *, char *, uint32_t);
extern "C" uint32_t yunluo_log_wrapped(void);
// Copies the whole live ring, not just what is new: each round rewrites the
// log from the start, which keeps the medium layout a single contiguous blob
// and makes the panic-time flush the same code path as a normal round.
extern "C" uint32_t yunluo_log_available(uint64_t *start);

namespace {

constexpr uint32_t kLogChunkBytes = 32u * 1024u;
uint8_t gLogBuffer[kLogChunkBytes];
uint64_t gLogRounds = 0;

struct YunluoLogHeader {
  char magic[8];      /* "YUNLUOG1" */
  uint32_t version;   /* 1 */
  uint32_t header_bytes;
  uint64_t window_first_lba;
  uint64_t window_blocks;
  uint64_t total_bytes;   /* bytes ever appended to the console ring */
  uint64_t rounds;
  uint32_t wrapped;       /* ring overflowed at least once */
  uint32_t errors;
  char note[64];
};

}  // namespace

// One synchronous snapshot: header block, then the whole console ring as a
// contiguous run of blocks, then the header again so it names only payload that
// is already on the medium, then a cache flush.  The caller holds gUtpGate.
//
// Rewriting the whole ring each round costs about 7 blocks for a normal boot
// (the console produces ~27 KiB) and buys two things: the reader never has to
// deal with per-slot padding or stale slots from an earlier boot, and the panic
// path can reuse this function verbatim.
bool NativeMtkUfs::logWriteSnapshotLocked(uint64_t *outTotal)
{
  uint64_t start = 0;
  uint32_t n = yunluo_log_available(&start);
  const uint64_t maxBytes = (kLogWindowBlocks - 1ULL) * 4096ULL;
  if (static_cast<uint64_t>(n) > maxBytes) {
    n = static_cast<uint32_t>(maxBytes);
  }
  uint64_t pos = start;
  uint32_t writtenBlocks = 0;
  uint32_t remaining = n;
  while (remaining > 0) {
    const uint32_t want = remaining > kLogChunkBytes ? kLogChunkBytes : remaining;
    bzero(gLogBuffer, sizeof(gLogBuffer));
    const uint32_t got = yunluo_log_copy(&pos, reinterpret_cast<char *>(gLogBuffer), want);
    if (got == 0) {
      break;
    }
    const uint32_t blocks = (got + 4095u) / 4096u;
    if (1u + writtenBlocks + blocks > kLogWindowBlocks) {
      break;
    }
    if (!writeDeviceBlocksLocked(kLogWindowFirstLba + 1u + writtenBlocks, blocks, gLogBuffer)) {
      return false;
    }
    writtenBlocks += blocks;
    remaining -= got;
  }

  YunluoLogHeader h;
  bzero(&h, sizeof(h));
  memcpy(h.magic, "YUNLUOG1", 8);
  h.version = 1;
  h.header_bytes = sizeof(h);
  h.window_first_lba = kLogWindowFirstLba;
  h.window_blocks = kLogWindowBlocks;
  h.total_bytes = yunluo_log_position();
  h.rounds = ++gLogRounds;
  h.wrapped = yunluo_log_wrapped();
  h.errors = native_mtkufs_log_errors;
  snprintf(h.note, sizeof(h.note), "owner=0x%llx held_ms=%llu %s",
      (unsigned long long)gUtpGateOwner,
      (unsigned long long)utpGateHeldMs(),
      gLogNote[0] ? gLogNote : "");
  bzero(gLogBuffer, 4096);
  memcpy(gLogBuffer, &h, sizeof(h));
  if (!writeDeviceBlocksLocked(kLogWindowFirstLba, 1, gLogBuffer)) {
    return false;
  }
  if (outTotal) {
    *outTotal = h.total_bytes;
  }
  return syncDeviceCacheLocked();
}

// Called from the panic path right after the panic message has been written to
// the console, i.e. after it is already in the ring.  Other CPUs are halted by
// then, so this is the last chance to get that text onto the medium.  It must
// never block: if the UTP gate is held (panic inside the driver) give up rather
// than deadlock the panic.
//
// A small marker goes to the last block of the window *before* anything else,
// because the interesting failure is this function running at all and then
// failing silently.  The reader reports it, so a boot whose log stops early
// still says whether the flush was attempted and why it gave up:
//   1 driver or window not up, 2 UTP gate busy, 3 wrote marker but the
//   snapshot failed, 4 snapshot complete.
struct YunluoPanicMarker {
  char magic[8];        /* "YUNLOPNC" */
  uint32_t state;
  uint32_t pad;
  uint64_t total_bytes;
  uint64_t attempts;
};

static bool writePanicMarkerLocked(uint32_t state)
{
  return gBlockMedia->logWritePanicMarkerLocked(state);
}

bool NativeMtkUfs::logWritePanicMarkerLocked(uint32_t state)
{
  YunluoPanicMarker m;
  bzero(&m, sizeof(m));
  memcpy(m.magic, "YUNLOPNC", 8);
  m.state = state;
  m.total_bytes = yunluo_log_position();
  m.attempts = ++gLogRounds;
  return writeDeviceBlocksLocked(kLogWindowLastLba, 1, &m);
}

extern "C" void yunluo_panic_flush(void)
{
  uint32_t state = 1;
  if (gBlockMedia && gLogOpen && gUtpGate) {
    state = 2;
    // Never yield a panic/debugger CPU. A stopped lock owner cannot release it.
    const bool got = IOLockTryLock(gUtpGate);
    if (got) {
      yunluo_panic_flush_state = 2;
      (void)writePanicMarkerLocked(3);   /* marker first: it survives a later failure */
      state = gBlockMedia->logWriteSnapshotLocked(nullptr) ? 4 : 3;
      (void)writePanicMarkerLocked(state);
      IOLockUnlock(gUtpGate);
    }
  }
  yunluo_panic_flush_state = state;
}

[[maybe_unused]] static void logWriterThread(void *arg, wait_result_t)
{
  NativeMtkUfs *drv = static_cast<NativeMtkUfs *>(arg);
  if (!drv) {
    return;
  }
  // On by default: the whole point is to capture a boot that ends in a panic,
  // which by definition cannot be predicted in the boot-arg.  yunluo-log=0
  // turns it off for a run that must not touch expdb.
  uint32_t enabled = 1;
  PE_parse_boot_argn("yunluo-log", &enabled, sizeof(enabled));
  if (enabled == 0) {
    kprintf("NativeMtkUfs: expdb log writer off (yunluo-log=0)\n");
    return;
  }
  // Give the root mount and the first init exec time to settle, and let the
  // boot-arg parse happen after PE is up.
  IOSleep(3000);

  if (!drv->logWindowReady()) {
    kprintf("NativeMtkUfs: expdb log writer: device not ready\n");
    return;
  }
  if (drv->logWindowWriteProtected()) {
    kprintf("NativeMtkUfs: expdb log writer: LU is write protected, not writing\n");
    native_mtkufs_log_errors = native_mtkufs_log_errors + 1;
    return;
  }

  // The cryptex volume has to be attached before init exec, and APFS gets only
  // about 20 s to find it.  Every expdb write steals the single UTP tag from
  // that search, and an earlier image proved the cost: reads crawled 20 per
  // round and the volume never attached in time.  So hold off completely until
  // the cryptex device name shows up (or the search has definitively failed),
  // then start writing.  Nothing is lost: the console ring keeps collecting.
  //
  // The hard floor below matters as much as the wait: init exec, the root mount
  // and libSystem bring-up all happen in the first seconds after preboot, and
  // an expdb write that lands in the middle of them costs far more than the
  // couple of seconds of log it buys.
  {
    uint64_t base = static_cast<uint64_t>(mach_absolute_time());
    uint64_t waitNs = 0;
    absolutetime_to_nanoseconds(600ULL * 1000ULL * 1000ULL * 1000ULL, &waitNs);
    const uint64_t deadline = base + waitNs;
    while (mach_absolute_time() < deadline) {
      if (yunluo_cryptex_dev[0] || yunluo_preboot_status == -4 ||
          yunluo_preboot_status >= 4) {
        break;
      }
      IOSleep(500);
    }
    // Even when cryptex attached early, do not touch the medium until init has
    // had a chance to get going.
    IOSleep(20000);
    kprintf("NativeMtkUfs: expdb log writer taking over (cryptex=%s preboot=%d)\n",
            yunluo_cryptex_dev[0] ? yunluo_cryptex_dev : "none",
            yunluo_preboot_status);
  }

  uint64_t rounds = 0;
  unsigned consecutiveErrors = 0;
  uint64_t written = 0;

  kprintf("NativeMtkUfs: expdb log writer started, window lba=%llu blocks=%llu\n",
          static_cast<unsigned long long>(kLogWindowFirstLba),
          static_cast<unsigned long long>(kLogWindowBlocks));

  for (;;) {
    IOSleep(2000);
    const uint64_t total = yunluo_log_position();
    if (total == written && rounds != 0) {
      continue;
    }
    bool ok = false;
    bool got = false;
    {
      __atomic_store_n(&gLogWritePending, 1, __ATOMIC_RELEASE);
      for (int attempt = 0; attempt < 100 && !got; attempt++) {   /* ~5 s ceiling */
        if (IOLockTryLock(gUtpGate)) {
          got = true;
          break;
        }
        IOSleep(50);
      }
      __atomic_store_n(&gLogWritePending, 0, __ATOMIC_RELEASE);
      if (got) {
        ok = drv->logWriteSnapshotLocked(&written);
        IOLockUnlock(gUtpGate);
      } else {
        snprintf(gLogNote, sizeof(gLogNote), "writer starved");
        kprintf("NativeMtkUfs: expdb writer starved (gate owner 0x%llx held %llu ms)\n",
                (unsigned long long)gUtpGateOwner,
                (unsigned long long)utpGateHeldMs());
        continue;
      }
    }
    if (!ok) {
      native_mtkufs_log_errors = native_mtkufs_log_errors + 1;
      consecutiveErrors++;
      // Three failures in a row is systemic, not a transient; stop rather than
      // filling the console with retry noise.  A single failure is retried on
      // the next round, because losing the rest of a boot over one bad command
      // is worse than the retry.
      if (consecutiveErrors >= 3) {
        kprintf("NativeMtkUfs: expdb log writer stopped after round %llu "
                "(writes=%u errors=%u)\n",
                static_cast<unsigned long long>(rounds),
                native_mtkufs_log_writes, native_mtkufs_log_errors);
        return;
      }
      continue;
    }
    consecutiveErrors = 0;
    ++rounds;
    native_mtkufs_log_writes = native_mtkufs_log_writes + 1;
    // A periodic one-liner so the screen shows the writer is alive; the
    // snapshot thread prints the long form.
    if (rounds <= 3 || (rounds % 30) == 0) {
      kprintf("NativeMtkUfs: expdb round=%llu total=%llu\n",
              static_cast<unsigned long long>(rounds),
              static_cast<unsigned long long>(written));
    }
  }
}

// APFS only probes a new media once its properties match, and the snapshot
// thread only retries every 5 s - too late for launchd's exec. This thread
// retries immediately so the cryptex volume is attached before init runs.
[[maybe_unused]] static void cryptexAttachThread(void *, wait_result_t)
{
  bool marked = false;
  for (int i = 0; i < 120 && !yunluo_cryptex_dev[0]; ++i) {
    if (gCryptexMedia) {
      if (!marked && gCryptexMedia->retryApfsMatch() > 0) {
        marked = true;
        kprintf("NativeMtkUfs: cryptex media marked for apfs, kicking matcher\n");
      } else if (marked) {
        gCryptexMedia->kickApfsMatch();
      }
    }
    IOSleep(250);
  }
}

extern "C" {
extern int nblkdev;
volatile uint32_t yunluo_cryptex_opens;
volatile uint32_t yunluo_cryptex_ioctls;
volatile uint32_t yunluo_cryptex_reads;
volatile uint32_t yunluo_cryptex_badcmd;
}

extern "C" bool yunluo_publish_cryptex(void)
{
  return gBlockMedia ? gBlockMedia->publishSecondaryWindow() : false;
}

// ---------------------------------------------------------------------------
// Console log window on expdb, consumed by kits/yunluo/BootLog.c
// ---------------------------------------------------------------------------
// BootLog.c already drives the sheng-port API (native_ufs_log_open/write/sync)
// over weak symbol imports, so exporting the same names here makes the existing
// writer work on yunluo without touching it.  The three functions are
// deliberately narrow: the only LBA range reachable through them is
// [kLogWindowFirstLba, kLogWindowLastLba], and a write is refused outright when
// the LU reported bLUWriteProtect.
//
// Returns 1 with the window's first absolute LBA and its size in 4096-byte
// blocks, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_log_open(uint64_t *firstLba, uint64_t *blocks, const char **why)
{
  const char *reason = "";
  int ok = 0;
  native_mtkufs_log_open_attempts = native_mtkufs_log_open_attempts + 1;
  NativeMtkUfs *drv = gBlockMedia;
  if (!drv) reason = "no root layout";
  else if (gLogOpen) reason = "already open";
  else if (!drv->logWindowReady()) reason = "device not ready";
  else if (drv->logWindowWriteProtected()) reason = "lu write protected";
  else {
    gLogFirstLba = kLogWindowFirstLba;
    gLogBlocks = kLogWindowBlocks;
    gLogOpen = true;
    if (firstLba) *firstLba = gLogFirstLba;
    if (blocks) *blocks = gLogBlocks;
    kprintf("NativeMtkUfs: log window open lba=%llu..%llu (%llu blocks, %llu MiB)\n",
            static_cast<unsigned long long>(kLogWindowFirstLba),
            static_cast<unsigned long long>(kLogWindowLastLba),
            static_cast<unsigned long long>(kLogWindowBlocks),
            static_cast<unsigned long long>((kLogWindowBlocks * 4096ULL) >> 20));
    ok = 1;
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogOpen || !gBlockMedia || !src || !blocks) return 0;
  if (rel >= kLogWindowBlocks || blocks > kLogWindowBlocks - rel) {
    native_mtkufs_log_errors = native_mtkufs_log_errors + 1;
    return 0;
  }
  while (blocks) {
    const uint32_t chunk = blocks > kDataBytes / 4096
        ? static_cast<uint32_t>(kDataBytes / 4096) : blocks;
    if (!gBlockMedia->logWriteBlocks(kLogWindowFirstLba + rel, chunk, src)) {
      native_mtkufs_log_errors = native_mtkufs_log_errors + 1;
      return 0;
    }
    src = static_cast<const uint8_t *>(src) + static_cast<uint64_t>(chunk) * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  native_mtkufs_log_writes = native_mtkufs_log_writes + 1;
  return 1;
}

// Durability hook for the sheng-style BootLog.c caller; the built-in writer
// calls syncDeviceCache() itself after each round.
extern "C" int native_ufs_log_sync(void)
{
  if (!gLogOpen || !gBlockMedia) return 0;
  return gBlockMedia->syncDeviceCache() ? 1 : 0;
}

extern "C" uint64_t native_ufs_export_blocks(void) { return 0; }

// NativeRamBlock's persistent-store provider hooks (weak imports there): the
// real Data partition window on UFS (fb143).  The consumer (NativeRamBlock's
// overlay) asks for a window of the given size; it is placed at the tail of
// the userdata partition, clear of the flashed cryptex image plus a 4 MiB
// guard on each side (the tail guard also keeps the LU2 backup GPT safe).
// Boot-arg sheng-data-persist: 0/absent = off (volatile RAM disk, the
// previous behavior), 1 = read-only probe, 2 = read-write window.
namespace {
constexpr uint64_t kUserdataFirstLba = 0x2BC000ULL;   // factory scatter 0x2bc000000
constexpr uint64_t kUserdataBlocks = 0x300000ULL;     // 12 GiB
constexpr uint64_t kUserdataImageBlocks = 806400ULL;  // the flashed cryptex sits at the start
constexpr uint64_t kStoreGuardBlocks = 1024ULL;       // 4 MiB
uint64_t gStoreFirstLba = 0, gStoreBlocks = 0;

unsigned dataStoreMode()
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn("sheng-data-persist", &value, sizeof(value)) || value > 2) {
      value = 0;
    }
    cached = static_cast<int>(value);
  }
  return static_cast<unsigned>(cached);
}
}  // namespace

extern "C" unsigned native_ufs_store_mode(void) { return dataStoreMode(); }

extern "C" int native_ufs_store_open(uint64_t blocks, uint64_t *firstLba, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!dataStoreMode()) {
    reason = "off";
  } else if (gStoreBlocks) {
    reason = "already open";
  } else if (!gBlockMedia) {
    reason = "media not ready";
  } else {
    const uint64_t head = kUserdataImageBlocks + kStoreGuardBlocks;
    const uint64_t tail = kStoreGuardBlocks;
    if (!blocks || head + tail >= kUserdataBlocks || blocks > kUserdataBlocks - head - tail) {
      reason = "size";
    } else {
      const uint64_t first = kUserdataFirstLba + kUserdataBlocks - tail - blocks;
      if (first < kUserdataFirstLba + head) {
        reason = "overlaps the cryptex image";
      } else {
        gStoreFirstLba = first;
        gStoreBlocks = blocks;
        if (firstLba) {
          *firstLba = first;
        }
        kprintf("NativeMtkUfs: data store window lba=%llu blocks=%llu mode=%u\n",
                static_cast<unsigned long long>(first),
                static_cast<unsigned long long>(blocks), dataStoreMode());
        ok = 1;
      }
    }
  }
  if (why) {
    *why = reason;
  }
  if (!ok) {
    kprintf("NativeMtkUfs: data store refused: %s\n", reason);
  }
  return ok;
}

extern "C" int native_ufs_store_read(uint64_t rel, uint32_t blocks, void *dst)
{
  if (!gStoreBlocks || !gBlockMedia || !dst || !blocks || rel >= gStoreBlocks ||
      blocks > gStoreBlocks - rel) {
    return 0;
  }
  uint8_t *out = static_cast<uint8_t *>(dst);
  constexpr uint32_t kChunkBlocks = kDataBytes / 4096;
  while (blocks) {
    const uint32_t take = blocks > kChunkBlocks ? kChunkBlocks : blocks;
    if (!gBlockMedia->storeReadBlocks(gStoreFirstLba + rel, take, out)) {
      return 0;
    }
    out += static_cast<uint64_t>(take) * 4096;
    rel += take;
    blocks -= take;
  }
  return 1;
}

extern "C" int native_ufs_store_write(uint64_t rel, uint32_t blocks, const void *src)
{
  if (dataStoreMode() != 2 || !gStoreBlocks || !gBlockMedia || !src || !blocks ||
      rel >= gStoreBlocks || blocks > gStoreBlocks - rel) {
    return 0;
  }
  const uint8_t *in = static_cast<const uint8_t *>(src);
  constexpr uint32_t kChunkBlocks = kDataBytes / 4096;
  while (blocks) {
    const uint32_t take = blocks > kChunkBlocks ? kChunkBlocks : blocks;
    if (!gBlockMedia->storeWriteBlocks(gStoreFirstLba + rel, take, in)) {
      return 0;
    }
    in += static_cast<uint64_t>(take) * 4096;
    rel += take;
    blocks -= take;
  }
  return 1;
}

extern "C" int native_ufs_store_sync(void)
{
  if (dataStoreMode() != 2 || !gStoreBlocks || !gBlockMedia) {
    return 0;
  }
  return gBlockMedia->syncDeviceCache() ? 1 : 0;
}

extern "C" unsigned native_ufs_store_gpt_check(void)
{
  // open() places the window inside userdata clear of both ends; the tail
  // guard keeps the LU2 backup GPT out of it.  Reported for diagnostics.
  return gStoreBlocks ? 1u : 0u;
}

[[maybe_unused]] static void cryptexNameThread(void *, wait_result_t)
{
  for (int i = 0; i < 160 && !yunluo_cryptex_dev[0] && yunluo_preboot_status < 100; ++i) {
    OSDictionary *m = IOService::serviceMatching("AppleAPFSVolume");
    if (m) {
      OSIterator *it = IOService::getMatchingServices(m);
      m->release();
      if (it) {
        while (OSObject *o = it->getNextObject()) {
          IOService *vol = OSDynamicCast(IOService, o);
          if (!vol) {
            continue;
          }
          OSString *u = OSDynamicCast(OSString, vol->getProperty("UUID"));
          OSString *b = OSDynamicCast(OSString, vol->getProperty("BSD Name"));
          if (u && b && strcmp(u->getCStringNoCopy(), kCryptexVolumeUUID) == 0) {
            const char *n = b->getCStringNoCopy();
            unsigned k = 0;
            for (; n && n[k] && k < sizeof(yunluo_cryptex_dev) - 1; ++k) {
              yunluo_cryptex_dev[k] = n[k];
            }
            yunluo_cryptex_dev[k] = 0;
            __atomic_store_n(&yunluo_cryptex_dev_ready, 1, __ATOMIC_RELEASE);
            kprintf("NativeMtkUfs: cryptex volume /dev/%s\n", yunluo_cryptex_dev);
            break;
          }
        }
        it->release();
      }
    }
    if (!yunluo_cryptex_dev[0]) {
      IOSleep(250);
    }
  }
  if (!yunluo_cryptex_dev[0]) {
    kprintf("NativeMtkUfs: cryptex volume never attached\n");
  }
}

namespace {

int gCryptexMajor = -1;
void *gCryptexDevNode = nullptr;
uint32_t gCryptexBlockSize = 4096;   /* negotiated through DKIOCSETBLOCKSIZE */
uint8_t gCryptexBounce[4096];
IOLock *gCryptexBounceLock = nullptr;

int cryptexOpen(dev_t, int, int, proc_t)
{
  __atomic_fetch_add(&yunluo_cryptex_opens, 1, __ATOMIC_RELAXED);
  return gCryptexMajor >= 0 ? 0 : ENXIO;
}
int cryptexClose(dev_t, int, int, proc_t) { return 0; }
int cryptexDump(void) { return ENXIO; }
/* d_psize is the device block size in bytes, not a sector count. */
int cryptexPsize(dev_t) { return 4096; }

// One request at a time; the UTP path is polled, so the copy happens inline.
// Cluster/UPL buffers have no b_data: take the kernel mapping with buf_map().
void cryptexStrategy(buf_t bp)
{
  const uint32_t seq = __atomic_fetch_add(&yunluo_cryptex_reads, 1, __ATOMIC_RELAXED) + 1;
  const uint32_t bsize = gCryptexBlockSize ? gCryptexBlockSize : 4096;
  const uint32_t want = buf_count(bp);
  buf_setresid(bp, want);

  if (!(buf_flags(bp) & B_READ)) {
    buf_seterror(bp, EROFS);
    buf_biodone(bp);
    return;
  }
  if (!gBlockMedia) {
    buf_seterror(bp, ENXIO);
    buf_biodone(bp);
    return;
  }

  const uint64_t total = kCryptexWindowBlocks * 4096ULL;
  uint64_t off = static_cast<uint64_t>(buf_blkno(bp)) * bsize;
  if (off >= total) {
    buf_seterror(bp, EINVAL);
    buf_biodone(bp);
    return;
  }

  caddr_t base = nullptr;
  if (buf_map(bp, &base) != 0) {
    buf_seterror(bp, EIO);
    buf_biodone(bp);
    return;
  }

  if (gCryptexBounceLock) {
    IOLockLock(gCryptexBounceLock);
  }
  uint8_t *dst = reinterpret_cast<uint8_t *>(base);
  uint32_t left = want;
  uint32_t copied = 0;
  int error = 0;
  while (left) {
    const uint64_t lba = kCryptexWindowBaseLba + off / 4096ULL;
    const uint32_t inblk = static_cast<uint32_t>(off % 4096ULL);
    uint32_t chunk = 4096 - inblk;
    if (chunk > left) {
      chunk = left;
    }
    if (off + chunk > total) {
      break;   /* short read at the end of the window; resid reports it */
    }
    if (!gBlockMedia->readWindowBlock(lba, gCryptexBounce)) {
      error = EIO;
      break;
    }
    if (seq <= 2) {
      kprintf("NativeMtkUfs: cryptex read#%u blkno=%llu bsize=%u off=%llu "
              "srcmagic=%c%c%c%c\n",
              seq, (unsigned long long)buf_blkno(bp), bsize,
              (unsigned long long)off,
              gCryptexBounce[0x20] ? gCryptexBounce[0x20] : '.',
              gCryptexBounce[0x21] ? gCryptexBounce[0x21] : '.',
              gCryptexBounce[0x22] ? gCryptexBounce[0x22] : '.',
              gCryptexBounce[0x23] ? gCryptexBounce[0x23] : '.');
    }
    memcpy(dst, gCryptexBounce + inblk, chunk);
    dst += chunk;
    off += chunk;
    left -= chunk;
    copied += chunk;
  }
  if (gCryptexBounceLock) {
    IOLockUnlock(gCryptexBounceLock);
  }
  buf_unmap(bp);
  if (error) {
    buf_seterror(bp, error);
  } else {
    buf_setresid(bp, want - copied);
  }
  buf_biodone(bp);
}

int cryptexIoctl(dev_t, u_long cmd, caddr_t data, int, proc_t)
{
  __atomic_fetch_add(&yunluo_cryptex_ioctls, 1, __ATOMIC_RELAXED);
  const uint64_t total = kCryptexWindowBlocks * 4096ULL;
  switch (cmd) {
  case DKIOCSETBLOCKSIZE: {
    /* spec_open() probes 512 first, apfs then sets 4096 (0x80046418). */
    const uint32_t size = *reinterpret_cast<uint32_t *>(data);
    if (size < 512 || size > 4096 || (size & (size - 1)) != 0 || (total % size) != 0) {
      return EINVAL;
    }
    gCryptexBlockSize = size;
    return 0;
  }
  case DKIOCGETBLOCKSIZE:
    *reinterpret_cast<uint32_t *>(data) = gCryptexBlockSize;
    return 0;
  case DKIOCGETPHYSICALBLOCKSIZE:
    *reinterpret_cast<uint32_t *>(data) = 4096;
    return 0;
  case DKIOCGETBLOCKCOUNT:
    *reinterpret_cast<uint64_t *>(data) = total / (gCryptexBlockSize ? gCryptexBlockSize : 4096);
    return 0;
  case DKIOCGETMAXBLOCKCOUNTREAD:
    *reinterpret_cast<uint64_t *>(data) = 256;
    return 0;
  case DKIOCGETMAXBLOCKCOUNTWRITE:
    *reinterpret_cast<uint64_t *>(data) = 0;
    return 0;
  case DKIOCGETMAXBYTECOUNTREAD:
    *reinterpret_cast<uint64_t *>(data) = 256ULL * 4096ULL;
    return 0;
  case DKIOCGETMAXBYTECOUNTWRITE:
    *reinterpret_cast<uint64_t *>(data) = 0;
    return 0;
  case DKIOCGETMAXSEGMENTCOUNTREAD:
    *reinterpret_cast<uint64_t *>(data) = 32;
    return 0;
  case DKIOCGETMAXSEGMENTCOUNTWRITE:
    *reinterpret_cast<uint64_t *>(data) = 0;
    return 0;
  case DKIOCGETMAXSEGMENTBYTECOUNTREAD:
    *reinterpret_cast<uint64_t *>(data) = 256ULL * 4096ULL;
    return 0;
  case DKIOCGETMAXSEGMENTBYTECOUNTWRITE:
    *reinterpret_cast<uint64_t *>(data) = 0;
    return 0;
  case DKIOCGETMINSEGMENTALIGNMENTBYTECOUNT:
    *reinterpret_cast<uint64_t *>(data) = 4096;
    return 0;
  case DKIOCGETMAXSEGMENTADDRESSABLEBITCOUNT:
    *reinterpret_cast<uint64_t *>(data) = 64;
    return 0;
  case DKIOCGETFEATURES:
    *reinterpret_cast<uint32_t *>(data) = 0;
    return 0;
  case DKIOCISWRITABLE:
  case DKIOCISFORMATTED:
    *reinterpret_cast<uint32_t *>(data) = 0;
    return 0;
  case DKIOCISVIRTUAL:
    *reinterpret_cast<uint32_t *>(data) = 0;
    return 0;
  case DKIOCISSOLIDSTATE:
    *reinterpret_cast<uint32_t *>(data) = 1;
    return 0;
  case DKIOCGETCOMMANDPOOLSIZE:
    *reinterpret_cast<uint32_t *>(data) = 32;
    return 0;
  case DKIOCGETBSDUNIT:
    *reinterpret_cast<uint32_t *>(data) = 0;
    return 0;
  case DKIOCGETLOCATION:
  case DKIOCGETBASE:
  case DKIOCGETTHROTTLEMASK:
    *reinterpret_cast<uint64_t *>(data) = 0;
    return 0;
  case DKIOCGETENCRYPTIONTYPE:
    *reinterpret_cast<uint32_t *>(data) = 0;
    return 0;
  default:
    yunluo_cryptex_badcmd = static_cast<uint32_t>(cmd);
    kprintf("NativeMtkUfs: cryptex unknown ioctl 0x%x\n", static_cast<unsigned>(cmd));
    return ENOTTY;
  }
}

struct bdevsw gCryptexBdevsw = {
  cryptexOpen, cryptexClose, cryptexStrategy, cryptexIoctl, cryptexDump, cryptexPsize, D_DISK,
};

bool publishCryptexBlockDev()
{
  if (gCryptexMajor >= 0) {
    return true;
  }
  if (!gCryptexBounceLock) {
    gCryptexBounceLock = IOLockAlloc();
  }
  /* Major 8 is the first slot outside Apple's reserved 0..7 range. */
  const int maj = bdevsw_add(8, &gCryptexBdevsw);
  if (maj < 0) {
    kprintf("NativeMtkUfs: bdevsw_add failed\n");
    return false;
  }
  gCryptexDevNode = devfs_make_node(makedev(maj, 0), DEVFS_BLOCK, 0, 0, 0600, "cryptex");
  if (!gCryptexDevNode) {
    kprintf("NativeMtkUfs: devfs_make_node failed\n");
    (void)bdevsw_remove(maj, &gCryptexBdevsw);
    return false;
  }
  gCryptexMajor = maj;
  kprintf("NativeMtkUfs: /dev/cryptex major=%d nblkdev=%d blocks=%llu\n", maj, nblkdev,
          static_cast<unsigned long long>(kCryptexWindowBlocks));
  return true;
}

}  // namespace

bool NativeMtkUfs::publishSecondaryWindow()
{
  if (secondaryWindow || gCryptexPublished || !utpReady || !deviceBlockSize ||
      !deviceCapacityBlocks) {
    return false;
  }
  if (kCryptexWindowBaseLba + kCryptexWindowBlocks > deviceCapacityBlocks) {
    kprintf("NativeMtkUfs: cryptex window outside the LUN cap=%llu\n",
            static_cast<unsigned long long>(deviceCapacityBlocks));
    return false;
  }
  // Two exposures of the same window:
  //  - /dev/cryptex (bdevsw): a plain container device, deterministic, used as
  //    a fallback and proven to read the container correctly;
  //  - an IOKit IOMedia: lets the apfs kext build the volume device (diskNs1),
  //    which is what mount_apfs itself mounts.
  if (!publishCryptexBlockDev()) {
    kprintf("NativeMtkUfs: bdev cryptex unavailable\n");
  }

  NativeMtkUfs *second = new NativeMtkUfs;
  if (!second) {
    return false;
  }
  if (!second->init()) {
    second->release();
    return false;
  }
  second->secondaryWindow = true;
  second->windowBaseLba = kCryptexWindowBaseLba;
  second->deviceBlockSize = deviceBlockSize;
  second->deviceCapacityBlocks = deviceCapacityBlocks;
  second->deviceBlocks = kCryptexWindowBlocks;
  second->mediaSectors = (kCryptexWindowBlocks * deviceBlockSize) / 512ULL;
  second->hwMajor = hwMajor;
  second->utpReady = true;
  second->online = true;
  // Own bounce buffer: the primary's dataBytesPtr is DMA'd into under
  // gUtpGate only, but this instance copies out after that lock is dropped.
  second->dataMemory = IOBufferMemoryDescriptor::inTaskWithOptions(
      kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut) |
          static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
      kDataBytes, 4096);
  if (second->dataMemory) {
    second->dataPhysical = second->dataMemory->getPhysicalAddress();
    second->dataBytesPtr = static_cast<uint8_t *>(second->dataMemory->getBytesNoCopy());
  }
  if (!second->dataBytesPtr) {
    kprintf("NativeMtkUfs: cryptex bounce buffer allocation failed\n");
    second->release();
    return false;
  }
  second->gate = IOLockAlloc();
  if (!second->gate) {
    second->release();
    return false;
  }
  IOService *parent = ufsProvider ? ufsProvider : this;
  if (!second->attach(parent)) {
    kprintf("NativeMtkUfs: cryptex media attach failed\n");
    second->release();
    return false;
  }
  if (!second->installBlockMediaDispatch()) {
    second->release();
    return false;
  }
  (void)second->start(parent);
  second->registerService();
  gCryptexMedia = second;
  gCryptexPublished = true;
  kprintf("NativeMtkUfs: cryptex media published lba=%llu blocks=%llu\n",
          static_cast<unsigned long long>(kCryptexWindowBaseLba),
          static_cast<unsigned long long>(kCryptexWindowBlocks));
  {
    thread_t t = nullptr;
    if (kernel_thread_start(cryptexAttachThread, second, &t) == KERN_SUCCESS) {
      thread_deallocate(t);
    }
    t = nullptr;
    if (kernel_thread_start(cryptexNameThread, second, &t) == KERN_SUCCESS) {
      thread_deallocate(t);
    }
  }
  return true;
}

bool NativeMtkUfs::start(IOService *provider)
{
  if (secondaryWindow) {
    return IOService::start(provider);
  }
  ufsProvider = provider;
  unsigned recoverReads = 0;
  PE_parse_boot_argn("yunluo-ufs-read-recover", &recoverReads, sizeof(recoverReads));
  readRecoveryEnabled = recoverReads == 1;
  if (!IOService::start(provider)) {
    return false;
  }
  if (native_mtkufs_stage < 1) {
    native_mtkufs_stage = 1;
  }
  gate = IOLockAlloc();
  if (!gate) {
    kprintf("NativeMtkUfs: IOLockAlloc failed\n");
    return false;
  }
  if (!gUtpGate) {
    gUtpGate = IOLockAlloc();
  }

  if (!mapRegisters()) {
    return false;
  }
  if (!enableClocks()) {
    return false;
  }
  if (!requestReferenceClock()) {
    return false;
  }
  // The vendor reads CCAP/REG_UFS_VERSION in ufshcd_init(), after the clock and
  // reference-clock blocks (ufshcd_hba_init) have completed.  Keep that order.
  readCapabilities();

  if (!resetDeviceAndHost()) {
    return false;
  }
  if (!enableController()) {
    return false;
  }
  if (!linkStartup()) {
    return false;
  }
  if (!readDescriptors()) {
    return false;
  }
  if (!proveLbaZero()) {
    return false;
  }
  (void)prepareApfsCompatibility();

  {
    // Report what the APFS window actually starts with. NXSB lives at 0x20 in
    // the container's block 0; anything else means the window base is wrong or
    // super still holds Android LP metadata instead of the APFS container.
    if (readDeviceBlocks(kSuperWindowBaseLba, 1, dataBytesPtr)) {
      const uint8_t *w = dataBytesPtr;
      const bool nxsb = w[0x20] == 'N' && w[0x21] == 'X' &&
                        w[0x22] == 'S' && w[0x23] == 'B';
      kprintf("NativeMtkUfs: window lba=%llu first=%02x %02x %02x %02x "
              "magic@0x20=%c%c%c%c nxsb=%u\n",
              static_cast<unsigned long long>(kSuperWindowBaseLba),
              static_cast<unsigned>(w[0]), static_cast<unsigned>(w[1]),
              static_cast<unsigned>(w[2]), static_cast<unsigned>(w[3]),
              w[0x20] ? w[0x20] : '.', w[0x21] ? w[0x21] : '.',
              w[0x22] ? w[0x22] : '.', w[0x23] ? w[0x23] : '.',
              nxsb ? 1U : 0U);
      kprintf("NativeMtkUfs: nx block_size=%u block_count=%llu features=0x%llx "
              "rocompat=0x%llx incompat=0x%llx\n",
              getLe32(w + 0x24),
              static_cast<unsigned long long>(getLe64(w + 0x28)),
              static_cast<unsigned long long>(getLe64(w + 0x30)),
              static_cast<unsigned long long>(getLe64(w + 0x38)),
              static_cast<unsigned long long>(getLe64(w + 0x40)));
    } else {
      kprintf("NativeMtkUfs: window block read failed\n");
    }
  }

  // Stage 10: only after stage 9 succeeded is the media published.
  if (!installBlockMediaDispatch()) {
    return false;
  }
  online = true;
  registerService();
  IOSleep(100);
  setApfsMediaProperties();
  kprintf("NativeMtkUfs: block services registered\n");
  native_mtkufs_stage = 10;
  {
    thread_t snapshotThread = nullptr;
    if (kernel_thread_start(mediaSnapshotThread, nullptr, &snapshotThread) == KERN_SUCCESS) {
      thread_deallocate(snapshotThread);
    }
  }
  {
    thread_t logThread = nullptr;
    if (kernel_thread_start(logWriterThread, this, &logThread) == KERN_SUCCESS) {
      thread_deallocate(logThread);
    }
  }
  kprintf("NativeMtkUfs: stage 10 media published: %llu 512-byte sectors, "
          "device block %u, read-only\n",
          static_cast<unsigned long long>(mediaSectors), deviceBlockSize);
  // fb141: identifies the probe build in the expdb log unconditionally (the
  // fb140 boot's probe stayed silent and the log could not prove which image ran).
  kprintf("NativeMtkUfs: read-integrity probe active (fb141)\n");
  // The cryptex window is published from kern_exec right before init exec, so
  // apfs does not probe it while the root is still coming up.
  return true;
}

void NativeMtkUfs::stop(IOService *provider)
{
  online = false;
  // TODO(spec §2.7): no link/clock/power-down sequence is performed on stop.
  // The boot chain and EL3 own the device power state; tearing the controller
  // down here could break a subsequent hand-off.  Release of the reference
  // clock (REFCLK_CTRL bit 0 -> 0 plus REF_CLK_NOTIFICATION(off)) is a later step.
  IOService::stop(provider);
}

void NativeMtkUfs::free()
{
  if (hostMemory) {
    hostMemory->release();
    hostMemory = nullptr;
  }
  if (dataMemory) {
    dataMemory->release();
    dataMemory = nullptr;
  }
  if (ufsMapping) {
    ufsMapping->release();
    ufsMapping = nullptr;
  }
  if (clockMapping) {
    clockMapping->release();
    clockMapping = nullptr;
  }
  if (gate) {
    IOLockFree(gate);
    gate = nullptr;
  }
  IOService::free();
}

// ---------------------------------------------------------------------------
// Block storage interface

// ---------------------------------------------------------------------------
IOReturn NativeMtkUfs::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
                                        UInt64 count, IOStorageAttributes *,
                                        IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || count == 0) {
    return kIOReturnBadArgument;
  }
  if (!online || !utpReady) {
    return kIOReturnNotReady;
  }
  if (!deviceBlockSize || block >= deviceBlocks || count > deviceBlocks - block ||
      count > 0xFFFFFFFFULL / deviceBlockSize ||
      buffer->getLength() < count * deviceBlockSize) {
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection() != kIODirectionIn) {
    // This driver never writes the medium.
    return kIOReturnNotWritable;
  }

  const uint64_t totalBytes = count * deviceBlockSize;
  const uint32_t maxBlocks = kDataBytes / deviceBlockSize;
  uint64_t done = 0;
  IOReturn result = kIOReturnSuccess;

  IOLockLock(gate);
  while (done < totalBytes) {
    const uint64_t remaining = totalBytes - done;
    const uint32_t blocks = (remaining / deviceBlockSize > maxBlocks)
                                ? maxBlocks
                                : static_cast<uint32_t>(remaining / deviceBlockSize);
    const uint32_t chunk = blocks * deviceBlockSize;
    // Plan A: the published media is one window of LU2, so every media block
    // maps to LU2 block windowBaseLba + block.
    const uint64_t lba = windowBaseLba + block + done / deviceBlockSize;
    /* fb147 ROOT FIX for the read-corruption storms (fb138-fb145): the old
     * flow released the UTP gate inside readDeviceBlocks() and only THEN
     * copied the shared DMA bounce out to the request.  A concurrent request
     * on the other published window could take the gate and start its own
     * DMA into the SAME bounce while this thread was still copying -- every
     * command reports success, the data is a mix of two requests, roughly
     * once per 500k requests under daemon-phase concurrency (single CPU:
     * preemption between the unlock and the copy-out is enough).  Read,
     * verify and copy-out now happen under ONE gate hold. */
    NativeMtkUfs *io = (secondaryWindow && gBlockMedia && gBlockMedia != this) ? gBlockMedia : this;
    const bool useGate = gUtpGate != nullptr;
    if (useGate) {
      utpGateLockTracked();
    }
    bool chunkOk = io->readDeviceBlocksLocked(lba, blocks, dataBytesPtr);
    if (chunkOk && readVerifyEnabled()) {
      if (io->readDeviceBlocksLocked(lba, blocks, gVerifyChunk)) {
        gVerifyChecked++;
        if (memcmp(dataBytesPtr, gVerifyChunk, chunk) != 0) {
          for (uint32_t b = 0; b < blocks; ++b) {
            uint8_t *first = dataBytesPtr + static_cast<uint64_t>(b) * 4096;
            const uint8_t *second = gVerifyChunk + static_cast<uint64_t>(b) * 4096;
            if (memcmp(first, second, 4096) == 0) {
              continue;
            }
            gVerifyMismatch++;
            /* Majority vote: the third read decides which copy to serve. */
            if (!io->readDeviceBlocksLocked(lba + b, 1, gVerifyExpect)) {
              kprintf("NativeMtkUfs: VERIFY MISMATCH lba=%llu block=%u (tiebreak read failed, keeping first)\n",
                      static_cast<unsigned long long>(lba + b), b);
              continue;
            }
            if (memcmp(gVerifyExpect, second, 4096) == 0) {
              memcpy(first, second, 4096);
              gVerifyFixed++;
              kprintf("NativeMtkUfs: VERIFY FIXUP lba=%llu block=%u (first copy was bad, served re-read)\n",
                      static_cast<unsigned long long>(lba + b), b);
            } else {
              kprintf("NativeMtkUfs: VERIFY MISMATCH lba=%llu block=%u (re-read was bad, kept first)\n",
                      static_cast<unsigned long long>(lba + b), b);
            }
          }
        } else if (gVerifyChecked % 4096 == 0) {
          kprintf("NativeMtkUfs: verify checked=%llu mismatch=%llu fixed=%llu\n",
                  static_cast<unsigned long long>(gVerifyChecked),
                  static_cast<unsigned long long>(gVerifyMismatch),
                  static_cast<unsigned long long>(gVerifyFixed));
        }
      } else {
        gVerifyChecked++;
      }
    } else if (!chunkOk) {
      /* fall through to the error path below */
    } else {
      gVerifyChecked++;
    }
    if (chunkOk && buffer->writeBytes(done, dataBytesPtr, chunk) != chunk) {
      chunkOk = false;
      result = kIOReturnIOError;
    }
    if (useGate) {
      utpGateUnlockTracked();
    }
    if (!chunkOk) {
      if (result == kIOReturnSuccess) {
        result = kIOReturnError;
      }
      break;
    }
    patchMediaSectors(lba, blocks, dataBytesPtr);
    done += chunk;
  }
  IOLockUnlock(gate);

  completion->action(completion->target, completion->parameter, result, done);
  const uint32_t request = native_mtkufs_block_requests;
  if (request <= 32) {
    kprintf("NativeMtkUfs: io done req=%u block=%llu count=%llu result=0x%x "
            "done=%llu len=%llu\n",
            request, static_cast<unsigned long long>(block),
            static_cast<unsigned long long>(count), result,
            static_cast<unsigned long long>(done),
            static_cast<unsigned long long>(buffer->getLength()));
  }
  return kIOReturnSuccess;
}

