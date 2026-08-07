/**
 * @file mifare_regs.h
 * @brief MFRC522 register map, commands, bit fields and OEM-proven values.
 *
 * The NZ3801-AB is a pin- and register-compatible MFRC522 clone (proven by
 * sniffer: identical map, ISO14443-3 flow and MFAuthent usage). All register
 * naming follows the MFRC522 datasheet.
 *
 * "OEM_*" values were captured from the ES242F factory firmware with a PIO
 * SPI sniffer and are the parity reference (see PROJECT_NOTES.md Stage 2).
 */
#pragma once

/* ---------------------------------------------------------------------------
 * Register map (6-bit address, page 0..3)
 * ------------------------------------------------------------------------- */
/* Page 0: Command and Status */
#define MFRC522_REG_COMMAND         0x01
#define MFRC522_REG_COM_IEN         0x02
#define MFRC522_REG_DIV_IEN         0x03
#define MFRC522_REG_COM_IRQ         0x04
#define MFRC522_REG_DIV_IRQ         0x05
#define MFRC522_REG_ERROR           0x06
#define MFRC522_REG_STATUS1         0x07
#define MFRC522_REG_STATUS2         0x08
#define MFRC522_REG_FIFO_DATA       0x09
#define MFRC522_REG_FIFO_LEVEL      0x0A
#define MFRC522_REG_WATER_LEVEL     0x0B
#define MFRC522_REG_CONTROL         0x0C
#define MFRC522_REG_BIT_FRAMING     0x0D
#define MFRC522_REG_COLL            0x0E
/* Page 1: Command Configuration */
#define MFRC522_REG_MODE            0x11
#define MFRC522_REG_TX_MODE         0x12
#define MFRC522_REG_RX_MODE         0x13
#define MFRC522_REG_TX_CONTROL      0x14
#define MFRC522_REG_TX_ASK          0x15
#define MFRC522_REG_TX_SEL          0x16
#define MFRC522_REG_RX_SEL          0x17
#define MFRC522_REG_RX_THRESHOLD    0x18
#define MFRC522_REG_DEMOD           0x19
#define MFRC522_REG_MF_TX           0x1C
#define MFRC522_REG_MF_RX           0x1D
#define MFRC522_REG_SERIAL_SPEED    0x1F
/* Page 2: Configuration */
#define MFRC522_REG_CRC_RESULT_H    0x21
#define MFRC522_REG_CRC_RESULT_L    0x22
#define MFRC522_REG_RESERVED_23     0x23  /* Clone factory trim (OEM writes 0x00) */
#define MFRC522_REG_MOD_WIDTH       0x24
#define MFRC522_REG_RESERVED_25     0x25  /* Clone factory trim (OEM writes 0x80) */
#define MFRC522_REG_RF_CFG          0x26
#define MFRC522_REG_GS_N            0x27
#define MFRC522_REG_CW_GS_P         0x28
#define MFRC522_REG_MOD_GS_P        0x29
#define MFRC522_REG_T_MODE          0x2A
#define MFRC522_REG_T_PRESCALER     0x2B
#define MFRC522_REG_T_RELOAD_H      0x2C
#define MFRC522_REG_T_RELOAD_L      0x2D
/* Page 3: Test */
#define MFRC522_REG_AUTO_TEST       0x36
#define MFRC522_REG_VERSION         0x37

/* ---------------------------------------------------------------------------
 * CommandReg commands
 * ------------------------------------------------------------------------- */
#define MFRC522_CMD_IDLE            0x00
#define MFRC522_CMD_CALC_CRC        0x03
#define MFRC522_CMD_TRANSMIT        0x04
#define MFRC522_CMD_RECEIVE         0x09
#define MFRC522_CMD_TRANSCEIVE      0x0C
#define MFRC522_CMD_MFAUTHENT       0x0E
#define MFRC522_CMD_SOFT_RESET      0x0F

/* ---------------------------------------------------------------------------
 * Bit fields
 * ------------------------------------------------------------------------- */
/* ComIrqReg */
#define MFRC522_IRQ_TIMER           0x01
#define MFRC522_IRQ_ERR             0x02
#define MFRC522_IRQ_RX              0x20
#define MFRC522_IRQ_IDLE            0x10
#define MFRC522_IRQ_CLEAR_ALL       0x7F

