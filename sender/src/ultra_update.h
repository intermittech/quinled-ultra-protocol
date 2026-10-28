/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 * QuinLED Ultra update contract v2, protocol 0.3.2-draft.
 * Normative transaction, pacing and erase rules: ../docs/update.md.
 * Fixed wire objects use byte arrays: no packing, alignment or host-endian ABI.
 * Transport/image integers are big-endian. CRC32 feeds little-endian words.
 */
#ifndef ULTRA_UPDATE_H
#define ULTRA_UPDATE_H
#include "ultra_protocol.h"
#ifdef __cplusplus
extern "C" {
#endif

#define UPD_HDR_VERSION 3u /* frozen with update ABI 3; independent of ULTRA_VERSION */
#define UPD_HDR_FLAG 0x08u
#define UPD_HDR_BYTES 8u
#define UPD_BLOCK_BYTES 40u
#define UPD_FRAME_BYTES 48u
#define UPD_FRAME_BITS 384u
#define UPD_CHUNK_BYTES 32u
#define UPD_PROGRAM_BYTES 256u
#define UPD_CHUNKS_PER_PROGRAM 8u
#define UPD_INDEX_REACH 0x200000u
#define UPD_B_TYPE 0u
#define UPD_B_SESSION 1u
#define UPD_B_INDEX 2u /* u16; DATA offset = INDEX * 32, widened before multiply */
#define UPD_B_PASS 4u /* high nibble pass, low nibble passes; informational */
#define UPD_B_FLAGS 5u
#define UPD_B_DATA 6u
#define UPD_B_CRC 38u
enum {
    UPD_ANNOUNCE=0x81, /* INDEX 0/1: two halves of the 64-byte descriptor */
    UPD_DATA=0x82,
    UPD_END=0x83,      /* DATA: body CRC32, body length, then zero[24] */
    UPD_ABORT=0x84,
    UPD_PING=0x85,     /* idle app confirmation; selected-session training/keepalive or COMMITTED reboot ACK */
    UPD_STATUS=0x86,   /* RW+UPD only; DATA: UID[12], index=0,R=1,rate,zero[17] */
    UPD_SELECT=0x87    /* status profile: UID[12], immutable target[16], zero[4] */
};
/* Status profile: retain COMMITTED and answer status until PING for the same
 * selected session acknowledges it, then reboot within declared entry_10ms.
 * The host sends at most three ACK copies and waits the full entry bound after
 * each. An application ignores every nonzero-session PING; only an idle,
 * session-zero PING can authorize its installed-image confirmation. */
#define UPD_F_FORCE 0x01u /* ANNOUNCE only: permit same/older firmware; no check bypass */

/* Immutable target tuple, 16 bytes. MCU ids 2/3 are reserved; only V203/V407
 * profiles are defined. Board ids 1..254 are shared across all board kinds. */
#define UPD_TARGET_BYTES 16u
typedef struct { uint8_t bytes[UPD_TARGET_BYTES]; } upd_target_t;
#define UPD_T_BOARD 0u
#define UPD_T_MCU 1u
#define UPD_T_PACKAGE 2u
#define UPD_T_LAYOUT 3u
#define UPD_T_ABI 4u
#define UPD_T_FEATURES 5u /* u8; bits 4..7 reserved */
#define UPD_T_VARIANT 6u /* exact assembly variant, never a feature subset */
#define UPD_T_REVISION 7u
/* PCB vMrR = (M << 4) | R; v0r1=0x01, v0r4=0x04, v1r1=0x11. */
#define UPD_REV_V1R1 0x11u
#define UPD_REV_ENCODE(major, minor) (((major)<<4)|(minor))
#define UPD_T_SLOT_BASE 8u /* u32 physical flash address */
#define UPD_T_SLOT_SIZE 12u /* u32, body only, excludes metadata */
#define UPD_MCU_V203 1u
#define UPD_MCU_V407 4u
#define UPD_PACKAGE_F8U6 1u
#define UPD_PACKAGE_RET6 5u
#define UPD_LAYOUT_V203 1u
#define UPD_LAYOUT_V407 4u
#define UPD_BOOT_ABI 3u
#define UPD_FEATURE_STATUS 0x0001u
#define UPD_FEATURE_ERASE_4K 0x0002u
#define UPD_FEATURE_ERASE_32K 0x0004u
#define UPD_FEATURE_RESUME 0x0008u
#define UPD_FEATURE_MASK 0x000Fu

/* Protected bootloader info v2 at physical 0x08000010 (32 bytes):
 * magic QDB2[4], bl_version, target[16], geometry[3], entry_10ms, build_id[7].
 * Jig provisions it from the board firmware definition; apps copy it, never invent identity.
 * Geometry: program_log2<<4 | erase_log2, program_max_ms, erase_max_ms.
 * entry_10ms is ceil(entry_max_ms/10), 5..255; host waits this many 10 ms units.
 * Both operation bounds are nonzero u8 engineering bounds for the minimum
 * erase unit. Larger optional erase operations are not used by this profile.
 */
#define UPD_BL_INFO_ADDR 0x08000010u
#define UPD_BL_INFO_BYTES 32u
typedef struct {
    uint8_t magic[4], bl_version;
    upd_target_t target;
    uint8_t geometry[3], entry_10ms, build_id[7];
} upd_bl_info_t;
#define UPD_GEOMETRY_BYTES 3u
#define UPD_G_UNITS 0u
#define UPD_G_PROGRAM_MS 1u
#define UPD_G_ERASE_MS 2u
#define UPD_V203_GEOMETRY 0x88u /* 256-byte program and erase */
#define UPD_V407_GEOMETRY 0x8Cu /* 256-byte program, 4096-byte erase */
#define UPD_V203_PROGRAM_MS 10u
#define UPD_V203_ERASE_MS 20u
#define UPD_V407_PROGRAM_MS 30u /* example engineering bounds; qualification required */
#define UPD_V407_ERASE_MS 100u
/* Recorded design limits: change only by an explicit decision.
 * DEADLINE_FACTOR: a flash operation's deadline is twice its advertised maximum
 * (timing.md, flash and busy intervals). DECLARED_MARGIN: the 100 ms on top of
 * the erase lead (update.md, blind group). DISPATCH: the 100 ms verification /
 * dispatch allowance after END and after an idle PING, on top of twice
 * program_max (update.md, blind group; application confirmation page).
 * IDLE_TIMEOUT: the ten-second inactivity timeout (update.md, blind group).
 * ENTRY_MIN: the immutable entry bound is at least 50 ms, entry_10ms >= 5
 * (timing.md, update forwarding and entry). */
#define UPD_OPERATION_DEADLINE_FACTOR 2u
#define UPD_DECLARED_MARGIN_MS 100u
#define UPD_DISPATCH_MS 100u
#define UPD_IDLE_TIMEOUT_MS 10000u /* excludes declared/acknowledged busy intervals */
#define UPD_ENTRY_MIN_MS 50u
/* Update forwarding (timing.md, update forwarding and entry). Recorded design
 * limits: change only by an explicit decision.
 * TRAIN_MIN: >= 1000 ms of valid nonzero-session UPD PINGs before the entry
 * ANNOUNCE pair and again before the boot pair. IDLE_CLOSE: a transparent
 * forwarder closes 2000 ms after the last valid UPD header. FORWARD_QUIET: every
 * forward gate opens only after >= 350 us of LOW with no edge (above the 280 us
 * reset recognition and the 250 us late release, below the 500 us port gap). */
#define UPD_TRAIN_MIN_MS 1000u
#define UPD_IDLE_CLOSE_MS 2000u
#define UPD_FORWARD_QUIET_US 350u

/* Image descriptor v2: 64 bytes; metadata always has a 256-byte logical area
 * (64 descriptor + 192 status records), even in a 4 KiB erase sector.
 * CRC32 at 60 covers bytes 0..59 as 15 little-endian words. */
#define UPD_DESC_BYTES 64u
#define UPD_DESC_VERSION 2u
typedef struct { uint8_t bytes[UPD_DESC_BYTES]; } upd_descriptor_t;
#define UPD_D_MAGIC 0u /* QDU6 */
#define UPD_D_VERSION 4u
#define UPD_D_FLAGS 5u /* zero */
#define UPD_D_REV_MIN 6u
#define UPD_D_REV_MAX 7u
#define UPD_D_TARGET 8u /* tuple bytes 0..6: board,mcu,package,layout,ABI,required features:u8,variant:u8 */
#define UPD_D_FEATURES 13u
#define UPD_D_VARIANT 14u
#define UPD_D_MIN_BL 15u /* within the exact ABI */
#define UPD_D_FW_VERSION 16u
#define UPD_D_BODY_LEN 20u
#define UPD_D_BODY_CRC 24u
#define UPD_D_LOAD 28u /* physical address, equals immutable slot_base */
#define UPD_D_ENTRY 32u /* physical, halfword-aligned address inside body; no alias */
#define UPD_D_RECORD_FORMAT 36u
#define UPD_D_WIRE 37u
#define UPD_D_DESCRIPTOR_FORMAT 38u
#define UPD_D_RESERVED 39u /* zero */
#define UPD_D_BUILD_ID 40u /* 16 opaque bytes, e.g. truncated commit hash */
#define UPD_D_RESERVED_TAIL 56u /* zero[4] */
#define UPD_D_CRC 60u
#define UPD_PRE_ALPHA 0x00u /* +0..63 */
#define UPD_PRE_BETA 0x40u
#define UPD_PRE_RC 0x80u
#define UPD_PRE_STABLE 0xFFu
/* Firmware word = major<<24 | minor<<16 | patch<<8 | pre. */

#define UPD_METADATA_BYTES 256u
#define UPD_ST_FIRST 32u /* halfword index */
#define UPD_ST_COUNT 96u
#define UPD_ST_VERIFIED 0xA5B7u
#define UPD_ST_CONFIRMED 0xA5C3u
#define UPD_ST_TRIAL_QUIET 0xA5A0u
#define UPD_ST_TRIAL_SEEN 0xA5A1u
/* Layout constants are namespaced examples, not a universal flash map. */
#define UPD_V203_BL_BYTES 8192u
#define UPD_V203_METADATA 0x08002000u
#define UPD_V203_BODY 0x08002100u
#define UPD_V203_END 0x08010000u
#define UPD_V407_BL_BYTES 16384u
#define UPD_V407_METADATA 0x08004000u
#define UPD_V407_BODY 0x08005000u
#define UPD_V407_END 0x08080000u

/* UPD_STATUS uses the standard 32-byte answer, PAGE=0x86, SEQ=request session,
 * index 0, UID16=CRC16(UID). DATA offsets below; all u32 counts are byte counts.
 * BUSY: zero credit, retry_after_ms until next poll. Credit counts 32-byte chunks
 * (0..8) in the current 256-byte block; mask bits 0..7, high byte zero.
 * accepted = contiguous bytes in RAM or flash; committed = verified flash prefix.
 */
#define UPD_STATUS_VERSION 1u
#define UPD_S_VERSION 0u
#define UPD_S_STATE 1u
#define UPD_S_ERROR 2u
#define UPD_S_CREDIT 3u
#define UPD_S_SESSION 4u
#define UPD_S_FLAGS 5u
#define UPD_S_CHUNK_MASK 6u
#define UPD_S_ACCEPTED 8u
#define UPD_S_COMMITTED 12u
#define UPD_S_RETRY_MS 16u
#define UPD_S_IMAGE_CRC 20u
enum { UPD_STATE_IDLE=0, UPD_STATE_PREPARED=1, UPD_STATE_BUSY=2,
       UPD_STATE_READY=3, UPD_STATE_RECEIVE=4, UPD_STATE_VERIFY=5,
       UPD_STATE_COMMITTED=6, UPD_STATE_FAILED=7, UPD_STATE_RESTART_REQUIRED=8 };
enum { UPD_ERROR_NONE=0, UPD_ERROR_DESCRIPTOR=1, UPD_ERROR_TARGET=2,
       UPD_ERROR_BOUNDS=3, UPD_ERROR_SUPPLY=4, UPD_ERROR_TIMEOUT=5,
       UPD_ERROR_FLASH=6, UPD_ERROR_CONFLICT=7, UPD_ERROR_CRC=8,
       UPD_ERROR_SESSION=9, UPD_ERROR_UNSUPPORTED=10 };
#define UPD_SF_RESUMABLE 0x01u
#define UPD_SF_REVOKED 0x02u

/* Helpers are reference codecs/checks, not a bootloader implementation. */
static inline uint32_t upd_crc32_words(uint32_t crc, const uint8_t *p, uint32_t nwords)
{
    while(nwords--) {
        uint32_t b, w=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
        p+=4; crc^=w;
        for(b=0;b<32u;++b) crc=(crc&0x80000000u)?(crc<<1)^0x04C11DB7u:crc<<1;
    }
    return crc;
}
static inline int upd_erased16(uint32_t value) { return value==0xFFFFu || value==0xE339u; }

/* Zero = compatible. This pure check MUST precede revoke/erase/program.
 * Caller separately verifies actual option bytes, supply, firmware-owned target,
 * complete erase coverage and all rounded erase/program bounds (docs/update.md).
 * Unknown update descriptor formats/features fail closed. App record/wire/board
 * descriptor versions at 36..38 are informational to this immutable bootloader. A FORCE flag never bypasses this check.
 */
static inline int upd_desc_check(const uint8_t *d, const upd_target_t *target, uint8_t bl_version)
{
    const uint8_t *t=target->bytes;
    uint32_t i, len=ultra_get32(d+UPD_D_BODY_LEN), base=ultra_get32(t+UPD_T_SLOT_BASE);
    uint32_t size=ultra_get32(t+UPD_T_SLOT_SIZE), entry=ultra_get32(d+UPD_D_ENTRY);
    if(d[0]!='Q'||d[1]!='D'||d[2]!='U'||d[3]!='6'||
       upd_crc32_words(0xFFFFFFFFu,d,15)!=ultra_get32(d+UPD_D_CRC)) return UPD_ERROR_DESCRIPTOR;
    if(d[UPD_D_VERSION]!=UPD_DESC_VERSION||d[UPD_D_FLAGS]||d[39]||d[56]||d[57]||d[58]||d[59]) return UPD_ERROR_UNSUPPORTED;
    if(t[0]==0u||t[0]==255u||t[4]!=UPD_BOOT_ABI) return UPD_ERROR_TARGET;
    for(i=0;i<5u;++i) if(d[8u+i]!=t[i]) return UPD_ERROR_TARGET;
    if(d[UPD_D_VARIANT]!=t[UPD_T_VARIANT]||
       (d[UPD_D_FEATURES]&~t[UPD_T_FEATURES])!=0u||
       (d[UPD_D_FEATURES]&~UPD_FEATURE_MASK)!=0u||d[6]>d[7]||
       t[7]<d[6]||t[7]>d[7]||d[15]>bl_version) return UPD_ERROR_TARGET;
    if(!len||(len&3u)||len>size||len>UPD_INDEX_REACH||size>0xFFFFFFFFu-base||
       (base&255u)||ultra_get32(d+28u)!=base||entry<base||entry-base>=len||(entry&1u))
        return UPD_ERROR_BOUNDS;
    return UPD_ERROR_NONE;
}
/* Transit forwarding hold (timing.md, update forwarding and entry), from the
 * physical end of a valid ANNOUNCE. The descriptor declares no erase bound, so
 * count the image's erase units exactly from INDEX 0 (layout, load, length) and
 * the layout table above, and assume the largest u8 erase_max_ms:
 * units*2*255+100 ms, never shorter than any declared lead. Any INDEX 0
 * invalidity that upd_desc_check() rejects for every target holds the largest
 * valid value, the full layout-1 slot (224 units): magic, version, flags,
 * rev_min > rev_max, board 0/255, ABI, features outside the mask, layout, load
 * and length. Not forwarder checks: the target match (tuple, variant, feature
 * subset, revision in range, minimum bootloader) needs the receiving board,
 * and entry, reserved bytes and the descriptor CRC lie in bytes 32..63.
 * Recorded design limits: 255 ms (the largest u8 erase bound), + the 100 ms
 * margin, 114340 ms for the full layout-1 slot. Change only by an explicit decision. */
#define UPD_TRANSIT_ERASE_MS 255u
#define UPD_TRANSIT_HOLD_MAX_MS 114340u
static inline uint32_t upd_transit_hold_ms(const uint8_t *half0)
{
    uint32_t len, metadata, body, end, unit;
    if(!half0||half0[0]!='Q'||half0[1]!='D'||half0[2]!='U'||half0[3]!='6'||
       half0[UPD_D_VERSION]!=UPD_DESC_VERSION||half0[UPD_D_FLAGS]||
       half0[UPD_D_REV_MIN]>half0[UPD_D_REV_MAX]||
       half0[UPD_D_TARGET+UPD_T_BOARD]==0u||half0[UPD_D_TARGET+UPD_T_BOARD]==255u||
       half0[UPD_D_TARGET+UPD_T_ABI]!=UPD_BOOT_ABI||
       (half0[UPD_D_FEATURES]&~UPD_FEATURE_MASK)!=0u) return UPD_TRANSIT_HOLD_MAX_MS;
    if(half0[UPD_D_TARGET+UPD_T_LAYOUT]==UPD_LAYOUT_V203){
        metadata=UPD_V203_METADATA;body=UPD_V203_BODY;end=UPD_V203_END;unit=256u;
    } else if(half0[UPD_D_TARGET+UPD_T_LAYOUT]==UPD_LAYOUT_V407){
        metadata=UPD_V407_METADATA;body=UPD_V407_BODY;end=UPD_V407_END;unit=4096u;
    } else return UPD_TRANSIT_HOLD_MAX_MS;
    len=ultra_get32(half0+UPD_D_BODY_LEN);
    if(ultra_get32(half0+UPD_D_LOAD)!=body||!len||(len&3u)||len>end-body) return UPD_TRANSIT_HOLD_MAX_MS;
    return (body+len-metadata+unit-1u)/unit*(UPD_OPERATION_DEADLINE_FACTOR*UPD_TRANSIT_ERASE_MS)+UPD_DECLARED_MARGIN_MS;
}
typedef char upd_transit_hold_check[(UPD_V203_END-UPD_V203_METADATA)/256u*(UPD_OPERATION_DEADLINE_FACTOR*UPD_TRANSIT_ERASE_MS)+
    UPD_DECLARED_MARGIN_MS==UPD_TRANSIT_HOLD_MAX_MS &&
    (UPD_V407_END-UPD_V407_METADATA)/4096u*(UPD_OPERATION_DEADLINE_FACTOR*UPD_TRANSIT_ERASE_MS)+UPD_DECLARED_MARGIN_MS<=
    UPD_TRANSIT_HOLD_MAX_MS?1:-1];
static inline void upd_build_frame(uint8_t *f,uint8_t type,uint8_t session,uint16_t index,
                                  uint8_t pass,uint8_t flags,const uint8_t *data,uint32_t time_us)
{
    uint32_t i; f[0]=type;f[1]=session;ultra_put16(f+2,index);f[4]=pass;f[5]=flags;
    for(i=0;i<32u;++i) f[6u+i]=data?data[i]:0u;
    ultra_put16(f+38,ultra_crc16(f,38));
    ultra_put16(f+40,ULTRA_MAGIC);
    f[42]=(uint8_t)((UPD_HDR_VERSION<<4)|UPD_HDR_FLAG|(type==UPD_STATUS?ULTRA_HDR_RW:0u));
    ultra_put32(f+43,time_us);f[47]=ultra_crc8(f+40,7);
}
static inline int upd_check_frame(const uint8_t *f)
{
    uint8_t expected=(uint8_t)(UPD_HDR_FLAG|(f[0]==UPD_STATUS?ULTRA_HDR_RW:0u));
    return ultra_get16(f+40)==ULTRA_MAGIC && f[42]==(uint8_t)((UPD_HDR_VERSION<<4)|expected) &&
           f[47]==ultra_crc8(f+40,7) &&
           f[0]>=UPD_ANNOUNCE && f[0]<=UPD_SELECT && ultra_get16(f+38)==ultra_crc16(f,38) ? 0:-1;
}
typedef char upd_target_size_check[sizeof(upd_target_t)==16?1:-1];
typedef char upd_info_size_check[sizeof(upd_bl_info_t)==32?1:-1];
typedef char upd_descriptor_size_check[sizeof(upd_descriptor_t)==64?1:-1];
#ifdef __cplusplus
}
#endif
#endif
