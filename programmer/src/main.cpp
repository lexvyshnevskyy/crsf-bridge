/**
 * @file main.cpp
 * @brief STM32F103 UART programmer for the YD-ESP32-23 (ESP32-S3).
 *
 * USB-UART (CH343, /dev/ttyACM0) is the host console at 115200 8N1.
 * GPIO17/18 talk to the STM32 ROM bootloader at 115200 8E1.
 * GPIO4 drives BOOT0, GPIO5 drives NRST (open-drain).
 *
 * Host protocol (one command per line):
 *   PING
 *   ID
 *   FLASH <size> <crc32hex>     then exactly <size> raw bytes
 */

#include <Arduino.h>
#include <cstring>
#include <cstdio>

static const int PIN_STM_TX = 17;  // ESP TX -> STM32 PA10 RX
static const int PIN_STM_RX = 18;  // ESP RX <- STM32 PA9 TX
static const int PIN_BOOT0 = 4;
static const int PIN_NRST = 5;

static const uint8_t ACK = 0x79;
static const uint8_t NACK = 0x1F;
static const uint32_t FLASH_BASE = 0x08000000;
static const size_t FLASH_MAX = 32 * 1024;

static HardwareSerial Console(0);
static HardwareSerial Stm32(1);
static uint8_t image[FLASH_MAX];

/** @brief Put BOOT0 and NRST in the normal-run idle state. */
static void pins_idle() {
  pinMode(PIN_BOOT0, OUTPUT);
  pinMode(PIN_NRST, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_BOOT0, LOW);
  digitalWrite(PIN_NRST, HIGH);
}

/** @brief Reset the target into its ROM bootloader or application. @param[in] bootloader True selects the ROM bootloader. */
static void stm32_reset_into(bool bootloader) {
  digitalWrite(PIN_BOOT0, bootloader ? HIGH : LOW);
  delay(5);
  digitalWrite(PIN_NRST, LOW);
  delay(20);
  digitalWrite(PIN_NRST, HIGH);
  delay(50);
}

/** @brief Read one byte from the STM32 with a timeout. @param[in] timeout_ms Timeout in milliseconds. @return Byte value or -1 on timeout. */
static int read_stm(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    if (Stm32.available()) {
      return Stm32.read();
    }
  }
  return -1;
}

/** @brief Discard all pending bytes from the target UART. */
static void drain_stm() {
  while (Stm32.available()) {
    Stm32.read();
  }
}

/** @brief Wait for a ROM-loader ACK or NACK. @param[in] timeout_ms Timeout in milliseconds. @return True only for ACK. */
static bool wait_ack(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    int b = read_stm(timeout_ms);
    if (b < 0) {
      return false;
    }
    if (b == ACK) {
      return true;
    }
    if (b == NACK) {
      return false;
    }
  }
  return false;
}

/** @brief Send a bootloader command and its complement. @param[in] cmd Command byte. @return True when acknowledged. */
static bool send_cmd(uint8_t cmd) {
  uint8_t frame[2] = {cmd, static_cast<uint8_t>(~cmd)};
  Stm32.write(frame, 2);
  Stm32.flush();
  return wait_ack(1000);
}

/** @brief Synchronize with the STM32 system bootloader. @return True on success. */
static bool sync_bootloader() {
  for (int attempt = 0; attempt < 5; attempt++) {
    drain_stm();
    Stm32.write(static_cast<uint8_t>(0x7F));
    Stm32.flush();
    if (wait_ack(300)) {
      return true;
    }
    delay(20);
  }
  return false;
}

/** @brief Read an exact number of target bytes. @param[out] dst Destination buffer. @param[in] n Byte count. @param[in] timeout_ms Per-byte timeout. @return True on success. */
static bool read_exact(uint8_t *dst, int n, uint32_t timeout_ms) {
  for (int i = 0; i < n; i++) {
    int b = read_stm(timeout_ms);
    if (b < 0) {
      return false;
    }
    dst[i] = static_cast<uint8_t>(b);
  }
  return true;
}

