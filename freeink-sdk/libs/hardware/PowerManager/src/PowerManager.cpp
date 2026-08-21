#include "PowerManager.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include <driver/rtc_io.h>
#endif

namespace freeink {
namespace {
int8_t powerPin() { return BoardConfig::ACTIVE.input.power; }
bool powerActiveHigh() { return BoardConfig::ACTIVE.input.powerActiveHigh; }

// Configure the power-button pad so a short press can wake from deep sleep.
//
// Two things must both be true on EXT1 parts (ESP32-S3 X4 Pro, GPIO3 active-low):
//   1) The pad lives in the RTC domain with the correct RTC pull for the idle
//      level (pull-up for active-low).
//   2) ESP_PD_DOMAIN_RTC_PERIPH stays ON — IDF shuts it down by default, and
//      internal RTC pulls do nothing while that domain is off. Isolate alone
//      then leaves the line floating; short presses miss EXT1 and only a long
//      hardware hold / reset appears to "wake" the device.
void configurePowerButtonPad(const int8_t pin, const bool activeHigh) {
  if (pin < 0) return;
  const auto g = static_cast<gpio_num_t>(pin);

  // Digital pull for any pre-sleep reads; RTC path below is what deep sleep uses.
  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
  if (rtc_gpio_is_valid_gpio(g)) {
    rtc_gpio_init(g);
    rtc_gpio_set_direction(g, RTC_GPIO_MODE_INPUT_ONLY);
    if (activeHigh) {
      rtc_gpio_pulldown_en(g);
      rtc_gpio_pullup_dis(g);
    } else {
      rtc_gpio_pullup_en(g);
      rtc_gpio_pulldown_dis(g);
    }
    // Keep RTC peripherals powered so the RTC pulls above actually hold.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  }
#else
  (void)g;
#endif
}
}  // namespace

void PowerManager::armWakeOnPins(uint64_t gpioMask, bool wakeLow) {
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  // Xtensa (S3/S2, classic ESP32): RTC ext1. Pins must be RTC GPIOs.
  //
  // The classic ESP32 RTC has no "any low" mode — only ESP_EXT1_WAKEUP_ALL_LOW
  // ("wake when ALL selected pins are low"). For a single wake pin (the common
  // power-button case) ALL_LOW and ANY_LOW are identical; a multi-pin low wake on
  // classic ESP32 fires only when every pin is low. S2/S3 expose ANY_LOW directly.
#if defined(CONFIG_IDF_TARGET_ESP32)
  const esp_sleep_ext1_wakeup_mode_t lowMode = ESP_EXT1_WAKEUP_ALL_LOW;
#else
  const esp_sleep_ext1_wakeup_mode_t lowMode = ESP_EXT1_WAKEUP_ANY_LOW;
#endif
  esp_sleep_enable_ext1_wakeup(gpioMask, wakeLow ? lowMode : ESP_EXT1_WAKEUP_ANY_HIGH);
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
  // RISC-V (C3/C6/H2): the deep-sleep "gpio" wakeup source.
  esp_deep_sleep_enable_gpio_wakeup(gpioMask, wakeLow ? ESP_GPIO_WAKEUP_GPIO_LOW : ESP_GPIO_WAKEUP_GPIO_HIGH);
#else
#error "FreeInk PowerManager: target has no supported deep-sleep GPIO wakeup source"
#endif
}

bool PowerManager::armPowerButtonWakeup() {
  const int8_t pin = powerPin();
  if (pin < 0) return false;
  const bool activeHigh = powerActiveHigh();

  configurePowerButtonPad(pin, activeHigh);
  armWakeOnPins(1ULL << pin, /*wakeLow=*/!activeHigh);
  return true;
}

void PowerManager::waitForPowerButtonRelease() {
  const int8_t pin = powerPin();
  if (pin < 0) return;
  const bool activeHigh = powerActiveHigh();

  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  const int pressedLevel = activeHigh ? HIGH : LOW;
  // Cap so a stuck/noisy button cannot hang forever before sleep (~5 s).
  const unsigned long start = millis();
  while (digitalRead(pin) == pressedLevel && (millis() - start) < 5000UL) {
    delay(50);
  }
}

namespace {
// Drive a rail-enable pin to `offLevel` and latch it so the level survives deep
// sleep (requires gpio_deep_sleep_hold_en(), done in deepSleep()). gpio_hold_dis
// first: a hold left over from a previous cycle would make the writes no-ops.
void holdRailOff(int8_t pin, uint8_t offLevel) {
  if (pin < 0) return;
  const auto g = static_cast<gpio_num_t>(pin);
  gpio_hold_dis(g);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, offLevel);
  gpio_hold_en(g);
}

// Keep a stay-alive latch asserted (HIGH) through deep sleep (Sticky PWR_HOLD).
// Peripheral load-switches use holdRailOff(LOW) instead — see keepAssertedInSleep.
void holdLatchOn(int8_t pin) {
  if (pin < 0) return;
  if (BoardConfig::latchConflictsWithBus(pin)) return;
  const auto g = static_cast<gpio_num_t>(pin);
  gpio_hold_dis(g);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  gpio_hold_en(g);
}
}  // namespace

void PowerManager::powerDownRailsForSleep() {
  const auto& b = BoardConfig::ACTIVE;
  // Hold the display RESET line at its idle level (HIGH) through deep sleep.
  // esp_sleep_config_gpio_isolate() otherwise floats it high-Z; a drifting
  // active-low RST can pull a controller out of its deep-sleep hold. The SSD1677
  // tolerates that (no external booster, and its deepSleep actively discharges),
  // but the UC8179 — which runs an explicit BTST-programmed DC-DC booster — does
  // not stay collapsed, so its analog restarts and drains the pack through "off"
  // (field report: dead in ~36 h). Holding RST high is the controller's normal
  // idle level, so it's harmless for the SSD1677 batch. Released on wake in
  // EpdBus::begin() before the reset pulse. (holdRailOff no-ops if RST unassigned.)
  holdRailOff(b.display.rst, HIGH);
  holdRailOff(b.display.powerEnable, LOW);
  // SD enable OFF = the inactive level: LOW for active-high enables, HIGH for the
  // active-low ones (e.g. X4 Pro's GPIO5, which powers the card while held LOW).
  holdRailOff(b.sd.powerEnable, b.sd.powerActiveHigh ? LOW : HIGH);
  holdRailOff(b.touch.powerEnable, b.touch.powerEnableActiveHigh ? LOW : HIGH);
  // The mic enable also carries a polarity flag; OFF is the inactive level.
  holdRailOff(b.mic.enable, b.mic.enableActiveHigh ? LOW : HIGH);

  // Frontlight PWM: isolate otherwise floats the pads. Active-high LEDs then
  // glow (tens of mA). Hold the off level so gpio_deep_sleep_hold_en keeps them
  // dark. Consumer should already have detached LEDC (FrontlightManager::holdOffForDeepSleep).
  {
    const uint8_t flOff = b.frontlight.activeHigh ? LOW : HIGH;
    holdRailOff(b.frontlight.gpio, flOff);
    holdRailOff(b.frontlight.gpioWarm, flOff);
  }

  // Keep CS idle-high so a floating chip-select cannot clock a sleeping panel
  // out of DSLP (GPIO13 is CS on X4 Pro — never treat it as the C3 X4 MOSFET).
  holdRailOff(b.display.cs, HIGH);

  // Stay-alive latches (Sticky) stay HIGH. Peripheral load-switches (X4 Pro
  // GPIO1) go LOW so EPD analog + SD VCC are actually off. Callers must have
  // already sent the panel deep-sleep command while this rail was still up.
  if (b.power.keepAssertedInSleep) {
    holdLatchOn(b.power.latch0);
    holdLatchOn(b.power.latch1);
  } else {
    holdRailOff(b.power.latch0, LOW);
    holdRailOff(b.power.latch1, LOW);
  }
}

void PowerManager::deepSleep() {
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
  while (true) {
  }  // esp_deep_sleep_start() does not return; satisfy [[noreturn]]
}

void PowerManager::deepSleepUntilPowerButton() {
  waitForPowerButtonRelease();
  // Isolate *before* arming. Arm-then-isolate left the wake pad without a
  // working pull on C3 gpio-wakeup and S3 EXT1. armPowerButtonWakeup() also
  // keeps RTC_PERIPH on so RTC pulls survive deep sleep.
  esp_sleep_config_gpio_isolate();
  (void)armPowerButtonWakeup();
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
  while (true) {
  }
}

}  // namespace freeink
