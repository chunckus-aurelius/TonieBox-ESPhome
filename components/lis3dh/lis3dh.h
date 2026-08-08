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
};

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
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_range(Lis3dhRange range) { this->range_ = range; }

  void set_x_sensor(sensor::Sensor *x_sensor) { this->x_sensor_ = x_sensor; }
  void set_y_sensor(sensor::Sensor *y_sensor) { this->y_sensor_ = y_sensor; }
  void set_z_sensor(sensor::Sensor *z_sensor) { this->z_sensor_ = z_sensor; }

 protected:
  float counts_to_g_(int16_t counts);
  uint8_t range_g_() const;

  Lis3dhRange range_{RANGE_2G};
  bool initialized_{false};

  sensor::Sensor *x_sensor_{nullptr};
  sensor::Sensor *y_sensor_{nullptr};
  sensor::Sensor *z_sensor_{nullptr};
};

}  // namespace lis3dh
}  // namespace esphome
