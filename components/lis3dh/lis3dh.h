#pragma once

// ST LIS3DH 3-axis accelerometer, I2C control.
//
// Register map and init sequence ported from
// teddybox/components/lis3dh/{include/lis3dh_regs.h,src/lis3dh.c}, which
// targets this exact board revision (toniebox_esp32_v1.6.C). board_def.h
// defines LIS3DH_IRQ_GPIO and has no MMA8451 pin, and the teddybox project
// ships only a lis3dh component -- so this is the part populated here.
//
// Some board revisions populate an MMA8451 instead. If yours does, this
// component will not find its WHO_AM_I and will fail setup. Only one
// accelerometer should be instantiated per board.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace lis3dh {

enum Lis3dhRegister : uint8_t {
  REG_WHO_AM_I = 0x0F,
  REG_CTRL_REG1 = 0x20,
  REG_CTRL_REG2 = 0x21,
  REG_CTRL_REG3 = 0x22,
  REG_CTRL_REG4 = 0x23,
  REG_CTRL_REG5 = 0x24,
  REG_CTRL_REG6 = 0x25,
  REG_OUT_X_L = 0x28,
  REG_INT1_CFG = 0x30,
  REG_INT1_THS = 0x32,
  REG_INT1_DURATION = 0x33,
  REG_CLICK_CFG = 0x38,
  REG_CLICK_SRC = 0x39,
  REG_CLICK_THS = 0x3A,
  REG_TIME_LIMIT = 0x3B,
  REG_TIME_LATENCY = 0x3C,
  REG_TIME_WINDOW = 0x3D,
};

// Click detection. teddybox defines these registers and the two CLICK_CFG
// patterns below but only ever writes 0x00 to them, so there is nothing to
// port from it. The reference for this path is instead Adafruit_LIS3DH's
// setClick(), which is known to work; where the ST app note and that library
// disagree, the library wins.
//
// CLICK_CFG enables per-axis single (xS/yS/zS) or double (xD/yD/zD) detection.
// 0x15 = 0b00010101 = single on Z,Y,X. 0x2A = 0b00101010 = double on Z,Y,X.
// Both names are teddybox's (LIS3DH_SINGLE_CLICK / LIS3DH_DOUBLE_CLICK).
static const uint8_t LIS3DH_CLICK_CFG_SINGLE = 0x15;
static const uint8_t LIS3DH_CLICK_CFG_DOUBLE = 0x2A;

// CLICK_THS bit 7 (LIR_Click). NOT set -- the reference implementation leaves
// it clear and polls CLICK_SRC successfully, and latching is configured via
// CTRL_REG5 instead. Kept documented so it is not "rediscovered" as a fix.
static const uint8_t LIS3DH_CLICK_THS_LIR = 0x80;

// CLICK_SRC bits. IA is the "an event happened" flag; SIGN distinguishes a
// tap on one side of an axis from the other, which is what makes left/right
// separable for seek direction.
static const uint8_t LIS3DH_CLICK_SRC_X = 0x01;
static const uint8_t LIS3DH_CLICK_SRC_Y = 0x02;
static const uint8_t LIS3DH_CLICK_SRC_Z = 0x04;
static const uint8_t LIS3DH_CLICK_SRC_SIGN = 0x08;
static const uint8_t LIS3DH_CLICK_SRC_SCLICK = 0x10;
static const uint8_t LIS3DH_CLICK_SRC_DCLICK = 0x20;
static const uint8_t LIS3DH_CLICK_SRC_IA = 0x40;

// CTRL_REG2 bit 2 runs the click detector off the high-pass filter. The ST app
// note recommends it, to stop the 1G gravity bias counting toward the
// threshold on whichever axis points down. Adafruit's working implementation
// does NOT use it. Off by default for that reason -- matching something known
// to work beats matching something recommended -- but exposed so it can be
// tried if the down-axis turns out to need a harder tap.
static const uint8_t LIS3DH_CTRL_REG2_HPCLICK = 0x04;

// Routing and latching, both written by the reference implementation. Not
// strictly needed when polling CLICK_SRC rather than watching INT1, but this
// driver has been bitten before by copying a reference's conditions without
// its control flow -- so write them as the reference does.
static const uint8_t LIS3DH_CTRL_REG3_I1_CLICK = 0x80;
static const uint8_t LIS3DH_CTRL_REG5_LIR_INT1 = 0x08;

