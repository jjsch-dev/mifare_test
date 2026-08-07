# PROJECT NOTES — ES242F Smart Lock ESP32-S3 Retrofit

> **How to use this file:** living architecture document. Update it at the end
> of every development stage, commit it with the code, and hand it over at the
> start of every new working session/thread — it is the project's memory.
> Sections answer questions, in order: why it exists, what we have, what we
> decided (and why), where we are, what we don't know yet, how we work.

---

## 1. Vision — why this project exists

Standard commercial smart locks (ELOCK ES242F class) are built around the
**Tuya** cloud ecosystem. For our company that is a dead end:

* The lock cannot be integrated into our own infrastructure.
* We cannot issue/sell **our own Mifare cards** managed by our backend.
* There is no continuous WiFi connectivity under our control — Tuya owns the
  channel.

**Solution:** buy the commercial lock and **replace only the main CPU**. The
original SoC is a **Freqchip FR8018H** (BLE 5.1 only — no WiFi), which is the
real integration blocker. We desolder it and plug an **ESP32-S3 adapter board**
in its place, reusing all the other electronics (fingerprint module, touch
keypad, Mifare reader, audio, lock driver, power supply).

**End goal — own IoT infrastructure:**

* ESP32-S3 <-> **MQTT** (short messages) over our own network.
* A backend administers the fleet of locks.
* An **app** lets each user interact with the locks they own.
* We (the company) are the administrator of the whole network.

---

## 2. Hardware inventory — what we have

### 2.1 Main board (silkscreen "FINGER", red PCB)

| Ref / Marking | Function | Fate in the retrofit |
| --- | --- | --- |
| **FR8018H** (Freqchip) | Original main SoC, BLE 5.1, runs Tuya firmware | **Removed** — replaced by ESP32-S3 adapter board |
| **NZ3801-AB** | Mifare/13.56 MHz reader, **pin-compatible MFRC522 clone**, SPI. VersionReg reads `0xB2` (genuine MFRC522 = 0x91/0x92) | Reused — Stage 2 lib. **Confirmed clone:** identical register map, ISO14443-3 sequence, and uses MFAuthent (0x0E) command. |
| **GTX312L** | 12-channel capacitive touch controller, I2C | Reused — Stage 3 lib |
| **CS8302** | Class-D audio amplifier -> mini speaker | Reused — Stage 4 lib |
| **HSJ08H** | Lock solenoid/electromagnet driver (exact identity **unconfirmed**, see §6) | Reused — driven as a GPIO/PWM signal |
| **ZB25VQ64C** | 8 MB SPI NOR flash (firmware + probably voice prompts), **shares the SPI bus with the NZ3801** | Reused — **dump contents before removing the CPU** (see §6) |
| Fingerprint module connector | GROW **R558-S** (custom ELOCK firmware "FPM.TXRF"), UART, 57600 8N1 | Done — Stage 1 (`fpm` component) |

### 2.2 Front panel (silkscreen "TOUCH_BKN001T V2.1", blue PCB)

* **12 capacitive touch zones** K1–K12: digits 0–9 plus `*` and `#` — wired to
  the GTX312L's 12 channels.
* **13.56 MHz Mifare antenna** routed in the center area of the PCB.
* **Center hole** for the keypad **backlight LED**.
* **3 RGB LEDs** for user status indication.

### 2.3 Photos

* `docs/images/main_board.jpg` — component side (FR8018H, NZ3801, GTX312L,
  CS8302, HSJ08H, ZB25VQ64C identified).
* `docs/images/front_panel.jpg` — blue touch panel with K1–K12 zones, antenna
  and status LEDs.

*(TODO: copy the two board photos into `docs/images/` with these names.)*

---

## 3. Target architecture

