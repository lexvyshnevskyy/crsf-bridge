# CRSF ↔ CH9121 baud bridge (STM32F103C6T6)

Transparent bidirectional UART bridge so Crossfire / ExpressLRS **CRSF at 400000** can ride Ethernet via **CH9121**. The CH9121 UART runs at **921600**, the fastest rate in its Default clock list (230400 / 460800 / 921600).

```
CRSF RX ──400k──► STM32#1 ──921600──► CH9121#1 ──Ethernet──► CH9121#2 ──921600──► STM32#2 ──400k──► TX12/TX16
         ◄───────           ◄────────                       ◄────────           ◄───────
```

Same firmware on both STM32 boards.

## Architecture with SineLink

```
UAV FC <-416700-> SineLink slave ~~~ radio ~~~ SineLink master <-400000-> STM32 <-921600-> CH9121 <-Ethernet-> CH9121 <-921600-> STM32 <-400000-> TX12
```

The slave on the UAV talks to the flight controller directly and is not bridged. The STM32 on the ground connects to the **master's** UART1, standing in for the radio handset.

### SineLink master UART1 settings (LinkApp)

| Setting | Value |
|---------|-------|
| Port mapping | S1 (same label as the slave's CRSF port) |
| Protocol | CRSF |
| Speed | **400000** |
| Polarity | **Not Inverted** |
| Duplex mode | **Full Duplex** |
| Rx/Tx swap | off |
| RTS/CTS flow control | **off** |
| Telemetry | Inject LINK_STATISTICS |

### Which firmware on which board

| Board | Peer on USART2 | CRSF baud | Build env |
|-------|----------------|-----------|-----------|
| Ground, SineLink master | master UART1, not inverted, full duplex | 400000 | `genericSTM32F103C6` |
| Radio side, separate TX/RX pads | radio UART, not inverted | 400000 | `genericSTM32F103C6` |
| Radio side, TX12 module bay | bay pin 5, inverted, half duplex | 400000 | `genericSTM32F103C6_bay` |
| Any peer set to 416700 | full duplex, not inverted | 416700 | `genericSTM32F103C6_rx` |

The master is a CRSF transmitter module. It sends telemetry back on the same port where it receives RC frames. If no RC frames reach it from the TX12 end of the Ethernet link, it may send nothing, so test the full chain or feed it RC frames.

### Checking the Ethernet side

```bash
# CH9121 in UDP mode with destination = this PC, port 2000
python3 tools/crsf_eth_monitor.py --udp 2000
# CH9121 in TCP CLIENT mode with destination = this PC, port 2000
python3 tools/crsf_eth_monitor.py --tcp-server 2000
```

The tool prints raw byte count, CRSF frame counts by type, and CRC failures once per second. With a live SineLink link you should see `RC_CHANNELS` and `LINK_STATISTICS` climbing and `bad_crc=0`. Raw bytes with only `bad_crc` means a baud mismatch on one of the UARTs. No bytes means the CH9121 is not configured (factory default is 9600 baud, TCP client to 192.168.1.100:1000) or not reaching this PC.

## Why 921600

On a 72 MHz STM32 the USART divisor for 921600 is `4 + 14/16` (BRR `0x4E`). Actual rate is 923077 baud, **+0.16%**, inside the CH9121 receive tolerance (2%). 400000 on USART2 is exact.

## Pinout (STM32F103C6T6)

| Signal | MCU pin | Notes |
|--------|---------|--------|
| CRSF TX (MCU → CRSF device) | **PA2** | USART2 TX |
| CRSF RX (CRSF device → MCU) | **PA3** | USART2 RX |
| CH9121 TXD → MCU | **PA10** | USART1 RX |
| MCU → CH9121 RXD | **PA9** | USART1 TX |
| GND | GND | Common ground everywhere |
| 3.3 V | 3V3 | Logic is 3.3 V |
| Heartbeat LED | **PC13** | Active-low, optional |

Cross TX↔RX between peers (MCU TX → peer RX).

### Crystal

Firmware expects an **8 MHz HSE** crystal (Blue Pill / common F103 modules) and runs the core at **72 MHz**. If your board has a different crystal, say so and the clock setup can be adjusted.

## Hardware wiring

### Board A — CRSF receiver side

```
CRSF receiver TX ────────► STM32 PA3 (USART2 RX)
CRSF receiver RX ◄──────── STM32 PA2 (USART2 TX)
CRSF GND ───────────────── STM32 GND
CRSF 3V3 / 5V (per RX) ─── power as required by the receiver

STM32 PA9  (USART1 TX) ──► CH9121 RXD1
STM32 PA10 (USART1 RX) ◄── CH9121 TXD1
STM32 GND ──────────────── CH9121 GND
STM32 3V3 ──────────────── CH9121 VCCIO / logic supply (per module)
```

### Board B — TX12 / TX16 side

Same STM32↔CH9121 wiring. CRSF side goes to the radio (module bay / external CRSF pad) at **400000**.

**Inverted UART:** many JR-style module bays need inverted TTL. The F103 USART has no hardware invert. If the radio expects inverted CRSF, put a simple inverter (transistor / 74HC04 / dedicated inverter) on PA2/PA3 for that board only. ELRS / CRSF receivers are usually non-inverted.

**Half-duplex single wire:** if the pad is one wire, tie PA2 and PA3 with ~1 kΩ (TX drives, RX listens) and treat the pad as that node. Prefer separate TX/RX when the hardware allows it.

### Wiring tips for 921600

- Keep STM32↔CH9121 wires short (a few cm).
- Solid common GND.
- Prefer CH9121 **port 1** (has RTS/CTS on CH9121T if you later add flow control).

## CH9121 configuration (both modules)

Use **NetModuleConfig** (or serial CFG commands). Critical UART settings:

| Parameter | Value |
|-----------|--------|
| Serial clock mode | **Default** (the mode that offers 230400 / 460800 / 921600) |
| Baud rate | **921600** |
| Data / stop / parity | 8N1 |
| RX packet length | 64–256 (smaller = lower latency) |
| RX packet timeout | **0** (hardware timeout, best for realtime) |

Network example (adjust IPs to your LAN):

| | Module A (RX side) | Module B (radio side) |
|--|--------------------|------------------------|
| Mode | TCP Client *or* UDP | TCP Server *or* UDP |
| Device IP | e.g. 192.168.1.10 | e.g. 192.168.1.11 |
| Local port | 2000 | 2000 |
| Destination | B’s IP:2000 | A’s IP:2000 (if client) |

**UDP** is usually better for CRSF (no reconnect stall). Point each module at the other IP/port.

Set **both** CH9121 UARTs to 921600. Default clock mode is required for that rate; Classical mode does not include 921600.

## TX12 module bay, two-way

Bay pin 5 is one inverted half-duplex wire. Two NPN transistors invert it back to a normal UART and let the STM32 drive the wire only while sending. Flash **only the radio-side STM32** with the `genericSTM32F103C6_bay` firmware. The receiver-side board keeps the normal full-duplex build.

The bay build runs at **400000 baud** and forwards bytes transparently. It does
not insert delimiters or diagnostic marker bytes.

EdgeTX: Internal RF **Off**, External RF **CRSF**, baud **400000**.

![TX12/TX16 module-bay inverter schematic](docs/module-bay-interface.svg)

```text
Bay pin 3 ---- 3.3 V regulator ---- STM32 3.3 V
Bay pin 4 ----------------------- GND

                                     3.3 V
                                       |
                                    [4.7 kΩ]
                                       |
Bay pin 5 ----+----[1 kΩ]---- C Q1     |
              |                 E ---- GND
              |                 B ----[4.7 kΩ]---- PA2 (pin 12, TX)
              |
              +----[4.7 kΩ]----+---- B Q2
                                     |      E ---- GND
                                     |
                                     +-- A --|>|-- K --+
                                              D1 BAT54  |
                                                       |
                                          C Q2 --------+---- PA3 (pin 13, RX)
                                                       |
                                                    [4.7 kΩ]
                                                       |
                                                     3.3 V
```

The stripe/cathode end of D1 faces the Q2 collector and PA3. Use a fast
Schottky diode such as BAT54. It prevents Q2 from entering deep saturation,
which otherwise makes the rising edge too slow at 400 kbaud.
BC547 uses the opposite TO-92 order: **C B E**. Q1 sends to the radio. Q2
receives from the radio. PA9/PA10 still go straight to the CH9121.

```bash
pio run -e genericSTM32F103C6_bay
~/.platformio/penv/bin/python tools/flash_via_esp32.py .pio/build/genericSTM32F103C6_bay/firmware.bin
```

Disconnect the CH9121 from PA9/PA10 before that upload, then reconnect it. The level-shifter module is for separate 5 V TX/RX pads. It does not replace this circuit.

## Program the STM32 with the YD-ESP32-23

The YD-ESP32-23 (ESP32-S3) is the programmer. Its USB-UART port is `/dev/ttyACM0`. Unplug the CH9121 from PA9/PA10 while programming. Put a **10 kΩ resistor from STM32 BOOT0 to GND** so BOOT0 stays low while the ESP32 is resetting.

| YD-ESP32-23 | STM32F103C6T6 |
|-------------|----------------|
| **3V3** | 3V3 |
| **G** | GND |
| **17** | **PA10** (pin 31, USART1 RX) |
| **18** | **PA9** (pin 30, USART1 TX) |
| **4** | **BOOT0** (pin 44) |
| **5** | **NRST** (pin 7) |

BOOT1 / PB2 stays at GND.

Flash the programmer onto the ESP32 once (this replaces whatever firmware is on that board):

```bash
cd ~/Projects/crsf-bridge/programmer
pio run -t upload
```

Build the bridge, then send it through the ESP32:

```bash
cd ~/Projects/crsf-bridge
pio run -e genericSTM32F103C6
~/.platformio/penv/bin/python tools/flash_via_esp32.py .pio/build/genericSTM32F103C6/firmware.bin
```

Check the wiring without erasing the STM32:

```bash
~/.platformio/penv/bin/python tools/flash_via_esp32.py --id
```

`ID 0412` is the STM32F103C6. After `DONE`, the ESP32 releases BOOT0 and the bridge firmware runs. Reconnect the CH9121 to PA9/PA10 only after disconnecting GPIO17 and GPIO18.

## Build & flash with ST-Link

```bash
cd ~/Projects/crsf-bridge
pio run -e genericSTM32F103C6
pio run -e genericSTM32F103C6 -t upload
```

ST-Link on SWDIO / SWCLK / GND / 3V3. For a Blue Pill / C8 board use `-e bluepill_f103c8`.

### Upload over serial (no ST-Link)

The ROM bootloader is on **USART1**, the same pins as the CH9121. Unplug the CH9121 from PA9/PA10 before flashing.

| USB-UART (3.3 V) | STM32 |
|------------------|--------|
| TX | **PA10** (USART1 RX) |
| RX | **PA9** (USART1 TX) |
| GND | GND |

1. **BOOT0** (pin 44) to 3.3 V. **BOOT1 / PB2** (pin 20) stays at GND.
2. Press reset, or power-cycle.
3. Upload:

```bash
pio run -e genericSTM32F103C6_serial -t upload
```

`upload_port` in `platformio.ini` is `/dev/ttyUSB0`. Change it if the adapter enumerates as another device.

4. **BOOT0** back to GND, reconnect the CH9121, press reset. The bridge firmware then runs from flash.

## Baud math (sanity check)

At 72 MHz:

- USART1 (APB2 = 72 MHz), 921600 → BRR `0x4E` → 923077 baud (+0.16%)
- USART2 (APB1 = 36 MHz), 400000 → `USARTDIV = 5.625` → exact  

## What this does / does not do

- **Does:** byte-transparent bridge, both directions, interrupt + ring buffers  
- **Does not:** parse CRSF, change frame timing, or configure the CH9121 for you  

If packets look corrupted, verify **921600** on **both** CH9121s and that radio/RX stay at 400000.
