#include "log.h"

#include <atomic>

#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
#include <hal/usb_serial_jtag_ll.h>  // Serial is the chip's USB Serial/JTAG port
#endif

volatile bool g_consoleOn = true;

namespace {
std::atomic<int> s_printing{0};  // tasks that are inside a LOGF right now
}

bool logBegin() {
  s_printing.fetch_add(1);
  if (g_consoleOn) return true;
  s_printing.fetch_sub(1);
  return false;
}

void logEnd() { s_printing.fetch_sub(1); }

void consoleShutDown() {
  if (!g_consoleOn) return;
  Serial.flush();
  g_consoleOn = false;  // no new lines from here on ...
  for (int i = 0; i < 200 && s_printing.load() > 0; i++) delay(5);  // ... and the one another task is in the middle of gets to finish

  // Serial.end() stops the driver: the interrupt is freed, the buffers go, and on the USB port the core turns
  // D+ and D- into plain pins held low, which also takes the USB pad and its pull-up away from the USB block.
  Serial.end();
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // Said outright as well: the transceiver off.  (The block's bus clock is left running: the SDK's check for a
  // computer on the port keeps reading its registers from the tick interrupt.)
  usb_serial_jtag_ll_phy_enable_pad(false);
#endif
}