```
                 +-------------------------------------------+
                 |              ESP32-S3 (adapter)           |
                 |                                           |
   R558-S  UART  |  +---------+ +--------+ +------+ +-----+  |
   (finger) <----|->|  fpm    | | mifare | |keypad| |audio|  |
                 |  | (done)  | |  lib   | | lib  | | lib |  |
   NZ3801   SPI  |  +---------+ +--------+ +------+ +-----+  |
   +flash   <----|->         lock_app (MQTT client)          |
   GTX312L  I2C  |    state machine: ACTIVE/ARMED/RECOVERY   |
   touch    <----|                                           |
   CS8302   GPIO |                                           |
   audio    <----|                                           |
   HSJ08H   GPIO |                                           |
   solenoid <----|                                           |
                 +--------------------+----------------------+
                                      | WiFi / MQTT
                                 our lock network
                              (backend + user app)
```

**Component strategy (validated in Stage 1):** every peripheral gets an
**isolated library** under `components/<name>/` (public API in `include/`,
implementation in `src/`) plus an **interactive console test harness** in
`main/` to validate it against the real hardware (scope/sniffer evidence)
before any integration. The final MQTT app only *composes* the libraries and
orchestrates the state machine — it contains no driver code.

---

## 4. Design decisions (and why)

| # | Decision | Rationale |
| --- | --- | --- |
| D1 | Isolated component libs + console test app per stage | Testable against hardware before integration; when the final app fails, the libs are already proven. Classic component strategy, matches the ESP-IDF model. |
| D2 | **OEM behavior parity by default**; deviate only with a documented reason | The OEM firmware is the reference spec. Deliberate deviations so far: 10 GetImage retries in verify (OEM: 3), search count 100 (OEM: 99, off-by-one). Stage 2 deviations: real sector auth + token instead of OEM's vestigial auth (D8, stronger security); optional polling decay (D10). |
| D3 | English for function names, comments and docs | Repo consistency; tooling- and collaboration-friendly. |
| D4 | Reverse engineering with **evidence**: UART/SPI sniffer captures + oscilloscope measurements before coding | Every protocol fact in this project is wire-proven. Custom PIO SPI sniffer: https://github.com/jjsch-dev/spi_sniffer_pio |
| D5 | ESP32-S3 replaces only the CPU; all other electronics reused | Minimizes risk and cost; the lock keeps its original mechanics, RF tuning and power design. |
| D6 | MQTT with short messages on our own network | Company-administered fleet; users interact through our app. |
| D7 | One development thread per stage; this file is the handoff document | Context windows are finite; the project's memory lives in the repo, not in a chat. |
| D8 | **Mifare security: sector authentication + structured token, NOT UID-only** | UID of Mifare Classic is trivially clonable (UID-changeable cards cost ~$0.50). **2026-08-06 update:** the OEM *is* UID-only in practice — its MFAuthent uses a fixed key, always fails (Crypto1On never set), the result is ignored, and enrollment writes nothing to the card (proven by 3-card matrix + enrollment capture). Our auth+token scheme is a documented security upgrade, not a parity break. |
| D9 | **Power management: two-mode RF policy matching OEM** | **2026-08-06 update (scope + logs):** OEM has two modes — (a) **zero cards enrolled:** RF only fires after GTX312L touch (event-driven); (b) **≥1 card enrolled:** continuous 1 Hz polling, no decay observed (30+ min scope watch). NRSTPD hard power-down between bursts keeps the reader at <10 µA while asleep. We replicate both modes for parity. |
| D10 | Optional deviation: inactivity decay from 1 Hz polling back to touch-gated mode | 1 Hz polling costs ~0.4–0.6 mA average in RF alone — significant on 4×AA. OEM never decays (observed 30+ min). If we add a decay timeout it must be a documented, opt-in deviation. **Status: open decision.** |

### 4.1 Card security strategy (2026-08-04, reinforced 2026-08-06)

We will issue **our own Mifare Classic 1K cards** with the following scheme:

