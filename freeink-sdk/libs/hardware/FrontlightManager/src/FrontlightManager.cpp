#include "FrontlightManager.h"

#if FREEINK_CAP_FRONTLIGHT
#include <M5Pm1.h>

namespace {
constexpr uint32_t maxDuty(uint8_t bits) { return (1u << bits) - 1u; }

// Paper Mono: the PWM lives in the M5PM1 PMIC, not the ESP. PM1 GPIO3 routed to
// alt-function PWM0 drives the AW9967 frontlight driver. Duty register is
// 12-bit; the high byte's bit 4 is the channel-enable bit. Perception-weighted
// like M5Unified's bring-up: duty = brightness^2 scaled into 12 bits.
constexpr uint8_t PM1_PWM_ENABLE = 0x10;

void pm1FrontlightAttach(uint32_t freqHz) {
  freeink::m5pm1::beginBus();
  // GPIO3 to push-pull, alt-function PWM0.
  freeink::m5pm1::updateReg(freeink::m5pm1::REG_GPIO_DRV, 1u << 3, 0);
  freeink::m5pm1::updateReg(freeink::m5pm1::REG_GPIO_FUNC0, 0xC0, 0xC0);
  freeink::m5pm1::writeReg16(freeink::m5pm1::REG_PWM_FREQ_L, static_cast<uint16_t>(freqHz));
}

void pm1FrontlightWrite(uint32_t pct) {
  const uint32_t duty = (pct * pct * 4095u) / 10000u;  // 0-100% -> 12-bit, gamma ~2
  const uint8_t data[2] = {static_cast<uint8_t>(duty & 0xFF),
                           static_cast<uint8_t>(((duty >> 8) & 0x0F) | (duty ? PM1_PWM_ENABLE : 0))};
  freeink::m5pm1::writeBytes(freeink::m5pm1::REG_PWM0_DUTY_L, data, sizeof(data));
}

// Fixed LEDC channels for the Arduino-ESP32 2.x path (3.x keys by GPIO and allocates
// channels itself). Frontlight owns 0 (cool/primary) and 1 (warm); no other SDK LEDC
// user on a frontlight board takes these (the Buzzer uses the 3.x gpio-keyed API).
constexpr uint8_t LEDC_CH_COOL = 0;
constexpr uint8_t LEDC_CH_WARM = 1;

// Turn a 0-100 percentage into a duty, honoring active level. `pct` is pre-clamped.
// Mild gamma (~1.6 via n^2/100 blend) keeps low percentages visibly dimmer and
// more even than pure linear (matches stock dual-LED frontlights better). When
// pct > 0, never collapse to duty 0 so 1% is still a real (very low) level.
uint32_t dutyFor(uint32_t pct, uint32_t full, bool activeHigh) {
  if (pct == 0 || full == 0) {
    return activeHigh ? 0u : full;
  }
  // Linear term + square term: soft toe at the bottom of the range.
  const uint32_t linear = (pct * full) / 100u;
  const uint32_t squared = (pct * pct * full) / 10000u;
  uint32_t duty = (linear + squared) / 2u;
  if (duty < 1u) duty = 1u;
  if (duty > full) duty = full;
  return activeHigh ? duty : full - duty;
}

#if defined(ARDUINO) && ESP_ARDUINO_VERSION_MAJOR >= 3
void attachChannel(int8_t gpio, uint8_t /*ch*/, uint32_t freq, uint8_t bits) {
  ledcAttach(gpio, freq, bits);
}
void writeChannel(int8_t gpio, uint8_t /*ch*/, uint32_t duty) { ledcWrite(gpio, duty); }
#else
void attachChannel(int8_t gpio, uint8_t ch, uint32_t freq, uint8_t bits) {
  ledcSetup(ch, freq, bits);
  ledcAttachPin(gpio, ch);
}
void writeChannel(int8_t /*gpio*/, uint8_t ch, uint32_t duty) { ledcWrite(ch, duty); }
#endif
}  // namespace
#endif