/** @brief Read the target chip ID and erase capability. @param[out] has_ext_erase Set when extended erase is advertised. @return Chip ID, or zero on failure. */
static uint16_t read_chip_id(bool *has_ext_erase) {
  *has_ext_erase = false;
  if (!send_cmd(0x00)) {
    return 0;
  }
  int n = read_stm(500);
  if (n < 0) {
    return 0;
  }
  uint8_t body[40];
  int total = n + 1;
  if (total > static_cast<int>(sizeof body)) {
    return 0;
  }
  if (!read_exact(body, total, 500) || !wait_ack(500)) {
    return 0;
  }
  for (int i = 1; i < total; i++) {
    if (body[i] == 0x44) {
      *has_ext_erase = true;
    }
  }

  if (!send_cmd(0x02)) {
    return 0;
  }
  int count = read_stm(500);
  if (count < 1) {
    return 0;
  }
  uint8_t idb[4];
  int nbytes = count + 1;
  if (nbytes > 4 || !read_exact(idb, nbytes, 500) || !wait_ack(500)) {
    return 0;
  }
  return (static_cast<uint16_t>(idb[0]) << 8) | idb[1];
}

/** @brief Erase all target flash. @param[in] extended Select extended-erase command. @return True on success. */
static bool mass_erase(bool extended) {
  if (extended) {
    if (!send_cmd(0x44)) {
      return false;
    }
    uint8_t frame[3] = {0xFF, 0xFF, 0x00};
    Stm32.write(frame, 3);
    Stm32.flush();
    return wait_ack(8000);
  }
  if (!send_cmd(0x43)) {
    return false;
  }
  uint8_t frame[2] = {0xFF, 0x00};
  Stm32.write(frame, 2);
  Stm32.flush();
  return wait_ack(8000);
}

/** @brief Write one block through the ROM bootloader. @param[in] addr Flash address. @param[in] data Block data. @param[in] len Block length. @return True on success. */
static bool write_block(uint32_t addr, const uint8_t *data, size_t len) {
  if (!send_cmd(0x31)) {
    return false;
  }
  uint8_t addr_frame[5] = {
      static_cast<uint8_t>(addr >> 24),
      static_cast<uint8_t>(addr >> 16),
      static_cast<uint8_t>(addr >> 8),
      static_cast<uint8_t>(addr),
      0};
  addr_frame[4] = addr_frame[0] ^ addr_frame[1] ^ addr_frame[2] ^ addr_frame[3];
  Stm32.write(addr_frame, 5);
  Stm32.flush();
  if (!wait_ack(1000)) {
    return false;
  }

  uint8_t chunk[1 + 256 + 1];
  chunk[0] = static_cast<uint8_t>(len - 1);
  memcpy(chunk + 1, data, len);
  uint8_t sum = chunk[0];
  for (size_t i = 0; i < len; i++) {
    sum ^= data[i];
  }
  chunk[1 + len] = sum;
  Stm32.write(chunk, len + 2);
  Stm32.flush();
  return wait_ack(2000);
}

/** @brief Erase and program the buffered firmware image. @param[in] size Image size. @param[out] chip_id Detected target ID. @return True on success. */
static bool program_image(size_t size, uint16_t *chip_id) {
  stm32_reset_into(true);
  if (!sync_bootloader()) {
    Console.println("ERR sync");
    stm32_reset_into(false);
    return false;
  }
  bool extended = false;
  uint16_t id = read_chip_id(&extended);
  if (id == 0) {
    Console.println("ERR id");
    stm32_reset_into(false);
    return false;
  }
  *chip_id = id;
  Console.printf("SYNC %04X\n", id);

  if (!mass_erase(extended) && !mass_erase(!extended)) {
    Console.println("ERR erase");
    stm32_reset_into(false);
    return false;
  }
  Console.println("ERASE");

  size_t offset = 0;
  while (offset < size) {
    size_t n = size - offset;
    if (n > 128) {
      n = 128;
    }
    uint8_t block[128];
    memcpy(block, image + offset, n);
    while (n & 3u) {
      block[n++] = 0xFF;
    }
    if (!write_block(FLASH_BASE + offset, block, n)) {
      Console.println("ERR write");
      stm32_reset_into(false);
      return false;
    }
    offset += n;
  }
  Console.println("WRITE");
  stm32_reset_into(false);
  return true;
}

/** @brief Calculate reflected IEEE CRC-32. @param[in] data Input bytes. @param[in] len Input length. @return CRC-32 value. */
static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

/** @brief Read one host command line. @param[out] buf Destination. @param[in] cap Buffer capacity. @return True when a nonempty line was read. */
static bool read_line(char *buf, size_t cap) {
  size_t i = 0;
  uint32_t start = millis();
  while (millis() - start < 60000) {
    while (Console.available()) {
      char c = static_cast<char>(Console.read());
      if (c == '\n') {
        if (i > 0 && buf[i - 1] == '\r') {
          i--;
        }
        buf[i] = 0;
        return i > 0;
      }
      if (i + 1 < cap) {
        buf[i++] = c;
      }
      start = millis();
    }
  }
  return false;
}