| Layer | Mechanism | Details |
|-------|-----------|---------|
| **Transport** | Sector authentication (Key A) | Key derived per-lock or per-fleet from a master secret |
| **Data payload** | 16-byte structured token in Sector 1 (blocks 4–6) | Magic (4B) + Device ID (4B) + Monotonic Counter/Timestamp (4B) + CRC (4B) |
| **Anti-clone / anti-replay** | Monotonic counter on card + last-seen counter in lock NVS | Lock rejects cards with counter <= last known value |
| **Key derivation** | Key A = HMAC-SHA256(master_secret, lock_serial \| sector_num)[0:6] | 6 bytes fit Mifare Classic Key A/B format. Master secret stored in encrypted ESP32-S3 NVS. |
| **Fallback** | UID-only mode with `ALERT` logging | For visitor cards or legacy; never default. Logged as unauthenticated access attempt. **Note: UID-only = exact OEM security level.** |

**Why Sector 1 instead of Sector 0?** Block 0 of Sector 0 contains
manufacturer/UID data and is read-only on genuine NXP cards. Using Sector 1
avoids write complications. (The OEM's sector-0 auth attempt turned out to be
vestigial — see Stage 2 findings — so there is no OEM payload layout to stay
compatible with.)

---

## 5. Roadmap and stage status

### Stage 1 — Fingerprint library (`fpm`) — DONE

GROW R558-S, custom ELOCK firmware ("FPM.TXRF", Synochip `0xEF01` framing).

* Full driver + CLI: init, enroll (5-pass, anti-duplicate, DB-full red LED),
  touch-triggered verify (sleep -> IRQ -> GetImage/GenChar/Search -> LED ->
  re-sleep), search, delete, LED control, sleep/cancel, power cycle,
  `debug_capacity` test hook.
* 7-test acceptance suite passed on the wire. Timing: touch->result ~480 ms.
* Full documentation: `components/fpm/README.md` (protocol notes, corrected
  ReadSysPara layout, scope captures).

### Stage 2 — Mifare library (`mifare`, NZ3801-AB) — BENCH VALIDATED, pending lock-board port

**Confirmed facts (scope + sniffer + processed log evidence):**

* **NZ3801-AB = MFRC522 clone.** Three independent proofs:
  1. Register map matches MFRC522 exactly (all accessed registers decode correctly).
  2. ISO14443-3 sequence: WUPA -> Anticollision Cascade 1 -> Select Cascade 1 -> SAK.
  3. **MFAuthent command (0x0E)** observed in OEM logs.
* **VersionReg = `0xB2`** (one-time read at mode entry; genuine MFRC522 =
  0x91/0x92). Use as chip self-test in `mifare_init()`.
* **SPI bus shared with ZB25VQ64C flash.** When the CPU powers the reader down
  it drives CS and all SPI pins LOW (avoids parasitic feeding) — the flash is
  then accessed on the same bus. Sniffer is **gated by NRSTPD** so flash
  traffic is ignored; side effect: bytes on the NRSTPD rising edge can be
  corrupt (e.g. spurious writes to "Reg 0x0F", odd-length frames — capture
  artifacts, not OEM behavior).
* **SPI speed:** 6.06 MHz measured on scope (`cs_clk_6mhz.png`). Use 6 MHz.
* **OEM card security model = UID-only (behaviorally proven, 2026-08-06).**
  3-card matrix (`card_c5099803_tuya_registrada.txt`,
  `card_tuya_0483ac03_no_registrada.txt`,
  `card_8804670D_mifare_generica.txt`):
  * All three cards get a **byte-identical RF transaction** (only UID/ATQA/SAK
    differ). The OEM does not branch on SAK or card type.
  * **MFAuthent is vestigial:** always `Auth A (0x60), block 0x00, key
    04 01 07 00 05 09, UID`. Status2Reg read 12 ms later = `0x00` in ALL
    cases (Crypto1On never set -> auth always fails, even for the enrolled
    card). The result is ignored: same antenna-off path regardless.
  * **No block is ever read or written** (no `0x30`/`0xA0` anywhere).
    Closes the old "post-MFAuthent" open question.
  * The enrolled card **opened the lock** during capture with auth failed and
    zero data exchanged -> accept/reject is a UID lookup off the RF bus
    (MCU/Tuya side).
