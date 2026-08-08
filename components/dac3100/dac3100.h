#pragma once

// TI TLV320DAC3100 ("DAC3100") I2S audio codec, I2C control.
//
// Register sequence ported (with attribution) from the MIT-licensed
// Espressif ESP-ADF reference driver in
// toniebox-reverse-engineering/teddybox (components/toniebox/dac3100),
// which itself credits
// https://github.com/toniebox-reverse-engineering/RvX_TLV320DAC3100
// for the PLL/NDAC/MDAC clock values used here.

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace dac3100 {

// TLV320DAC3100 Page 0 register addresses we touch.
enum Dac3100Register : uint8_t {
  REG_PAGE_CONTROL = 0x00,
  REG_SOFTWARE_RESET = 0x01,
  REG_CLOCKGEN_MUX = 0x04,
  REG_PLL_P_R_VAL = 0x05,
  REG_PLL_J_VAL = 0x06,
  REG_PLL_D_VAL_MSB = 0x07,
  REG_PLL_D_VAL_LSB = 0x08,
  REG_DAC_NDAC_VAL = 0x0B,
  REG_DAC_MDAC_VAL = 0x0C,
  REG_DAC_DOSR_VAL_MSB = 0x0D,
  REG_DAC_DOSR_VAL_LSB = 0x0E,
  REG_CODEC_IF_CTRL1 = 0x1B,
  REG_DAC_PROC_BLOCK_SEL = 0x3C,
  // 0x3F powers the DAC channels on and picks the L/R data path; 0x40 is the
  // DAC's own mute/volume-control register. Both were MISSING from the
  // original port, which is why the codec ACKed on I2C but produced silence.
  // Names and addresses match teddybox dac3100.h:71-72
  // (DAC_DATA_PATH_SETUP / DAC_VOL_CTRL).
  REG_DAC_DATA_PATH_SETUP = 0x3F,
  REG_DAC_VOL_CTRL = 0x40,
  REG_DAC_VOL_L_CTRL = 0x41,
  REG_DAC_VOL_R_CTRL = 0x42,
  REG_HEADSET_DETECT = 0x43,
  REG_BEEP_R_GEN = 0x48,
  REG_INT1_CTRL_REG = 0x30,
  REG_GPIO1_INOUT_CTRL = 0x33,
};

// Page 1 register addresses (analog output routing / drivers).
enum Dac3100Page1Register : uint8_t {
  REG_HP_DRIVERS = 0x1F,
  REG_SPK_AMP = 0x20,
  REG_HP_OUT_POP_REM_SET = 0x21,
  REG_OUT_PGA_RAMP_DOWN_PER_CTRL = 0x22,
  REG_DAC_LR_OUT_MIX_ROUTING = 0x23,
  REG_L_VOL_TO_HPL = 0x24,
  REG_R_VOL_TO_HPR = 0x25,
  REG_L_VOL_TO_SPK = 0x26,
  REG_HPL_DRIVER = 0x28,
  REG_HPR_DRIVER = 0x29,
  REG_SPK_DRIVER = 0x2A,
  REG_HP_DRIVER_CTRL = 0x2C,
  REG_MICBIAS = 0x2E,
};

enum Dac3100Page3Register : uint8_t {
  REG_TIMER_CLK_MCLK_DIV = 0x10,
};

class DAC3100Component final : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // volume in percent, 0-100
  void set_volume(uint8_t volume);
  void set_mute(bool mute);

 protected:
  void select_page_(uint8_t page);
  void write_reg_(uint8_t reg, uint8_t value);
  // Reads the registers that decide whether the codec can make a sound and
  // caches them for dump_config(). Called at the end of setup() because
  // dump_config() must not touch hardware.
  void read_back_registers_();

  uint8_t current_page_{0xFF};  // force a page-select write on first access
  bool muted_{false};
  uint8_t volume_{100};

  // Cached in setup() by read_back_registers_(), printed by dump_config().
  bool readback_ok_{false};
  bool analog_readback_ok_{false};
  uint8_t rb_data_path_{0};
  uint8_t rb_vol_ctrl_{0};
  uint8_t rb_spk_vol_{0};
  uint8_t rb_spk_drv_{0};
  uint8_t rb_spk_amp_{0};
};

}  // namespace dac3100
}  // namespace esphome