/** @brief Receive a binary image from the host. @param[in] size Expected byte count. @return True when complete. */
static bool read_image(size_t size) {
  size_t got = 0;
  uint32_t start = millis();
  while (got < size) {
    if (Console.available()) {
      int n = Console.read(image + got, size - got);
      if (n > 0) {
        got += static_cast<size_t>(n);
        start = millis();
      }
    } else if (millis() - start > 8000) {
      return false;
    }
  }
  return true;
}

/** @brief Calculate the CRSF DVB-S2 CRC-8. @param[in] d Input bytes. @param[in] n Input length. @return CRC-8 value. */
static uint8_t crsf_crc8(const uint8_t *d, size_t n) {
  uint8_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0xD5) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

/** @brief Stream valid RC_CHANNELS test frames on GPIO17. @param[in] baud UART rate. @param[in] seconds Test duration. */
static void gen_crsf(uint32_t baud, uint32_t seconds) {
  uint16_t ch[16] = {992, 992, 172, 992, 172, 992, 1811, 992,
                     992, 992, 992, 992, 992, 992, 992, 992};
  uint8_t frame[26];
  frame[0] = 0xEE;   // addressed to the TX module, like a handset
  frame[1] = 24;
  frame[2] = 0x16;
  memset(frame + 3, 0, 22);
  uint32_t bitpos = 0;
  for (int i = 0; i < 16; i++) {
    for (int b = 0; b < 11; b++) {
      if ((ch[i] >> b) & 1) {
        frame[3 + bitpos / 8] |= static_cast<uint8_t>(1u << (bitpos % 8));
      }
      bitpos++;
    }
  }
  frame[25] = crsf_crc8(frame + 2, 23);

  Stm32.end();
  Stm32.begin(baud, SERIAL_8N1, PIN_STM_RX, PIN_STM_TX);
  delay(20);
  uint32_t end = millis() + seconds * 1000u;
  uint32_t sent = 0;
  while (static_cast<int32_t>(end - millis()) > 0) {
    Stm32.write(frame, sizeof frame);
    Stm32.flush();
    sent++;
    delay(4);   // ~250 Hz
  }
  Stm32.end();
  Stm32.begin(115200, SERIAL_8E1, PIN_STM_RX, PIN_STM_TX);
  Console.printf("GEN done %lu frames at %lu 8N1\n", static_cast<unsigned long>(sent),
                 static_cast<unsigned long>(baud));
}

/** @brief Initialize the host console, target UART, and control pins. */
void setup() {
  pins_idle();
  Console.setRxBufferSize(4096);
  Console.begin(115200, SERIAL_8N1, 44, 43);
  Stm32.begin(115200, SERIAL_8E1, PIN_STM_RX, PIN_STM_TX);
  delay(50);
  Console.println("READY");
}

/** @brief Process one host protocol command. */
void loop() {
  char line[80];
  if (!read_line(line, sizeof line)) {
    return;
  }

  if (strcmp(line, "PING") == 0) {
    Console.println("PONG");
    return;
  }

  if (strcmp(line, "ID") == 0) {
    stm32_reset_into(true);
    if (!sync_bootloader()) {
      Console.println("ERR sync");
      stm32_reset_into(false);
      return;
    }
    bool extended = false;
    uint16_t id = read_chip_id(&extended);
    stm32_reset_into(false);
    if (id == 0) {
      Console.println("ERR id");
    } else {
      Console.printf("ID %04X\n", id);
    }
    return;
  }

  unsigned gbaud = 0, gsec = 0;
  if (sscanf(line, "GEN %u %u", &gbaud, &gsec) == 2 && gbaud >= 9600 && gbaud <= 2000000 && gsec > 0 && gsec <= 600) {
    Console.printf("GEN start %u 8N1 for %u s on GPIO17\n", gbaud, gsec);
    gen_crsf(gbaud, gsec);
    return;
  }

  unsigned size = 0;
  unsigned crc = 0;
  if (sscanf(line, "FLASH %u %x", &size, &crc) != 2 || size == 0 || size > FLASH_MAX) {
    Console.println("ERR cmd");
    return;
  }
  if (!read_image(size)) {
    Console.println("ERR rx");
    return;
  }
  if (crc32_ieee(image, size) != crc) {
    Console.println("ERR crc");
    return;
  }

  uint16_t id = 0;
  if (program_image(size, &id)) {
    Console.printf("DONE %04X\n", id);
  }
}
