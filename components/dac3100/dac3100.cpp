#include "dac3100.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace dac3100 {

static const char *const TAG = "dac3100";

void DAC3100Component::select_page_(uint8_t page) {
  if (page == this->current_page_) return;
  this->write_byte(REG_PAGE_CONTROL, page);
  this->current_page_ = page;
}

void DAC3100Component::write_reg_(uint8_t reg, uint8_t value) { this->write_byte(reg, value); }

void DAC3100Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up DAC3100...");

  // Page 0: clocking, PLL, I2S interface, DAC signal path.
  this->select_page_(0x00);
  this->write_reg_(REG_SOFTWARE_RESET, 0x01);
  this->write_reg_(REG_CLOCKGEN_MUX, 0x07);     // PLL_CLKIN=BCLK, CODEC_CLKIN=PLL_CLK
  this->write_reg_(REG_PLL_J_VAL, 0x20);        // PLL multiplier J=32
  this->write_reg_(REG_PLL_D_VAL_MSB, 0x00);
  this->write_reg_(REG_PLL_D_VAL_LSB, 0x00);    // fractional D-value = 0
  this->write_reg_(REG_PLL_P_R_VAL, 0x96);      // PLL powered up, P=1, R=6
  this->write_reg_(REG_DAC_NDAC_VAL, 0x84);     // NDAC powered up, divider=4
  this->write_reg_(REG_DAC_MDAC_VAL, 0x86);     // MDAC powered up, divider=6
  this->write_reg_(REG_DAC_DOSR_VAL_MSB, 0x01); // DAC OSR = 256
  this->write_reg_(REG_DAC_DOSR_VAL_LSB, 0x00);

  delay(10);

  this->write_reg_(REG_CODEC_IF_CTRL1, 0x00);      // I2S, 16-bit, BCLK/WCLK as configured by I2S bus
  this->write_reg_(REG_DAC_PROC_BLOCK_SEL, 0x19);  // DAC signal-processing block PRB_P25

  // Page 1: analog output routing and drivers.
  this->select_page_(0x01);
  this->write_reg_(REG_HP_DRIVERS, 0x04);
  this->write_reg_(REG_HPL_DRIVER, 0x06);
  this->write_reg_(REG_HPR_DRIVER, 0x06);
  // MUTED here on purpose (0x18 = bit2 clear); unmuted at the end of setup().
  // 0x1C at this point enables the class-D driver before the DAC channels are
  // powered up at REG_DAC_DATA_PATH_SETUP below, so the DAC coming online
  // lands as a step into an already-live amp -- an audible pop on every
  // power-up, and on every deep sleep wake since both run this same path.
  this->write_reg_(REG_SPK_DRIVER, 0x18);
  this->write_reg_(REG_HP_DRIVERS, 0xC4);
  this->write_reg_(REG_SPK_AMP, 0x86);
  this->write_reg_(REG_L_VOL_TO_HPL, 0x92);
  this->write_reg_(REG_R_VOL_TO_HPR, 0x92);
  // L_VOL_TO_SPK is an ANALOG ATTENUATOR: 0x00 = routed at 0dB (full
  // volume), and larger values attenuate (0x7F = -inf, used as a mute).
  // The original port copied 0x92 from an early block of teddybox
  // dac3100.c (line 232), but that value is superseded later in the SAME
  // init function: dac3100.c:300 rewrites it to 0x00, flagged there with
  // the author's own "!!! FEHLTE !!!" ("was missing") comment, immediately
  // followed by SPK_AMP 0x86 to power the amp up. 0x92 left the speaker
  // path heavily attenuated, which is why the codec configured cleanly and
  // reported healthy while producing NO audible output. Match the
  // reference's FINAL value. CONFIRMED on hardware 2026-08-03: with 0x00
  // the rtttl test tone is audible; with 0x92 it was not.
  this->write_reg_(REG_L_VOL_TO_SPK, 0x00);
  this->write_reg_(REG_HP_OUT_POP_REM_SET, 0x4E);
  this->write_reg_(REG_OUT_PGA_RAMP_DOWN_PER_CTRL, 0x70);
  this->write_reg_(REG_DAC_LR_OUT_MIX_ROUTING, 0x44);  // DAC_L->MixAmp_L, DAC_R->MixAmp_R
  this->write_reg_(REG_MICBIAS, 0x0B);
  this->write_reg_(REG_HP_DRIVER_CTRL, 0xE0);

  // Page 3: MCLK divider for the internal delay timer.
  this->select_page_(0x03);
  this->write_reg_(REG_TIMER_CLK_MCLK_DIV, 0x01);

  // Back to page 0 for headset-detect/IRQ config.
  this->select_page_(0x00);
  this->write_reg_(REG_HEADSET_DETECT, 0x8C);
  this->write_reg_(REG_INT1_CTRL_REG, 0x80);
  this->write_reg_(REG_GPIO1_INOUT_CTRL, 0x14);

  // POWER THE DAC CHANNELS ON AND UNMUTE (added 2026-08-03).
  //
  // These two writes were MISSING from the original port. Without them the
  // codec configures cleanly and ACKs every I2C transaction -- so nothing
  // looks wrong at the bus level -- but the DAC channels are never powered
  // up and the output stays silent no matter what the I2S pipeline feeds it.
  //
  // Matches teddybox dac3100.c:308-313, which does exactly this at the end
  // of its init, after the analog routing on page 1:
  //     dac3100_write_reg(DAC_DATA_PATH_SETUP, 0xD5);
  //     dac3100_write_reg(DAC_VOL_L_CTRL, 0xDC);
  //     dac3100_write_reg(DAC_VOL_R_CTRL, 0xDC);
  //     dac3100_write_reg(DAC_VOL_CTRL, 0x00);
  // 0xD5 = both DAC channels powered up, left data -> left channel, right
  // data -> right channel, soft-stepping enabled.
  // 0x00 in DAC_VOL_CTRL = neither channel muted.
  //
  // The per-channel volume registers (0x41/0x42) are deliberately NOT
  // hardcoded to 0xDC here -- set_volume() below drives them from the
  // configured volume instead, which is the same registers by a different
  // route. The reference then calls dac3100_set_mute(true) as its last act;
  // we do NOT replicate that, since an ESPHome media_player expects the
  // codec to be ready to play without an explicit unmute first.
  this->write_reg_(REG_DAC_DATA_PATH_SETUP, 0xD5);
  this->write_reg_(REG_DAC_VOL_CTRL, 0x00);

  this->set_volume(this->volume_);

  // UNMUTE THE SPEAKER DRIVER LAST -- this is the anti-pop step and the order
  // is the whole point. Everything above has settled: the DAC channels are
  // powered, unmuted and at their configured volume, and DATA_PATH_SETUP 0xD5
  // enables soft-stepping so the output ramps rather than jumping. The delay
  // lets that ramp finish before the driver starts passing signal.
  //
  // Must stay ABOVE read_back_registers_(), or dump_config() caches the muted
  // 0x18 and reports a mute that is not real. Do not move it back up next to
  // the other page 1 writes -- that is what made every board pop.
  delay(50);
  this->select_page_(0x01);
  this->write_reg_(REG_SPK_DRIVER, 0x1C);
  this->select_page_(0x00);

  this->read_back_registers_();
}

