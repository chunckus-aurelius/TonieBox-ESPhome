#include "lis3dh.h"
#include "esphome/core/log.h"

namespace esphome {
namespace lis3dh {

static const char *const TAG = "lis3dh";

void LIS3DHComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LIS3DH...");

  uint8_t who_am_i = 0;
  if (!this->read_byte(REG_WHO_AM_I, &who_am_i) || who_am_i != LIS3DH_WHO_AM_I_VALUE) {
    ESP_LOGE(TAG, "WHO_AM_I mismatch (got 0x%02X, expected 0x%02X) -- is this actually a LIS3DH?", who_am_i,
             LIS3DH_WHO_AM_I_VALUE);
    this->mark_failed();
    return;
  }

  // Init order follows teddybox lis3dh_init(): enable axes at a known data
  // rate first, then clear the remaining control/interrupt registers so the
  // part comes up in a defined state regardless of what the stock firmware
  // (or a warm reset) left behind.
  this->write_byte(REG_CTRL_REG1, LIS3DH_ODR_100HZ | LIS3DH_CTRL_REG1_AXES_ENABLE);
  this->write_byte(REG_CTRL_REG2, 0x00);
  this->write_byte(REG_CTRL_REG3, 0x00);
  // CTRL_REG4: full-scale in bits 5:4. BDU is deliberately left clear to
  // match teddybox, which writes 0x00 here for the 2g default.
  this->write_byte(REG_CTRL_REG4, static_cast<uint8_t>(this->range_ << 4));
  this->write_byte(REG_CTRL_REG5, 0x00);
  this->write_byte(REG_CTRL_REG6, 0x00);
  this->write_byte(REG_INT1_CFG, 0x00);
  this->write_byte(REG_INT1_THS, 0x00);
  this->write_byte(REG_INT1_DURATION, 0x00);