* **Enrollment writes NOTHING to the card** (`card_c5099803_enroll.txt`):
  enrolling = read UID + store it (lock flash / Tuya). The capture shows two
  ordinary read transactions ~1 s apart, identical to open attempts. The
  immediate "door open" audio right after the app confirms enrollment is just
  the continuous 1 Hz polling re-reading the still-present, now-registered
  card — **not** an enrollment confirmation exchange.
* **7-byte UID cards:** the generic card answered ATQA `44 00`, SAK `0x04`
  (cascade bit set; UID0 = `0x88` = Cascade Tag). The OEM **ignores cascade
  level 2** and treats `88 04 67 0D` as the 4-byte UID. Parity note for UID
  reporting (our lib may implement proper CL2 — documented deviation if so).
* **Power management — two modes (scope `polling_continuo_enroll_card.png` +
  logs):**
  * **Zero cards enrolled:** antenna only fires after GTX312L touch
    (event-driven), ~5–6 bursts then back to full idle.
  * **≥1 card enrolled:** continuous **1 Hz polling** (log timestamps exactly
    1.000 s apart; scope shows it sustained 50 s, user observed 30+ min — it
    does NOT decay).
  * NRSTPD hard power-down between bursts; SPI pins driven LOW while down.
* **Idle poll burst sequence (1 Hz, lightweight — no SoftReset, no full init):**
  ```
  RD CommandReg (expect 0x20)
  WR ControlReg=0x10, ModeReg=0x3F, Reg0x23=0x00, Reg0x25=0x80,
     AutoTestReg=0x40, TxASKReg=0x40, CommandReg=Idle(0x00),
     TxControlReg=0x03 (antenna ON, no InvTx2), BitFramingReg=0x07,
     ComIrqReg=0x7F, DivIrqReg=0x7F (clear), FIFOLevelReg=0x80 (flush)
  WR CommandReg=Transceive(0x0C) -> WR FIFO=0x52 (WUPA) -> WR BitFramingReg=0x87 (StartSend)
  Poll Status2Reg ModemState: 0x03 -> 0x04 -> 0x05 = no card -> NRSTPD LOW, sleep 1 s
                              0x06 = card responding -> card-detected branch
  ```
* **Card-detected branch (full transaction, classic ordering):**
  ```
  SoftReset (CommandReg=0x0F)
  Full init: ModWidthReg=0x26, RD TxASKReg(0x00), WR TxASKReg=0x40,
    RD RxThresholdReg(0x84), RD RFCfgReg(0x48), WR GsNReg=0xF8,
    WR CWGsPReg=0x3F, WR ControlReg=0x10, WR RFCfgReg=0x40,
    WR RxThresholdReg=0x54, RD TxControlReg(0x80), WR TxControlReg=0x83,
    WR TxModeReg=0x00, WR RxModeReg=0x00, Idle, flush FIFO
  WUPA (0x52) -> ATQA
  Anticollision CL1 (93 20) -> UID0..3 + BCC (verify BCC = XOR)
  Select CL1 (93 70 + UID + BCC) -> SAK
  WR Status2Reg=0x00, TxModeReg=0x80, RxModeReg=0x80 (CRC on)
  MFAuthent: 60 00 | 04 01 07 00 05 09 | UID (result ignored by OEM)
  RD TxControlReg (0x83) -> WR TxControlReg=0x80 (antenna drivers OFF)
  -> resume 1 Hz idle polling
  ```
* **Observed recovery behavior:** SoftReset is issued at the start of every
  card-detected branch (and was seen mid-sequence after failed polls in an
  earlier log). Implement as branch entry + fallback after N failed cycles.
* Minor observation: enrollment capture shows ~300–500 ms gaps mid-burst —
  most likely BLE/app activity stalling the FR8018H during enrollment (or PC
  timestamp batching). Not protocol-relevant.

**Proposed library API (`components/mifare/include/mifare.h`):**