void DAC3100Component::set_mute(bool mute) {
  this->muted_ = mute;

  // Mute at the DAC itself (page 0, DAC_VOL_CTRL bits 3:2), which is what
  // teddybox dac3100_set_mute() drives. 0x0C mutes both channels, 0x00
  // unmutes. This is the register that actually gates the digital path.
  this->select_page_(0x00);
  this->write_reg_(REG_DAC_VOL_CTRL, mute ? 0x0C : 0x00);

  // Also gate the class-D speaker driver on page 1. SPK_DRIVER bit2 mirrors
  // upstream's mute bit. Note this is a blind write rather than a
  // read-modify-write -- we don't cache page 1 state -- so it clobbers any
  // independent gain setting in this register. Acceptable while gain is
  // fixed; revisit if independent gain-stage control is needed later.
  this->select_page_(0x01);
  this->write_reg_(REG_SPK_DRIVER, mute ? 0x18 : 0x1C);
}

void DAC3100Component::set_volume(uint8_t volume) {
  if (volume > 100) volume = 100;
  this->volume_ = volume;

  // Upstream formula: maps 0-100% onto the DAC's -63.5dB..0dB volume register range.
  int value = -635 + (static_cast<int>(volume) * 635 / 100);
  uint8_t reg_val = static_cast<uint8_t>(value / 5);
  uint8_t beep_val = static_cast<uint8_t>(((100 - volume) * 0x3F) / 100) & 0x3F;

  this->select_page_(0x00);
  this->write_reg_(REG_DAC_VOL_L_CTRL, reg_val);
  this->write_reg_(REG_DAC_VOL_R_CTRL, reg_val);
  this->write_reg_(REG_BEEP_R_GEN, 0x40 | beep_val);
}

