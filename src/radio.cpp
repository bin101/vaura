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
    Serial.printf("Radio: startReceive fehlgeschlagen, Code %d\n", state);
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

  int state = radio.beginFSK(GFSK_FREQUENCY_MHZ, GFSK_BITRATE_KBPS, GFSK_FREQUENCY_DEVIATION_KHZ,
                              GFSK_RX_BANDWIDTH_KHZ, GFSK_TX_POWER_DBM, GFSK_PREAMBLE_LENGTH_BITS,
                              LORA_TCXO_VOLTAGE);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio: SX1262 Init fehlgeschlagen, Code %d -- Geraet laeuft ohne Funk weiter.\n",
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
  dutyCycleBudgetUs = kDutyCycleCapacityUs; // start with a full tank

  startListening();
  Serial.println("Radio: SX1262 bereit (EU868, GFSK).");
}

void applyChannel(uint8_t channel) {
  uint8_t syncWord[GFSK_SYNC_WORD_LEN] = GFSK_SYNC_WORD_BYTES;
  syncWord[GFSK_SYNC_WORD_LEN - 1] += channel;
  int state = radio.setSyncWord(syncWord, GFSK_SYNC_WORD_LEN);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio: setSyncWord fehlgeschlagen, Code %d\n", state);
  }
  startListening();
  Serial.printf("Radio: Kanal %u aktiv.\n", channel);
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
    Serial.printf("Radio: readData Fehler, Code %d\n", state);
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
    Serial.println("Radio: Duty-Cycle-Budget erschoepft, Sendung uebersprungen.");
    return false;
  }

  // Briefly disarm the RX-complete interrupt while we switch the radio into
  // TX mode -- it has no meaning during a transmit and we don't want a stray
  // callback firing mid-transition.
  interruptArmed = false;
  int state = radio.transmit(data, len);
  dutyCycleBudgetUs -= timeOnAirUs;

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio: transmit Fehler, Code %d\n", state);
  }

  startListening(); // transmit() leaves the radio in standby -- resume listening
  return state == RADIOLIB_ERR_NONE;
}

} // namespace Radio