```c
typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t cs_pin;
    gpio_num_t rst_pin;        /* NRSTPD */
    gpio_num_t irq_pin;        /* Optional: NZ3801 IRQ for command-done */
    int spi_clock_hz;          /* 6_000_000 for OEM parity */
} mifare_config_t;

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;           /* 4, 7 or 10 bytes */
    uint8_t sak;
    uint16_t atqa;
} mifare_card_t;

/* Lifecycle */
esp_err_t mifare_init(const mifare_config_t *cfg);   /* self-test: VersionReg == 0xB2 */
esp_err_t mifare_power_up(void);     /* NRSTPD HIGH + init sequence */
esp_err_t mifare_power_down(void);   /* NRSTPD LOW + drive SPI pins LOW */

/* ISO14443-3 */
esp_err_t mifare_reqa(uint16_t *atqa);
esp_err_t mifare_wupa(uint16_t *atqa);       /* OEM uses WUPA, not REQA */
esp_err_t mifare_anticoll_select(mifare_card_t *card);

/* Mifare Classic — for OUR card scheme (sector 1 token), not OEM parity */
esp_err_t mifare_auth(uint8_t block, mifare_key_type_t key_type,
                      const uint8_t *key, const uint8_t *uid);
esp_err_t mifare_read_block(uint8_t block, uint8_t data[16]);
esp_err_t mifare_write_block(uint8_t block, const uint8_t data[16]);
esp_err_t mifare_halt(void);
```

**Bench validation (2026-08-06, RC522-clone module on ESP32-S3 DevKitC-1,
`mifare_test` harness, sniffer-verified):**

* Bench module reads **VersionReg = 0xB2** — same NZ3801-class silicon as the
  lock, so the bench is highly representative.
* **Full stack validated with 3 real cards** (console + sniffer evidence):
  WUPA/ATQA, anticollision CL1+CL2, SELECT/SAK, Crypto1 auth (Key A), block
  read, block write+verify. UIDs: Tuya `C5099803`, Tuya `0483AC03` (both SAK
  0x08), SUBE `04670DFAA95D80` (7-byte, cascade — matches the predicted OEM
  truncation `88 04 67 0D` = "8804670D").
* **Clone silicon quirks discovered (each one cost a capture; all worked
  around in `mifare.c`):**
  1. **ComIrqReg IRQs unreliable**: RxIRq asserts but the Transceive command
     never terminates; the TAuto hardware timer never fires (TimerIRq stayed
     clear for a 50 ms window). The OEM sidesteps all of it — waits =
     FIFOLevelReg polling after WUPA/anticoll/select, CommandReg-until-Idle
     after MFAuthent. We now do exactly the same (software deadline + Idle
     abort on timeout).
  2. **Self-reception (loopback):** on long CRC-enabled frames the receiver
     comes up during our own TX and the FIFO fills with our own tail bytes
     BEFORE the card could answer (SELECT: 6 bytes ~150 us into a ~770 us
     frame; HLTA: 1 phantom byte). Fix: one single FIFO flush timed at
     TX-end + 100 us (loopback settled, card's first byte still ~180 us
     away). Never flush mid-reception — it stalls the clone's receiver.
  3. **ErrorReg kept diagnostic-only** (collision stays fatal) — OEM never
     reads it.
* **`poll` validated (2026-08-06):** full 1 Hz OEM-cycle replica, 5/5 cycles
  detected the Tuya card and executed the complete card branch byte-faithful
  to OEM — idle burst WUPA → SoftReset + full init → WUPA/ATQA `04 00` →
  anticoll CL1 (UID `C5099803`, BCC OK) → SELECT/SAK `0x08` → vestigial
  MFAuthent probe (fails as expected, key `04 01 07 00 05 09` does not open
  a factory card) → CommandReg-until-Idle wait. Evidence:
  `mifare_poll_works.txt` (3359 SPI frames, all 5 cycles identical).