/* ErrorReg */
#define MFRC522_ERR_PROTOCOL        0x01
#define MFRC522_ERR_PARITY          0x02
#define MFRC522_ERR_CRC             0x04
#define MFRC522_ERR_COLL            0x08
#define MFRC522_ERR_BUFFER_OVFL     0x10

/* Status2Reg */
#define MFRC522_STATUS2_CRYPTO1_ON  0x08
#define MFRC522_STATUS2_MODEM_MASK  0x07
/* ModemState values (datasheet + OEM-observed) */
#define MFRC522_MODEM_IDLE          0x00
#define MFRC522_MODEM_RX_RECEIVING  0x06  /* OEM uses this as "card responding" */

/* FIFOLevelReg */
#define MFRC522_FIFO_FLUSH          0x80

/* BitFramingReg */
#define MFRC522_BITFRAMING_START_SEND 0x80

/* TxControlReg */
#define MFRC522_TXCTL_TX1_EN        0x01
#define MFRC522_TXCTL_TX2_EN        0x02
#define MFRC522_TXCTL_INV_TX2_ON    0x80

/* ControlReg */
#define MFRC522_CONTROL_INITIATOR   0x10

/* ---------------------------------------------------------------------------
 * ISO14443-A / Mifare Classic command bytes
 * ------------------------------------------------------------------------- */
#define MIFARE_CMD_REQA             0x26
#define MIFARE_CMD_WUPA             0x52
#define MIFARE_CMD_ANTICOLL_CL1     0x93
#define MIFARE_CMD_ANTICOLL_CL2     0x95
#define MIFARE_CMD_ANTICOLL_CL3     0x97
#define MIFARE_CMD_SELECT           0x70
#define MIFARE_CMD_HLTA             0x50
#define MIFARE_CMD_AUTH_A           0x60
#define MIFARE_CMD_AUTH_B           0x61
#define MIFARE_CMD_READ             0x30
#define MIFARE_CMD_WRITE            0xA0
#define MIFARE_CASCADE_TAG          0x88
#define MIFARE_ACK                  0x0A  /* 4-bit ACK from card */

/* ---------------------------------------------------------------------------
 * OEM-proven configuration values (sniffer evidence)
 * ------------------------------------------------------------------------- */
#define OEM_MOD_WIDTH               0x26
#define OEM_TX_ASK                  0x40
#define OEM_GS_N                    0xF8
#define OEM_CW_GS_P                 0x3F
#define OEM_CONTROL                 0x10  /* Initiator mode */
#define OEM_RF_CFG                  0x40  /* 48 dB RX gain */
#define OEM_RX_THRESHOLD            0x54
#define OEM_TX_CONTROL_FULL         0x83  /* TX1+TX2+InvTx2 (transaction branch) */
/* Idle-burst antenna config. OEM parity value is 0x03 (TX1+TX2, no InvTx2)
 * and is the default for the lock-board port.
 *
 * BENCH EXCEPTION (proven 2026-08-06, A/B: mifare_poll_fail.txt vs
 * mifare_poll_works.txt): the RC522-clone Arduino bench module's antenna
 * stage does NOT excite cards with 0x03 during the burst; it needs
 * InvTx2RFOn (0x83) to get ATQA. This is a bench-module antenna artifact,
 * not a silicon difference — the lock board's NZ3801 antenna network is the
 * one the OEM 0x03 value was captured from. For bench bring-up build with:
 *   idf.py -DCMAKE_C_FLAGS=-DMIFARE_BENCH_MODULE=1 build
 */
#ifdef MIFARE_BENCH_MODULE
#define OEM_TX_CONTROL_BURST        0x83  /* Bench module: needs InvTx2 */
#else
#define OEM_TX_CONTROL_BURST        0x03  /* TX1+TX2, no InvTx2 (idle burst) */
#endif
#define OEM_MODE                    0x3F  /* = MFRC522 reset default; CRC preset FFFFh */
#define OEM_RESERVED_23             0x00  /* Clone factory trim — replicate */
#define OEM_RESERVED_25             0x80  /* Clone factory trim — replicate */
#define OEM_AUTO_TEST               0x40  /* Written in idle bursts */

/** OEM vestigial fixed auth key (always fails, result ignored by OEM). */
#define OEM_PROBE_KEY { 0x04, 0x01, 0x07, 0x00, 0x05, 0x09 }