void FrontlightManager::begin() {
#if FREEINK_CAP_FRONTLIGHT
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  if (fl.viaPm1Pwm) {
    pm1FrontlightAttach(fl.pwmFrequency);
    _begun = true;
    setBrightness(0);
    return;
  }
  if (fl.gpio == BoardConfig::PIN_UNASSIGNED) return;

  attachChannel(fl.gpio, LEDC_CH_COOL, fl.pwmFrequency, fl.pwmResolutionBits);
  if (fl.gpioWarm != BoardConfig::PIN_UNASSIGNED) {
    attachChannel(fl.gpioWarm, LEDC_CH_WARM, fl.pwmFrequency, fl.pwmResolutionBits);
  }
  _begun = true;
  setBrightness(0);
#endif
}

#if FREEINK_CAP_FRONTLIGHT
void FrontlightManager::apply() {
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  if (!_begun) return;
  if (fl.viaPm1Pwm) {
    pm1FrontlightWrite(_brightness);
    return;
  }
  if (fl.gpio == BoardConfig::PIN_UNASSIGNED) return;

  const uint32_t full = maxDuty(fl.pwmResolutionBits);
  const bool dual = fl.gpioWarm != BoardConfig::PIN_UNASSIGNED;

  // Single channel: primary carries the whole brightness.
  if (!dual) {
    writeChannel(fl.gpio, LEDC_CH_COOL, dutyFor(_brightness, full, fl.activeHigh));
    return;
  }

  // Dual warm/cool: gamma the TOTAL brightness once, then split that duty by CT.
  // Previous code split percent first then gamma'd each share — as brightness
  // stepped 1%, warmShare rounded 4/5/4/5… so each click alternated warmer/cooler
  // instead of only dimming. Splitting after gamma keeps CT stable while dimming.
  //
  // dutyFor() already returns the pin-level value (inverted if active-low). For
  // the split we need the "energy" amount on a linear 0..full scale first.
  uint32_t totalEnergy = 0;
  if (_brightness > 0) {
    const uint32_t linear = (static_cast<uint32_t>(_brightness) * full) / 100u;
    const uint32_t squared = (static_cast<uint32_t>(_brightness) * _brightness * full) / 10000u;
    totalEnergy = (linear + squared) / 2u;
    if (totalEnergy < 1u) totalEnergy = 1u;
    if (totalEnergy > full) totalEnergy = full;
  }

  uint32_t warmEnergy = (totalEnergy * static_cast<uint32_t>(_warmPercent) + 50u) / 100u;
  if (warmEnergy > totalEnergy) warmEnergy = totalEnergy;
  uint32_t coolEnergy = totalEnergy - warmEnergy;

  // Preserve pure cool / pure warm endpoints; only force a 1-count floor when
  // the requested mix is interior (1–99%) and brightness is non-zero so neither
  // string extinguishes and snaps CT.
  if (_brightness > 0 && _warmPercent > 0 && _warmPercent < 100) {
    if (coolEnergy == 0 && totalEnergy >= 2) {
      coolEnergy = 1;
      warmEnergy = totalEnergy - 1;
    } else if (warmEnergy == 0 && totalEnergy >= 2) {
      warmEnergy = 1;
      coolEnergy = totalEnergy - 1;
    }
  }

  auto pinDuty = [&](uint32_t energy) -> uint32_t {
    if (energy == 0) return fl.activeHigh ? 0u : full;
    if (energy > full) energy = full;
    return fl.activeHigh ? energy : full - energy;
  };
  writeChannel(fl.gpio, LEDC_CH_COOL, pinDuty(coolEnergy));
  writeChannel(fl.gpioWarm, LEDC_CH_WARM, pinDuty(warmEnergy));
}
#endif

void FrontlightManager::setBrightness(uint8_t percent) {
#if FREEINK_CAP_FRONTLIGHT
  if (percent > 100) percent = 100;
  _brightness = percent;
  if (percent > 0) _lastBrightness = percent;
  apply();
#else
  (void)percent;
#endif
}

void FrontlightManager::off() { setBrightness(0); }
void FrontlightManager::on() { setBrightness(_lastBrightness); }

void FrontlightManager::setColorTemperature(uint8_t warmPercent) {
#if FREEINK_CAP_FRONTLIGHT
  _warmPercent = warmPercent > 100 ? 100 : warmPercent;
  // Only re-drives hardware when a warm channel exists; on single-channel boards this just
  // records the request (apply() ignores _warmPercent without a second channel).
  apply();
#else
  (void)warmPercent;
#endif
}