* **Antenna experiment resolved:** OEM burst config (TxControlReg=0x03) does
  not excite the bench module's antenna (A/B: `mifare_poll_fail.txt` vs
  `mifare_poll_works.txt`); InvTx2RFOn (0x83) does. This is a bench-module
  antenna artifact, NOT a silicon difference. Resolution: `OEM_TX_CONTROL_BURST`
  defaults to **0x03 (OEM parity)**; bench builds define
  `-DMIFARE_BENCH_MODULE=1` to get 0x83 (see `mifare_regs.h`).
* Harness extras (NOT OEM behavior, bench tooling in `main/`): state-tolerant
  `select` (HLTA + WUPA + anticollision) because the bench RF field is not
  duty-cycled; `auth`/`read`/`write` commands for our sector-1 token scheme.

**Stage inputs still needed:**
1. Verify if **GTX312L IRQ pin** is routed to a GPIO pad (event-driven wake
   without I2C polling). Critical for touch-gated mode + deep-sleep.
2. Verify if **NZ3801 IRQ pin** is routed to a GPIO pad (optional — OEM polls
   Status2Reg ModemState instead; IRQ would let us sleep during WUPA).
3. Measure **current draw** of NZ3801 in power-down vs. antenna-active states
   (quantify savings; also quantify the real cost of permanent 1 Hz polling).

**Remaining for Stage 2 completion:** port to the lock board (same driver,
`-V 0xB2` self-test), scope A/B of `mifare_idle_burst()` vs OEM captures, then
close out.

### Stage 3 — Keypad library (`keypad`, GTX312L)

* I2C, 12 channels (K1–K12 = 0–9, `*`, `#`).
* Observed OEM behavior: on finger contact the CPU reads which key is pressed,
  then polls until release, then **reconfigures the channels**. Captures exist;
  new measurements planned to confirm.
* **Open:** check whether the GTX312L **interrupt output** is routed on the PCB
  (would let us replace I2C polling with an IRQ, like we did with TOUCH_OUT).
  **This is now critical for Stage 2 power management:** the touch IRQ is the
  trigger that wakes the RFID reader in touch-gated mode.

### Stage 4 — Audio library (`audio`, CS8302)

* Class-D amp driving a mini speaker: haptic key beeps + voice guidance
  messages.
* Voice prompts are most likely stored in the ZB25VQ64C flash — see §6 dump task.

### Stage 5 — Lock application (`lock_app`, MQTT)

* Integrates all libs; state machine **ACTIVE / ARMED / RECOVERY**.
* Lesson from Stage 1: **stop verify before any power cycle** (the R558-S
  emits a ~15 ms TOUCH_OUT boot pulse ~100 ms after VCC-on that would trigger
  a spurious verification attempt).
* New lesson from Stage 2: **stop any MFAuthent/Transceive before power-down**
  to avoid leaving the card in an intermediate state.
* Stage 2 power policy to implement in `lock_app`/`power_manager` (D9/D10):
  no cards -> touch-gated RF; cards enrolled -> 1 Hz polling (optional decay).

---

## 6. Open questions / risks

1. **HSJ08H identity.** Closest datasheet found is the AOZ1331ADI, but that is
   a **dual load switch, not an H-bridge**. An H-bridge reverses coil polarity
   (active lock AND unlock); many solenoid locks use a single pulse + spring
   return instead. Method: probe the control lines and the coil with the scope
   during OEM lock/unlock — check line count and whether polarity reverses.
   Also check for a **PWM hold-current** scheme (reduce power after the pull-in
   pulse). Considered low-risk: whatever the signal is, we can replicate it.
2. **Dump the ZB25VQ64C flash before desoldering the CPU.** It holds the Tuya
   firmware and very likely the voice prompts. Recovering the audio keeps the
   OEM voice UX for free; the firmware dump is also a fallback reference.
3. **Power budget.** Battery-powered lock: quantify OEM sleep currents (CPU
   duty-cycling NRSTPD, fingerprint sleep <12 µA) so our ESP32-S3 design
   matches or beats them. ESP32-S3 deep-sleep + wake strategy TBD.
   **Priority:** measure NZ3801 current in NRSTPD LOW vs. antenna active, and
   the average cost of the permanent 1 Hz polling mode (matters for D10).