void DAC3100Component::read_back_registers_() {
  // setup() writes blind -- write_byte()'s return value is never checked, so
  // a codec that is absent or unpowered produces an identical-looking boot.
  // That is exactly how the missing DAC_DATA_PATH_SETUP bug stayed invisible.
  // Read the registers back here so a broken codec is visible in the log.
  //
  // This runs in setup(), not dump_config(): dump_config() is only allowed to
  // print values already determined during setup, and must not do I/O.
  this->select_page_(0x00);
  this->readback_ok_ = this->read_byte(REG_DAC_DATA_PATH_SETUP, &this->rb_data_path_);
  if (!this->readback_ok_) {
    return;
  }
  this->read_byte(REG_DAC_VOL_CTRL, &this->rb_vol_ctrl_);

  // Read back the analog output stage too. A codec can be perfectly
  // configured digitally and still be inaudible if the speaker attenuator
  // or the amp power bit is wrong, which is exactly the failure mode the
  // 0x92-vs-0x00 L_VOL_TO_SPK bug produced.
  this->select_page_(0x01);
  bool ok = this->read_byte(REG_L_VOL_TO_SPK, &this->rb_spk_vol_);
  ok &= this->read_byte(REG_SPK_DRIVER, &this->rb_spk_drv_);
  ok &= this->read_byte(REG_SPK_AMP, &this->rb_spk_amp_);
  this->analog_readback_ok_ = ok;
  this->select_page_(0x00);
}

void DAC3100Component::dump_config() {
  ESP_LOGCONFIG(TAG, "DAC3100:");
  LOG_I2C_DEVICE(this);

  if (!this->readback_ok_) {
    ESP_LOGE(TAG, "  Readback FAILED -- codec is not responding on I2C");
    return;
  }

  ESP_LOGCONFIG(TAG,
                "  DAC_DATA_PATH_SETUP: 0x%02X (expect 0xD5 = both channels powered)\n"
                "  DAC_VOL_CTRL:        0x%02X (expect 0x00 = unmuted)",
                this->rb_data_path_, this->rb_vol_ctrl_);
  if ((this->rb_data_path_ & 0xC0) != 0xC0) {
    ESP_LOGW(TAG, "  DAC channels are NOT powered up -- output will be silent");
  }

  if (this->analog_readback_ok_) {
    ESP_LOGCONFIG(TAG,
                  "  L_VOL_TO_SPK:        0x%02X (0x00 = 0dB, higher = attenuated)\n"
                  "  SPK_DRIVER:          0x%02X (bit2 set = unmuted)\n"
                  "  SPK_AMP:             0x%02X (expect 0x86 = amp powered)",
                  this->rb_spk_vol_, this->rb_spk_drv_, this->rb_spk_amp_);
    if ((this->rb_spk_amp_ & 0x80) == 0) {
      ESP_LOGW(TAG, "  Speaker amp is NOT powered -- output will be silent");
    }
  }

  ESP_LOGCONFIG(TAG, "  Volume: %u%%%s", this->volume_, this->muted_ ? " (MUTED)" : "");
}

}  // namespace dac3100
}  // namespace esphome