  this->initialized_ = true;
  this->write_click_config_();
}

// INT1 (GPIO14) is deliberately NOT used. CLICK_THS bit 7 latches CLICK_SRC
// until it is read, so polling at the component's update interval catches
// every event -- the latch is what makes the pin redundant. That keeps the
// I2C read out of an ISR context, where it could not run anyway, and leaves
// GPIO14 free. The cost is up to one update interval of latency, which at
// 100ms is imperceptible for a skip gesture.
void LIS3DHComponent::write_click_config_() {
  if (!this->initialized_)
    return;

  if (!this->click_enabled_) {
    // Match teddybox lis3dh_init(), which clears all five click registers so
    // the part comes up defined regardless of what stock firmware left behind.
    this->write_byte(REG_CLICK_CFG, 0x00);
    this->write_byte(REG_CLICK_THS, 0x00);
    this->write_byte(REG_TIME_LIMIT, 0x00);
    this->write_byte(REG_TIME_LATENCY, 0x00);
    this->write_byte(REG_TIME_WINDOW, 0x00);
    return;
  }

  // High-pass the click detector first, or gravity biases whichever axis is
  // pointing down and that side needs a far harder tap than the others.
  this->write_byte(REG_CTRL_REG2, LIS3DH_CTRL_REG2_HPCLICK);
  this->write_byte(REG_CLICK_CFG, this->click_double_ ? LIS3DH_CLICK_CFG_DOUBLE : LIS3DH_CLICK_CFG_SINGLE);
  this->write_byte(REG_CLICK_THS, static_cast<uint8_t>((this->click_threshold_ & 0x7F) | LIS3DH_CLICK_THS_LIR));
  this->write_byte(REG_TIME_LIMIT, this->click_time_limit_);
  this->write_byte(REG_TIME_LATENCY, this->click_time_latency_);
  this->write_byte(REG_TIME_WINDOW, this->click_time_window_);
}

void LIS3DHComponent::set_click_threshold(uint8_t ths) {
  this->click_threshold_ = ths;
  this->write_click_config_();
}
void LIS3DHComponent::set_click_time_limit(uint8_t limit) {
  this->click_time_limit_ = limit;
  this->write_click_config_();
}
void LIS3DHComponent::set_click_time_latency(uint8_t latency) {
  this->click_time_latency_ = latency;
  this->write_click_config_();
}
void LIS3DHComponent::set_click_time_window(uint8_t window) {
  this->click_time_window_ = window;
  this->write_click_config_();
}

// Reading CLICK_SRC is what clears the latch, so this must run every cycle
// once click is enabled -- skipping it would wedge the detector on the first
// tap and nothing would fire again.
void LIS3DHComponent::poll_click_() {
  uint8_t src = 0;
  if (!this->read_byte(REG_CLICK_SRC, &src))
    return;
  if ((src & LIS3DH_CLICK_SRC_IA) == 0)
    return;

  // The SCLICK bit is set for the first tap of a double too, so when we asked
  // for doubles a single-only event is a partial gesture and not ours.
  const bool want = this->click_double_ ? (src & LIS3DH_CLICK_SRC_DCLICK) : (src & LIS3DH_CLICK_SRC_SCLICK);
  if (!want)
    return;

  ESP_LOGD(TAG, "Click: CLICK_SRC=0x%02X (%s%s%s%s)", src, (src & LIS3DH_CLICK_SRC_X) ? "X" : "",
           (src & LIS3DH_CLICK_SRC_Y) ? "Y" : "", (src & LIS3DH_CLICK_SRC_Z) ? "Z" : "",
           (src & LIS3DH_CLICK_SRC_SIGN) ? " neg" : " pos");
  this->click_callback_.call(src);
}

uint8_t LIS3DHComponent::range_g_() const {
  switch (this->range_) {
    case RANGE_4G:
      return 4;
    case RANGE_8G:
      return 8;
    case RANGE_16G:
      return 16;
    case RANGE_2G:
    default:
      return 2;
  }
}

float LIS3DHComponent::counts_to_g_(int16_t counts) {
  // Output is left-justified in the 16-bit word (12-bit in normal mode), so
  // scale from full-scale deflection rather than shifting to a raw bit
  // width. teddybox lis3dh_fetch() uses the same (val / 32000.0) * range
  // form; 32000 rather than 32768 is that driver's convention and keeps
  // this port numerically consistent with the reference implementation.
  return (counts / 32000.0f) * this->range_g_();
}

void LIS3DHComponent::update() {
  if (!this->initialized_)
    return;

  // Six SEPARATE single-byte reads, not one 6-byte burst.
  //
  // This mirrors the reference driver exactly. teddybox lis3dh_fetch()
  // (components/lis3dh/src/lis3dh.c:36-39) loops LIS3DH_OUT_X_L_INCR + pos
  // and reads ONE byte per transaction via lis3dh_get_reg(), walking the
  // register address itself rather than relying on the device to
  // auto-increment across a multi-byte read. The auto-increment bit stays
  // set on each address because that is what the reference does
  // (LIS3DH_OUT_X_L_INCR == 0x28 | 0x80).
  //
  // HISTORICAL NOTE -- READ BEFORE "FIXING" THIS BACK TO A BURST:
  // An early revision of this comment claimed the burst form was NACKed by
  // the device and concluded the read LENGTH was the trigger. That was
  // wrong, and it sent the 2026-08-01/02 debugging down a dead end. The real
  // faults were elsewhere entirely (async `- delay:` in on_boot leaving the
  // DAC3100 in reset during bus init, then an output: platform's setup()
  // cutting the GPIO45 power rail right after the scan -- see the comments
  // in tonie-the-assistant.yaml).
  //
  // Log 15 (2026-08-03) settled the read-shape question directly, with a
  // probe reading three sub-addresses at equal length each cycle:
  //     PROBE 0x0F=OK(0x33) 0x28=OK(0xC0) 0xA8=OK(0xC0)
  // The device answers on the plain output register (0x28) and on the
  // auto-increment form (0xA8) equally well. There is no device quirk here.
  //
  // So this loop is NOT a workaround -- it is kept purely because it mirrors
  // the reference driver. A 6-byte burst read would very likely work too;
  // it simply has not been tested, and there is no reason to diverge from
  // teddybox's proven form. Verified working: X/Y/Z track gravity correctly
  // (~-0.97G on one axis at rest).
  uint8_t data[6];
  for (uint8_t pos = 0; pos < 6; pos++) {
    if (!this->read_byte(static_cast<uint8_t>((REG_OUT_X_L | LIS3DH_AUTO_INCREMENT) + pos), &data[pos])) {
      ESP_LOGW(TAG, "Read failed at offset %u", pos);
      this->status_set_warning();
      return;
    }
  }
  this->status_clear_warning();

  // LIS3DH is little-endian: low byte first, unlike the big-endian MMA8451.
  int16_t raw_x = static_cast<int16_t>(data[0] | (data[1] << 8));
  int16_t raw_y = static_cast<int16_t>(data[2] | (data[3] << 8));
  int16_t raw_z = static_cast<int16_t>(data[4] | (data[5] << 8));

  if (this->x_sensor_ != nullptr)
    this->x_sensor_->publish_state(this->counts_to_g_(raw_x));
  if (this->y_sensor_ != nullptr)
    this->y_sensor_->publish_state(this->counts_to_g_(raw_y));
  if (this->z_sensor_ != nullptr)
    this->z_sensor_->publish_state(this->counts_to_g_(raw_z));
}

// Click polling lives here rather than in update() because the two want very
// different rates: the configs set update_interval: 1s to keep accelerometer
// publishes down, and a tap answered up to a second later feels broken. The
// latch means nothing is lost at any rate -- this is purely about latency.
void LIS3DHComponent::loop() {
  if (!this->initialized_ || !this->click_enabled_)
    return;
  const uint32_t now = millis();
  if (now - this->last_click_poll_ < LIS3DH_CLICK_POLL_MS)
    return;
  this->last_click_poll_ = now;
  this->poll_click_();
}

void LIS3DHComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "LIS3DH:");
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Communication with LIS3DH failed!");
  }
  ESP_LOGCONFIG(TAG, "  Range: +/-%dg", this->range_g_());
  if (this->click_enabled_) {
    ESP_LOGCONFIG(TAG, "  Click: %s, threshold %u, limit %u, latency %u, window %u",
                  this->click_double_ ? "double" : "single", this->click_threshold_, this->click_time_limit_,
                  this->click_time_latency_, this->click_time_window_);
  }
  LOG_SENSOR("  ", "X", this->x_sensor_);
  LOG_SENSOR("  ", "Y", this->y_sensor_);
  LOG_SENSOR("  ", "Z", this->z_sensor_);
}

}  // namespace lis3dh
}  // namespace esphome