4. **Adapter board pinout.** Map every FR8018H pad used (UART finger, SPI
   flash+reader, I2C keypad, audio, solenoid, backlight LED, 3x RGB LEDs,
   NRSTPD, power) to ESP32-S3 GPIOs. Input for the adapter PCB design.
   **New requirement:** map NZ3801 IRQ and GTX312L IRQ if they exist.
5. **Backlight LED + 3 RGB LEDs drive** (front panel): driver circuit not yet
   traced; measure when the adapter pinout is defined.
6. **NZ3801 clone-specific registers.** The "reserved" registers `0x23` and
   `0x25` are written by the OEM with values `0x00` and `0x80`. If our clone
   reader shows lower sensitivity than the OEM, these are the first candidates
   to investigate. Document any deviation.
7. ~~**Post-MFAuthent OEM behavior.**~~ **RESOLVED 2026-08-06:** nothing
   follows MFAuthent — no block read/write; auth always fails and its result
   is ignored; antenna off; resume polling. The OEM is UID-only in practice.

---

## 7. Working conventions

* **Design before code:** significant changes are discussed and agreed first.
* **Evidence-driven:** protocol claims require sniffer/scope proof; capture
  files are named consistently (`sniff_<dut>_<scenario>.txt`) and archived.
* **Synced copies:** the user's repo is the working copy; the assistant keeps a
  reference copy. Manual edits are reported so both stay in sync.
* **Testing:** every stage ends with an explicit acceptance test list executed
  on real hardware; results captured in logs.
* **Commits:** English messages, `type: summary` style with bullet body.
* **Docs:** each lib gets a README (commands, protocol notes, scope captures);
  cross-stage knowledge lives in this file.

---

## 8. Glossary

* **UID** — Mifare card Unique Identifier (4/7 bytes). What the OEM reads — and
  the OEM's *only* credential (proven 2026-08-06).
* **CT (Cascade Tag)** — `0x88` as UID0 indicates a 7/10-byte UID; CL2/CL3
  anticollision is required to read the full UID. The OEM ignores it.
* **NRSTPD** — NZ3801/MFRC522 power-down/reset pin; LOW = hard power down.
* **Duty-cycling** — periodic wake/measure/sleep to save battery; the reader
  is blind while asleep. The OEM uses touch-gated duty-cycling when no cards
  are enrolled, and continuous 1 Hz cycling once at least one card exists.
* **ModemState** — Status2Reg bits [2:0], the NZ3801's Rx/Tx state machine.
  The OEM polls it after StartSend instead of using ComIrqReg: `03 -> 04 ->
  05` = WUPA sent, no response; `06` = card responding (triggers the
  card-detected branch).
* **StartSend** — BitFramingReg bit 7; starts the actual RF transmission after
  the Transceive command is armed. The OEM's idle burst pushes the WUPA byte
  into the FIFO *after* issuing Transceive and triggers the send via StartSend.
* **TOUCH_OUT** — R558-S fingerprint wake/touch signal (rising edge IRQ).
* **Load switch** — high-side power gate (e.g. TPS2530 for the finger module).
* **MFAuthent** — Mifare Classic authentication command (0x0E in MFRC522).
  In the OEM firmware it is vestigial: fixed key, always fails, result ignored.
* **WUPA** — Wake-Up Protocol Type A (0x52); wakes cards from HALT state.
* **REQA** — Request Type A (0x26); does not wake cards from HALT.
* **ATQA** — Answer To Request Type A (2 bytes); card type indicator.
* **SAK** — Select Acknowledge (1 byte); indicates UID size and card type.
* **BCC** — Block Check Character; XOR of the 4 UID bytes in anticollision.
* **Crypto1** — Mifare Classic proprietary stream cipher; enabled after
  successful MFAuthent (never enabled in any OEM capture — auth always fails).
