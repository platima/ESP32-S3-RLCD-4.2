#include "power.h"

#include <WiFi.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_rom_gpio.h>
#include <esp_sleep.h>
#include <soc/gpio_sig_map.h>

#include "config.h"
#include "fw_update.h"
#include "log.h"
#include "low_battery.h"

volatile bool g_powerDown = false;

namespace {

const char *const TAG = "power";
const uint32_t kLowMagic = 0x4C4F5742;     // "LOWB"
RTC_DATA_ATTR uint32_t s_lowShutdown = 0;  // kLowMagic after a low-battery shutdown; survives deep sleep, not a power cut

// The lines that must not float while the chip sleeps, and the level each idles at: the display's
// chip select, reset and data/command (high), its clock and data (low) and the audio amplifier's
// enable (low: off).  The pad hold is what keeps them there.
struct Pad {
  int pin;
  int level;
};
const Pad kPads[] = {{PIN_LCD_CS, 1}, {PIN_LCD_RST, 1}, {PIN_LCD_DC, 1}, {PIN_LCD_SCK, 0}, {PIN_LCD_MOSI, 0}, {46, 0}};

// Output at a fixed level.  The IDF calls rather than Arduino's pinMode(): once the SPI bus owns
// a pad, pinMode() detaches it from the bus, which takes the bus lock that the display driver
// holds (see ST7305_U8g2::busRelease()), and the task waits for itself for ever.
void driveLevel(const Pad &p) {
  esp_rom_gpio_connect_out_signal((uint32_t)p.pin, SIG_GPIO_OUT_IDX, false, false);  // the pad's own GPIO output, not a peripheral's
  gpio_set_direction((gpio_num_t)p.pin, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)p.pin, p.level);
}

}  // namespace

void powerSetCpuMhz(int mhz) {
  if ((int)getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz((uint32_t)mhz);
}

void powerAfterWake() {
  // The holds outlive deep sleep and also a watchdog reset, so this runs after every kind of
  // start.  gpio_hold_dis() hands a pad to its reset default (an input), so each one is driven
  // to the level it was held at first.
  for (const Pad &p : kPads) driveLevel(p);
  gpio_deep_sleep_hold_dis();
  for (const Pad &p : kPads) gpio_hold_dis((gpio_num_t)p.pin);
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) rtc_gpio_deinit((gpio_num_t)PIN_KEY);
}

bool powerWokeFromTimer() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER; }

bool powerWasLowShutdown() { return s_lowShutdown == kLowMagic; }

void powerForgetLowShutdown() { s_lowShutdown = 0; }

[[noreturn]] void powerDeepSleep(bool markLowShutdown) {
  fwConfirmNow();  // a firmware that puts itself to sleep on purpose has started properly; the next reset is no failure
  g_powerDown = true;
  if (markLowShutdown) s_lowShutdown = kLowMagic;
  delay(50);  // a WiFi call the network task is in the middle of gets a moment to finish
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  for (const Pad &p : kPads) {
    driveLevel(p);
    gpio_hold_en((gpio_num_t)p.pin);
  }
  gpio_deep_sleep_hold_en();

  // Ways out: the timer (to look at the battery) and KEY, pulled up on the chip.  A KEY that is
  // down right now (still held from the press that woke us, or stuck) would wake the chip at
  // once and for ever, so it is given a few seconds to come up and left out if it does not.
  esp_sleep_enable_timer_wakeup((uint64_t)lowbat::kSleepCheckSec * 1000000ULL);
  pinMode(PIN_KEY, INPUT_PULLUP);
  for (int i = 0; i < 300 && digitalRead(PIN_KEY) == LOW; i++) delay(10);
  if (digitalRead(PIN_KEY) == HIGH) {
    rtc_gpio_init((gpio_num_t)PIN_KEY);
    rtc_gpio_set_direction((gpio_num_t)PIN_KEY, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_KEY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_KEY);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // the pull-up needs it
    const esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_KEY, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) LOGF(TAG, "KEY wake-up not armed: 0x%x", (unsigned)err);
  } else {
    LOGF(TAG, "KEY is held down: sleeping on the timer alone");
  }

  Serial.flush();
  esp_deep_sleep_start();
  for (;;) {  // not reached
  }
}
