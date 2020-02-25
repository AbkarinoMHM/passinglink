#include "output/usb/usb.h"

#include <stdint.h>

#include <zephyr.h>

#include <init.h>
#include <logging/log.h>

#include <usb/class/usb_hid.h>
#include <usb/usb_device.h>

#include "arch.h"
#include "input/input.h"
#include "output/output.h"
#include "output/usb/hid.h"
#include "output/usb/nx/hid.h"
#include "output/usb/ps4/hid.h"

#define LOG_LEVEL LOG_LEVEL_DBG
LOG_MODULE_REGISTER(usb);

#if defined(CONFIG_PASSINGLINK_OUTPUT_USB_SWITCH)
static nx::Hid nx_hid;
#endif

#if defined(CONFIG_PASSINGLINK_OUTPUT_USB_PS4)
static ps4::Hid ps4_hid;
#endif

enum class ProbeType : uint64_t {
  NX = 0xAAAAAAAAAAAAAAAA,
  PS4 = 0x5555555555555555,
};

static optional<ProbeType> ProbeTypeFirst() {
#if defined(CONFIG_PASSINGLINK_OUTPUT_USB_SWITCH)
  return ProbeType::NX;
#elif defined(CONFIG_PASSINGLINK_OUTPUT_USB_PS4)
  return ProbeType::PS4;
#else
  return {};
#endif
}

optional<ProbeType> ProbeTypeNext(ProbeType probe_type) {
  switch (probe_type) {
    case ProbeType::NX:
#if defined(CONFIG_PASSINGLINK_OUTPUT_USB_PS4)
      return ProbeType::PS4;
#else
      return {};
#endif

    default:
      return {};
  }
}

bool ProbeTypeIsValid(ProbeType probe_type) {
  return probe_type == ProbeType::NX || probe_type == ProbeType::PS4;
}

Hid* ProbeTypeHid(ProbeType probe_type) {
#if defined(CONFIG_PASSINGLINK_OUTPUT_USB_SWITCH)
  if (probe_type == ProbeType::NX) {
    return &nx_hid;
  }
#endif

#if defined(CONFIG_PASSINGLINK_OUTPUT_USB_PS4)
  if (probe_type == ProbeType::PS4) {
    return &ps4_hid;
  }
#endif

  return nullptr;
}

#if defined(CONFIG_USB_DC_STM32)
static ProbeType boot_probe_type __attribute__((section(".noinit")));

optional<ProbeType> get_boot_probe() {
  if (ProbeTypeIsValid(boot_probe_type)) {
    return boot_probe_type;
  }
  return {};
}

void set_boot_probe(optional<ProbeType> probe) {
  if (probe) {
    boot_probe_type = *probe;
  } else {
    memset(&boot_probe_type, 0, sizeof(boot_probe_type));
  }
}

#endif

#define USB_OUTPUT_COUNT CONFIG_PASSINGLINK_OUTPUT_USB_SWITCH + CONFIG_PASSINGLINK_OUTPUT_USB_PS4

#if USB_OUTPUT_COUNT > 1
static optional<ProbeType> current_probe;

#if !defined(CONFIG_USB_DC_STM32)
static k_delayed_work probe_start_work;
#endif

static k_delayed_work probe_check_work;
static void usb_probe_start(k_work*);
static void usb_probe_check(k_work*);

static int usb_probe() {
#if !defined(CONFIG_USB_DC_STM32)
  k_delayed_work_init(&probe_start_work, usb_probe_start);
#endif
  k_delayed_work_init(&probe_check_work, usb_probe_check);

  optional<ProbeType> probe;
#if defined(CONFIG_USB_DC_STM32)
  probe = get_boot_probe();
  if (!probe) {
    LOG_INF("no boot probe found, starting usb probe");
  } else {
    LOG_INF("boot probe found: %s", ProbeTypeHid(*probe)->Name());
  }
#endif

  // Check to see if the user is overriding detection after we check if we already probed, so
  // inadvertent button presses don't override probing.
  if (!probe) {
    RawInputState input;
    if (!input_get_raw_state(&input)) {
      LOG_ERR("failed to get input state");
    } else {
#if defined(CONFIG_PASSINGLINK_OUTPUT_USB_SWITCH)
      if (input.button_west) {
        LOG_WRN("Switch mode selected via button");
        return passinglink::usb_hid_init(&nx_hid);
      }
#endif

#if defined(CONFIG_PASSINGLINK_OUTPUT_USB_PS4)
      if (input.button_north) {
        LOG_WRN("PS4 mode selected via button");
        return passinglink::usb_hid_init(&ps4_hid);
      }
#endif
    }
  }

  current_probe = probe;
  usb_probe_start(nullptr);
  return 0;
}

static void usb_probe_start(k_work*) {
  if (!current_probe) {
    current_probe = ProbeTypeFirst();
    if (!current_probe) {
      LOG_ERR("no available USB HID implementations");
      return;
    }
  }

  Hid* current_hid = ProbeTypeHid(*current_probe);
  passinglink::usb_hid_init(current_hid);

  u32_t delay = current_hid->ProbeDelay();
  k_delayed_work_submit_ticks(&probe_check_work, delay);
}

static void usb_probe_check(k_work*) {
  Hid* current_hid = ProbeTypeHid(*current_probe);
  LOG_INF("checking whether %s was successful", current_hid->Name());
  if (current_hid->ProbeResult()) {
    LOG_INF("%s Hid reports success", current_hid->Name());
    return;
  }

  optional<ProbeType> next_probe = ProbeTypeNext(*current_probe);
  if (!next_probe) {
    LOG_ERR("%s Hid reports failure, but no more Hids are available", current_hid->Name());
    return;
  }

  LOG_ERR("%s Hid reports failure, continuing", current_hid->Name());

#if defined(CONFIG_USB_DC_STM32)
  // usb_disable isn't implemented for STM32, so we need to stash our result and reboot.
  set_boot_probe(next_probe);
  reboot();
#else
  passinglink::usb_hid_uninit();
  current_probe = next_probe;
  usb_probe_start(nullptr);
#endif
}
#endif  // USB_OUTPUT_COUNT > 1

namespace passinglink {

int usb_init() {
#if USB_OUTPUT_COUNT > 1
  return usb_probe();
#elif defined(CONFIG_PASSINGLINK_OUTPUT_USB_PS4)
  return passinglink::usb_hid_init(&ps4_hid);
#elif defined(CONFIG_PASSINGLINK_OUTPUT_USB_SWITCH)
  return passinglink::usb_hid_init(&nx_hid);
#endif

  LOG_ERR("no USB HID output available");
  return -1;
}

}  // namespace passinglink