// How often loop() reads CLICK_SRC. Independent of update_interval on purpose.
static const uint32_t LIS3DH_CLICK_POLL_MS = 50;

// Reading more than one byte requires the auto-increment bit (0x80) OR'd
// into the sub-address; without it the device returns the same register
// repeatedly. teddybox spells this LIS3DH_OUT_X_L_INCR (0x28 | 0x80).
static const uint8_t LIS3DH_AUTO_INCREMENT = 0x80;

// Fixed WHO_AM_I value for the LIS3DH per ST datasheet.
static const uint8_t LIS3DH_WHO_AM_I_VALUE = 0x33;

// CTRL_REG1 = 0x07 enables X/Y/Z axes; the data-rate nibble is OR'd in on
// top of it (teddybox lis3dh_init writes 0x07, then sets the rate).
static const uint8_t LIS3DH_CTRL_REG1_AXES_ENABLE = 0x07;
// 100Hz in the ODR nibble (CTRL_REG1 bits 7:4) -- comfortably above the
// component's default 100ms poll so each update() sees fresh data.
static const uint8_t LIS3DH_ODR_100HZ = 0x50;
// 400Hz whenever click detection is on. A tap is a a few-millisecond transient;
// at 100Hz (one sample per 10ms) it can fall entirely between samples, which is
// the rate every working LIS3DH tap implementation avoids. Adafruit's library
// -- the reference for the click path, since teddybox has none -- runs 400Hz.
static const uint8_t LIS3DH_ODR_400HZ = 0x70;

// CTRL_REG4 bits 5:4 select full scale. Values match teddybox
// lis3dh_get_range()'s decode of (CTRL_REG4 & 0x30) >> 4.
enum Lis3dhRange : uint8_t {
  RANGE_2G = 0x00,
  RANGE_4G = 0x01,
  RANGE_8G = 0x02,
  RANGE_16G = 0x03,
};

class LIS3DHComponent final : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_range(Lis3dhRange range) { this->range_ = range; }

  void set_x_sensor(sensor::Sensor *x_sensor) { this->x_sensor_ = x_sensor; }
  void set_y_sensor(sensor::Sensor *y_sensor) { this->y_sensor_ = y_sensor; }
  void set_z_sensor(sensor::Sensor *z_sensor) { this->z_sensor_ = z_sensor; }

  void set_click_enabled(bool enabled) { this->click_enabled_ = enabled; }
  void set_click_double(bool dbl) { this->click_double_ = dbl; }
  void set_click_hpf(bool hpf) { this->click_hpf_ = hpf; }

  // Live setters: each writes its register immediately when the part is up,
  // so these can be driven from Home Assistant number entities. Tuning a tap
  // threshold by eye is a dozen guesses -- doing that as a dozen reflashes is
  // how this project has lost days before. Find the values on the bench,
  // then bake them into YAML as the new defaults.
  void set_click_threshold(uint8_t ths);
  void set_click_time_limit(uint8_t limit);
  void set_click_time_latency(uint8_t latency);
  void set_click_time_window(uint8_t window);

  void add_on_click_callback(std::function<void(uint8_t)> &&cb) { this->click_callback_.add(std::move(cb)); }

 protected:
  float counts_to_g_(int16_t counts);
  uint8_t range_g_() const;
  void write_click_config_();
  void poll_click_();

  Lis3dhRange range_{RANGE_2G};
  bool initialized_{false};

  bool click_enabled_{false};
  bool click_double_{false};
  bool click_hpf_{false};
  uint8_t click_threshold_{40};
  uint8_t click_time_limit_{10};
  uint8_t click_time_latency_{20};
  uint8_t click_time_window_{100};
  uint32_t last_click_poll_{0};
  CallbackManager<void(uint8_t)> click_callback_;

  sensor::Sensor *x_sensor_{nullptr};
  sensor::Sensor *y_sensor_{nullptr};
  sensor::Sensor *z_sensor_{nullptr};
};

// Fires with the raw CLICK_SRC byte rather than a decoded axis, so a config
// can tell apart which axis was struck and on which side (the SIGN bit) --
// that is what separates "tap the left" from "tap the right". Decode with the
// LIS3DH_CLICK_SRC_* masks above.
class LIS3DHClickTrigger : public Trigger<uint8_t> {
 public:
  explicit LIS3DHClickTrigger(LIS3DHComponent *parent) {
    parent->add_on_click_callback([this](uint8_t src) { this->trigger(src); });
  }
};

}  // namespace lis3dh
}  // namespace esphome
