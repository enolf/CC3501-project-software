#pragma once

#include <stdint.h>
#include <stdbool.h>

// Driver for the PiicoDev RFID Module (NFC 13.56 MHz), which is an NXP MFRC522
// reader IC wired into I2C mode (schematic: I2C pin and EA pin both tied to 3V3).
//
// Chip facts live here (registers, commands, bit values). Board wiring — the
// I2C instance, pins, baud rate and the module's I2C address — lives in board.h.

// --- Register map (datasheet section 9.3) ---
// In I2C mode the register address byte is used as-is — no shifting.
// (SPI mode shifts the address left by one; that trap does not apply here.)
#define MFRC522_REG_COMMAND     0x01    // starts and stops command execution
#define MFRC522_REG_COM_IRQ     0x04    // interrupt request flags (we poll these)
#define MFRC522_REG_DIV_IRQ     0x05    // more IRQ flags; bit 2 = CRCIRq (CRC done)
#define MFRC522_REG_ERROR       0x06    // error flags from the last command
#define MFRC522_REG_FIFO_DATA   0x09    // read/write window into the 64-byte FIFO
#define MFRC522_REG_FIFO_LEVEL  0x0A    // bytes currently in the FIFO; bit 7 flushes it
#define MFRC522_REG_BIT_FRAMING 0x0D    // bit 7 = StartSend, bits [2:0] = TxLastBits
#define MFRC522_REG_MODE        0x11    // general modes; CRC coprocessor preset value
#define MFRC522_REG_TX_CONTROL  0x14    // antenna driver enables (Tx1RFEn/Tx2RFEn)
#define MFRC522_REG_TX_ASK      0x15    // bit 6 = Force100ASK modulation
#define MFRC522_REG_CRC_RESULT_H 0x21   // CRC coprocessor result, high byte
#define MFRC522_REG_CRC_RESULT_L 0x22   // CRC coprocessor result, low byte
#define MFRC522_REG_RF_CFG      0x26    // receiver config; bits [6:4] = RxGain
#define MFRC522_REG_T_MODE      0x2A    // timer mode; bit 7 = TAuto
#define MFRC522_REG_T_PRESCALER 0x2B    // timer prescaler low byte
#define MFRC522_REG_T_RELOAD_H  0x2C    // timer reload value, high byte
#define MFRC522_REG_T_RELOAD_L  0x2D    // timer reload value, low byte
#define MFRC522_REG_VERSION     0x37    // chip type + version (our identity check)

// --- Commands written to CommandReg (datasheet section 10.3) ---
#define MFRC522_CMD_IDLE        0x00    // cancels current command
#define MFRC522_CMD_CALC_CRC    0x03    // run the CRC coprocessor over the FIFO
#define MFRC522_CMD_TRANSCEIVE  0x0C    // transmit FIFO, then auto-enable receiver
#define MFRC522_CMD_SOFT_RESET  0x0F    // reset all registers to defaults

// --- ISO 14443A card commands (sent over the air, not to the reader) ---
#define PICC_CMD_REQA           0x26    // "any card there?" — 7-bit short frame
#define PICC_CMD_SEL_CL1        0x93    // anticollision / select, cascade level 1
#define PICC_CMD_SEL_CL2        0x95    // anticollision / select, cascade level 2

// --- ISO 14443A frame constants ---
#define PICC_CASCADE_TAG        0x88    // 1st anticollision byte when the UID continues
#define PICC_NVB_ANTICOLL       0x20    // NVB "2 valid bytes" — ask the card for its UID
#define PICC_NVB_SELECT         0x70    // NVB "7 valid bytes" — full UID for this level

// --- Register bit flags & masks used by the driver ---
#define MFRC522_COMIRQ_CLEAR_ALL 0x7F   // ComIrqReg: clears all seven IRQ flags at once
#define MFRC522_IRQ_RX          0x20    // ComIrqReg RxIRq   — a reply was received
#define MFRC522_IRQ_IDLE        0x10    // ComIrqReg IdleIRq — the command finished
#define MFRC522_IRQ_TIMER       0x01    // ComIrqReg TimerIRq — auto-timer expired (no card)
#define MFRC522_DIVIRQ_CRC      0x04    // DivIrqReg CRCIRq  — CRC calculation finished
#define MFRC522_FIFO_FLUSH      0x80    // FIFOLevelReg bit 7 flushes the FIFO
#define MFRC522_BITFRAMING_START 0x80   // BitFramingReg StartSend — launch the frame
#define MFRC522_ERR_INVALID_RX  0x13    // ErrorReg: BufferOvfl|CollErr|ParityErr|ProtocolErr

// --- Config values written during init (see mfrc522_init for the reasoning) ---
#define MFRC522_TMODE_TAUTO_PSHI 0x8D   // TModeReg: TAuto=1 + prescaler high nibble
#define MFRC522_TPRESCALER_LO   0x3E    // TPrescalerReg low byte  (together ~25 ms timeout)
#define MFRC522_TRELOAD_HI      0x00    // timer reload, high byte
#define MFRC522_TRELOAD_LO      30      // timer reload, low byte
#define MFRC522_TXASK_FORCE100  0x40    // TxASKReg Force100ASK — required for ISO 14443A
#define MFRC522_MODE_CRC6363    0x3D    // ModeReg: defaults + CRCPreset 0x6363
#define MFRC522_TXCONTROL_ANT_ON 0x03   // Tx1RFEn | Tx2RFEn — switch the antenna on

// --- Receiver gain (RFCfgReg bits [6:4]). Higher gain = longer read range but
// more noise-sensitive. Change MFRC522_RXGAIN_* in mfrc522_init to tune range. ---
#define MFRC522_RXGAIN_18DB     (0x00 << 4)
#define MFRC522_RXGAIN_23DB     (0x01 << 4)
#define MFRC522_RXGAIN_33DB     (0x04 << 4)   // chip's power-on default
#define MFRC522_RXGAIN_38DB     (0x05 << 4)
#define MFRC522_RXGAIN_43DB     (0x06 << 4)
#define MFRC522_RXGAIN_48DB     (0x07 << 4)   // maximum

// Largest UID we report. Single-size UIDs are 4 bytes (e.g. Mifare Classic);
// double-size are 7 bytes (NTAG, Mifare Ultralight, DESFire). We handle both.
#define MFRC522_UID_MAX_LEN     7

// --- Public API ---

// Set up the I2C bus, soft-reset the chip, verify identity, configure the
// timer / modulation, and switch the antenna on. Returns false if the chip
// does not respond or identifies as the wrong type.
bool mfrc522_init(void);

// Send a REQA and see if any card answers. Non-blocking beyond the chip's
// ~25 ms internal timeout — safe to call from a run_* task each loop.
bool mfrc522_card_present(void);

// Run anticollision to fetch the card's UID. Handles both single-size
// (4-byte) and double-size (7-byte) UIDs, walking cascade levels as needed.
// Only valid right after mfrc522_card_present() returned true (cards answer
// REQA only once, then wait for the anticollision sequence).
//   uid     must point to at least MFRC522_UID_MAX_LEN bytes.
//   uid_len receives the actual UID length (4 or 7).
bool mfrc522_read_card_uid(uint8_t *uid, uint8_t *uid_len);

// Raw register access, exposed for debugging from main if needed.
// read returns the register value (0 on a bus failure, which is also logged);
// write returns false if the I2C transaction did not complete.
uint8_t mfrc522_read_register(uint8_t reg);
bool mfrc522_write_register(uint8_t reg, uint8_t value);
