#include "radio.h"

#include <RadioLib.h>
#include <SPI.h>

#include "config.h"

namespace Radio {

namespace {
Module loraModule(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RESET, PIN_LORA_BUSY);
SX1262 radio(&loraModule);

volatile bool receivedFlag = false;
volatile bool interruptArmed = true;

// Rolling token bucket for the EU868 1% duty-cycle limit. Refilled continuously,
// capped at one hour's worth of allowance, spent per transmit() based on the
// radio's own airtime calculation for the exact packet just sent.
double dutyCycleBudgetUs = 0.0;
uint32_t lastBudgetRefillMs = 0;
const double kDutyCycleCapacityUs = 3600.0 * 1.0e6 * LORA_DUTY_CYCLE_FRACTION;

void IRAM_ATTR onPacketReceived() {
  if (interruptArmed) {
    receivedFlag = true;
  }
}

void refillBudget() {
  uint32_t now = millis();
  uint32_t elapsedMs = now - lastBudgetRefillMs; // wraps correctly even across millis() overflow
  lastBudgetRefillMs = now;
  dutyCycleBudgetUs += static_cast<double>(elapsedMs) * 1000.0 * LORA_DUTY_CYCLE_FRACTION;
  if (dutyCycleBudgetUs > kDutyCycleCapacityUs) {
    dutyCycleBudgetUs = kDutyCycleCapacityUs;
  }
}

void startListening() {
  receivedFlag = false;
  interruptArmed = true;
  int state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio: startReceive failed, code %d\n", state);
  }
}

// RadioLib's SX126x::getTimeOnAir() only counts the raw payload bits for FSK
// (len*8/bitRate) -- unlike its LoRa branch, it does NOT include preamble,
// sync word, the variable-packet-length-mode length prefix byte, or the CRC.
// Since the legally-mandated 1% duty-cycle budget must reflect the real time
// the antenna is active, we compute the full on-air frame ourselves here
// instead of trusting the library value. Keep the byte counts in sync with
// the begin() config below (preamble/sync/CRC) if any of it ever changes.
double fullFrameTimeOnAirUs(size_t payloadLen) {
  const double kLengthPrefixBytes = 1.0; // added on air by variablePacketLengthMode()
  double totalBits = GFSK_PREAMBLE_LENGTH_BITS +
                      (GFSK_SYNC_WORD_LEN + kLengthPrefixBytes + static_cast<double>(payloadLen) +
                       GFSK_CRC_LEN_BYTES) *
                          8.0;
  return (totalBits / (static_cast<double>(GFSK_BITRATE_KBPS) * 1000.0)) * 1.0e6;
}
} // namespace

void begin() {
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

  // Presence probe before touching beginFSK() at all. RadioLib's own
  // beginFSK() does detect a missing chip (via its internal findChip()), but
  // only after retrying its own verified reset up to 10 times, each bounded
  // ~1 s -- several seconds of the boot-time channel-select screen (see
  // BootChannelSelect in ui.cpp) looking frozen before a merely-absent
  // SX1262 is reported as non-fatal. Do the same verified reset once, up
  // front, bounded to ~1 s, and skip beginFSK() entirely if the chip never
  // answers. Mirrors the pin setup beginFSK() itself would do first (CS mode
  // + idle-high, BUSY as input) so the probe is meaningful on its own.
  loraModule.init();
  pinMode(PIN_LORA_BUSY, INPUT);
  int presence = radio.reset(/*verify=*/true);
  if (presence != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio: SX1262 not found, code %d -- device continues without radio.\n",
                   presence);
    return;
  }

  int state = radio.beginFSK(GFSK_FREQUENCY_MHZ, GFSK_BITRATE_KBPS, GFSK_FREQUENCY_DEVIATION_KHZ,
                              GFSK_RX_BANDWIDTH_KHZ, GFSK_TX_POWER_DBM, GFSK_PREAMBLE_LENGTH_BITS,
                              LORA_TCXO_VOLTAGE);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio: SX1262 init failed, code %d -- device continues without radio.\n",
                   state);
    return;
  }

  // Group channel: shifts the second sync-word byte, see config.h. Devices on
  // different channels never see each other's packets.
  uint8_t syncWord[GFSK_SYNC_WORD_LEN] = GFSK_SYNC_WORD_BYTES;
  syncWord[GFSK_SYNC_WORD_LEN - 1] += DeviceConfig::channel();
  radio.setSyncWord(syncWord, GFSK_SYNC_WORD_LEN);
  radio.setCRC(GFSK_CRC_LEN_BYTES);
  // Gaussian pulse shaping (BT = 0.5) is what makes this GFSK rather than plain
  // 2-FSK -- narrows the occupied spectrum without changing the bit rate.
  radio.setDataShaping(RADIOLIB_SHAPING_0_5);

  // Single-pin RF switch: RadioLib drives it automatically (low = idle,
  // high while actively transmitting or receiving); the same pin is passed
  // for both the "rx enable" and "tx enable" roles.
  radio.setRfSwitchPins(PIN_LORA_ANT_SW, PIN_LORA_ANT_SW);

  radio.setPacketReceivedAction(onPacketReceived);

  lastBudgetRefillMs = millis();
  // Start empty, not full: crediting a full hour's allowance on every boot
  // would let a device that brownout-reboots mid-ride (a real scenario, see
  // config.h's BOOT_CHANNEL_SELECT_TIMEOUT_MS comment) exceed the EU868 1%
  // duty-cycle limit across the reboot boundary by simply power-cycling. The
  // budget refills on its own (refillBudget(), called from every poll()/
  // send()) at the same steady rate either way -- this only costs the first
  // ~833 ms after boot before there's enough banked to send the first
  // ~8.3 ms-airtime heartbeat, not perceptible against
  // BOOT_CHANNEL_SELECT_TIMEOUT_MS's own 10 s window.
  dutyCycleBudgetUs = 0.0;

  startListening();
  Serial.println("Radio: SX1262 ready (EU868, GFSK).");
}

