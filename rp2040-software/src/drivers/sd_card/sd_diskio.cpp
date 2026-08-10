// The glue between FatFs and this project's SD card driver.
//
// FatFs is written to call five functions it does not provide: disk_status,
// disk_initialize, disk_read, disk_write and disk_ioctl. This file implements
// them in terms of sd_card.h. It is OUR code, which is why it lives in src/
// rather than lib/fatfs/ — everything in lib/fatfs/ is untouched third-party
// source and should stay that way, so it can be replaced wholesale on an
// upgrade.
//
// Only one drive exists, so the drive number is checked and otherwise ignored.

#include "ff.h"
#include "diskio.h"

#include "drivers/sd_card/sd_card.h"

namespace {

/// The only physical drive. FatFs calls it drive 0.
constexpr BYTE DRIVE_SD = 0;

constexpr UINT SECTOR_SIZE = 512;

} // namespace

extern "C" {

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != DRIVE_SD) {
        return STA_NOINIT;
    }
    return sd_card_is_ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != DRIVE_SD) {
        return STA_NOINIT;
    }

    // A probe that has already succeeded is not repeated: re-running the card's
    // initialisation handshake costs up to a second and gains nothing.
    if (sd_card_is_ready()) {
        return 0;
    }

    SdCardInfo info;
    return sd_card_probe(info) ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != DRIVE_SD || !sd_card_is_ready()) {
        return RES_NOTRDY;
    }

    // Multi-sector requests are served one sector at a time. The card supports
    // a multi-block read command that would be faster, but this driver logs a
    // few hundred bytes at a time and the simpler path is easier to be sure of.
    for (UINT i = 0; i < count; i++) {
        if (!sd_card_read_block(static_cast<uint32_t>(sector + i),
                                buff + (i * SECTOR_SIZE))) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != DRIVE_SD || !sd_card_is_ready()) {
        return RES_NOTRDY;
    }

    for (UINT i = 0; i < count; i++) {
        if (!sd_card_write_block(static_cast<uint32_t>(sector + i),
                                 buff + (i * SECTOR_SIZE))) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != DRIVE_SD) {
        return RES_NOTRDY;
    }

    switch (cmd) {
        case CTRL_SYNC:
            // sd_card_write_block does not return until the card has finished
            // programming, so by the time FatFs asks, nothing is outstanding.
            return RES_OK;

        case GET_SECTOR_COUNT:
            if (!sd_card_is_ready()) {
                return RES_NOTRDY;
            }
            *static_cast<LBA_t *>(buff) =
                static_cast<LBA_t>(sd_card_block_count());
            return RES_OK;

        case GET_SECTOR_SIZE:
            *static_cast<WORD *>(buff) = SECTOR_SIZE;
            return RES_OK;

        case GET_BLOCK_SIZE:
            // Erase block size in sectors. Only used by f_mkfs, which is
            // disabled, so the neutral answer of 1 is fine.
            *static_cast<DWORD *>(buff) = 1;
            return RES_OK;

        default:
            return RES_PARERR;
    }
}

} // extern "C"