void applyChannel(uint8_t channel) {
  uint8_t syncWord[GFSK_SYNC_WORD_LEN] = GFSK_SYNC_WORD_BYTES;
  syncWord[GFSK_SYNC_WORD_LEN - 1] += channel;
  int state = radio.setSyncWord(syncWord, GFSK_SYNC_WORD_LEN);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio: setSyncWord failed, code %d\n", state);
  }
  startListening();
  Serial.printf("Radio: channel %u active.\n", channel);
}

bool poll(Protocol::DecodedPacket &pkt, int16_t &rssi) {
  refillBudget();

  if (!receivedFlag) {
    return false;
  }
  receivedFlag = false;

  size_t len = radio.getPacketLength();
  if (len == 0 || len > Protocol::kMaxPacketLen) {
    startListening();
    return false;
  }

  uint8_t buf[Protocol::kMaxPacketLen];
  int state = radio.readData(buf, len);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio: readData error, code %d\n", state);
    startListening();
    return false;
  }

  rssi = static_cast<int16_t>(lroundf(radio.getRSSI()));

  startListening();

  return Protocol::decode(buf, len, pkt);
}

bool send(uint8_t *data, size_t len) {
  refillBudget();

  double timeOnAirUs = fullFrameTimeOnAirUs(len);
  if (timeOnAirUs > dutyCycleBudgetUs) {
    Serial.println("Radio: duty-cycle budget exhausted, transmission skipped.");
    return false;
  }

  // Briefly disarm the RX-complete interrupt while we switch the radio into
  // TX mode -- it has no meaning during a transmit and we don't want a stray
  // callback firing mid-transition.
  interruptArmed = false;
  int state = radio.transmit(data, len);

  if (state == RADIOLIB_ERR_NONE) {
    dutyCycleBudgetUs -= timeOnAirUs;
  } else {
    // No antenna time was actually spent on a failed transmit -- charging the
    // budget anyway would needlessly (if conservatively) burn the shared
    // duty-cycle allowance on transient SPI/radio errors, and would also
    // disagree with Stats::countWarningSent()'s "only successful sends count"
    // policy (see main.cpp/ui.cpp callers).
    Serial.printf("Radio: transmit error, code %d\n", state);
  }

  startListening(); // transmit() leaves the radio in standby -- resume listening
  return state == RADIOLIB_ERR_NONE;
}

void sleep() {
  // Disarm first: RadioLib's standby() itself doesn't touch the interrupt,
  // and a stray RX-complete firing right as the radio drops out of receive
  // has no meaning once we're about to stop servicing it anyway.
  interruptArmed = false;
  int state = radio.standby();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio: standby failed, code %d\n", state);
  }
}

void resume() { startListening(); }

} // namespace Radio
