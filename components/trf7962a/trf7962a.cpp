#include "trf7962a.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include <cinttypes>
#include <cstring>

namespace esphome {
namespace trf7962a {

static const char *const TAG = "trf7962a";

// Timeouts for the transmit/receive state machines, in microseconds.
//
// THESE ARE DELIBERATELY SHORTER THAN THE REFERENCE DRIVER'S 50ms. That
// driver polls from its own FreeRTOS task, where a 50ms block costs nothing.
// update() here runs on the ESPHome main loop, which also feeds the audio
// pipeline -- blocking it for 50ms several times a second causes dropouts
// and trips the component watchdog. An ISO15693 inventory round trip is
// well under 10ms, so these are generous for the protocol while staying
// cheap enough for the loop. Raise them only if real tags time out.
// Both are 50ms in the reference driver (trf7962a.c: `uint32_t timeout =
// 50000` and the Tx wait's `> 50000`). At 10ms an ISO15693 exchange can be
// abandoned before the tag has finished answering.
static const uint32_t TX_TIMEOUT_US = 50000;
static const uint32_t RX_TIMEOUT_US = 50000;
// Once the first response bytes arrive the tag is talking, so the gap timeout
// can drop a long way. This is now the ONLY thing that ends a reception (see
// read_packet_), so it is load-bearing in a way it was not before.
//
// teddybox uses 5000. 2000 here, from the wire arithmetic rather than taste:
// ISO15693 high bitrate is 26.48 kbps, so one byte takes ~302us, and 2ms is
// over six byte-times of silence -- comfortably past any inter-byte gap, and
// a frame that has ended never produces another byte. The 3ms saved matters
// because this wait is now paid on EVERY successful exchange, up to four
// times per poll, on the ESPHome main loop that also feeds audio. If frames
// ever come back truncated at the tail, raise this FIRST before suspecting
// anything else.
static const uint32_t RX_CONTINUE_TIMEOUT_US = 2000;

// ISO15693 CRC16 (ISO/IEC 13239): polynomial 0x8408, preset 0xFFFF, and the
// result is inverted and sent LSB-first. A correct frame therefore checksums
// to the constant 0xF0B8 when the CRC bytes are included in the calculation.
static const uint16_t ISO15693_CRC_PRESET = 0xFFFF;
static const uint16_t ISO15693_CRC_RESIDUE = 0xF0B8;

static uint16_t iso15693_crc(const uint8_t *data, uint8_t length) {
  uint16_t crc = ISO15693_CRC_PRESET;
  for (uint8_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
  }
  return crc;
}

// A tag must be missed this many polls in a row before we report it removed.
// A single INVENTORY can fail for reasons other than the tag being gone
// (collision, the figure being lifted a few mm), and flapping the media
// player on/off is much worse than reacting one poll late.
// Derived from the `removal_debounce` YAML key against the update interval,
// because "how long may the box be knocked for" is the thing worth tuning
// and it must not silently change when the poll rate does. Never zero --
// that would report removal on the very first missed read.
static uint8_t miss_threshold_for(uint32_t debounce_ms, uint32_t interval_ms) {
  if (interval_ms == 0)
    return 1;
  uint32_t polls = debounce_ms / interval_ms;
  if (polls < 1)
    polls = 1;
  if (polls > 255)
    polls = 255;
  return static_cast<uint8_t>(polls);
}

// Command/address byte layout (TRF7962A datasheet, "Serial Interface"):
//   bit7: 1=command, 0=register access
//   bit6: 1=read, 0=write (register access only)
//   bit5: 1=continuous address mode
static uint8_t addr_byte_(uint8_t addr, bool is_command, bool read, bool continuous) {
  uint8_t b = addr & 0x1F;
  if (is_command) b |= 0x80;
  if (read) b |= 0x40;
  if (continuous) b |= 0x20;
  return b;
}

void TRF7962AComponent::cs_low_() {
  if (this->cs_pin_ != nullptr) {
    this->cs_pin_->digital_write(false);
  }
}

void TRF7962AComponent::cs_high_() {
  if (this->cs_pin_ != nullptr) {
    this->cs_pin_->digital_write(true);
  }
}

void TRF7962AComponent::write_register_(uint8_t reg, uint8_t value) {
  this->cs_low_();
  this->enable();
  this->write_byte(addr_byte_(reg, false, false, false));
  this->write_byte(value);
  this->disable();
  this->cs_high_();
}

void TRF7962AComponent::read_registers_(uint8_t reg, uint8_t *data, size_t count) {
  if (count == 0) {
    return;
  }
  uint8_t cmd = addr_byte_(reg, false, true, count > 1);

  // CS is asserted by hand and stays low across BOTH phases, as the chip
  // requires. It cannot be left to the delegates: they are registered with
  // a null CS precisely so that the two phases below can hand the bus
  // between two different IDF devices without dropping the line.
  this->cs_low_();

  // Phase 1 -- send the address byte in mode 0 (this object's mode).
  //
  // >>> THE enable()/disable() PAIRS BELOW MUST NOT NEST. <<<
  // ESPHome's enable() calls spi_device_acquire_bus() and disable() calls
  // spi_device_release_bus(). Holding the write device's lock while the
  // read device transmits sends spi_device_polling_start() down the
  // "someone else owns the bus" path, and it blocks forever inside the bus
  // lock -- an unrecoverable boot hang, which is exactly the crash the
  // first version of this driver caused (fault on core 1 during setup(),
  // then an OTA rollback to the previous image). Acquire, transfer,
  // release; then acquire on the other device.
  this->enable();
  this->write_byte(cmd);
  this->disable();

  // Phase 2 -- clock the data back in mode 1 (the read client's mode).
  //
  // QUIRK, sloa140b.pdf 1.1: the chip latches the command byte on one clock
  // edge and drives its response on the other. Doing both phases in one mode
  // reads garbage. This is why TRF7962AReadClient exists.
  //
  // QUIRK, and this one cost the reference driver's author 12 hours: the
  // ESP32 leaves MOSI high after clocking a byte. If the TX buffer is zeros,
  // MOSI therefore toggles between bytes. The TRF7962A is supposed to ignore
  // MOSI while it is driving MISO -- it does not. It "totally freaks out and
  // delivers the first byte three times, then continues normally."
  //
  // >>> IF YOU EVER SEE A REGISTER VALUE REPEATED THREE TIMES IN A LOG,
  // >>> THIS IS THE CAUSE. It is not a hardware fault. Do not go looking
  // >>> for one. The fix is that MOSI must be held high, hence the 0xFF
  // >>> fill below -- ESPHome's read_array() sends zeros, so it cannot be
  // >>> used here.
  // transfer_array() is in-place: it sends the buffer's contents and
  // overwrites them with what came back. So pre-fill the caller's buffer
  // with 0xFF and read straight into it.
  memset(data, 0xFF, count);
  this->read_client_.enable();
  this->read_client_.transfer_array(data, count);
  this->read_client_.disable();

  this->cs_high_();
}

uint8_t TRF7962AComponent::read_register_(uint8_t reg) {
  uint8_t value = 0;
  this->read_registers_(reg, &value, 1);
  return value;
}

void TRF7962AComponent::set_register_mask_(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t val = 0;
  if (mask != 0) {
    val = this->read_register_(reg);
    val &= mask;
  }
  val |= value;
  this->write_register_(reg, val);
}

void TRF7962AComponent::send_direct_command_(uint8_t command) {
  // QUIRK, sloa140b.pdf 1.3: some direct commands need an extra clock cycle
  // after the command byte. The reference driver clocks 9 bits rather than 8
  // for every command; ESPHome's delegate exposes that as write(data, bits).
  this->cs_low_();
  this->enable();
  this->write(addr_byte_(command, true, false, false) << 1, 9);
  this->disable();
  this->cs_high_();
}

uint8_t TRF7962AComponent::read_irq_status_() {
  // QUIRK, sloa140b.pdf 1.2: a dummy byte must be read after the IRQ status
  // register, otherwise the chip's internal pointer is left in a bad state.
  uint8_t status[2] = {0, 0};
  this->read_registers_(REG_IRQ_STATUS, status, 2);
  return status[0];
}

uint8_t TRF7962AComponent::read_fifo_status_() { return this->read_register_(REG_FIFO_STATUS); }

void TRF7962AComponent::read_fifo_(uint8_t *data, uint8_t length) {
  // QUIRK, sloa140b.pdf 1.6: when the low bit of the read address is 1, the
  // chip emits two spurious zero bytes. Dodge it by starting the read one
  // register early (TX_LENGTH_BYTE2, an even address) and discarding the
  // first byte.
  //
  // NOTE, if you are comparing against RvX_TRF7962A: it does NOT do this. It
  // reads REG_FIFO (0x1F, an odd address) head-on, walks into the quirk, and
  // cleans up afterwards by checking whether the first three bytes came back
  // identical and memmove-ing two off the front if so. Two different fixes
  // for the same silicon bug -- DO NOT APPLY BOTH. The address dodge here is
  // deterministic, so the triple-byte heuristic is deliberately absent.
  uint8_t tmp[TRF7962A_FIFO_SIZE + 8];
  if (length + 1u > sizeof(tmp)) {
    ESP_LOGW(TAG, "FIFO read of %u bytes too large", length);
    return;
  }
  this->read_registers_(REG_TX_LENGTH_BYTE2, tmp, length + 1u);
  memcpy(data, &tmp[1], length);
}

void TRF7962AComponent::init_registers_() {
  for (const auto &entry : TRF7962A_INIT_REGS) {
    this->set_register_mask_(entry.reg, entry.mask, entry.value);
  }
}

void TRF7962AComponent::reinit_chip_() {
  // teddybox's trf7962a_reset(), byte for byte: the three direct commands,
  // then the WHOLE register table, then an IRQ-status read to clear anything
  // latched. CMD_SOFT_INIT is a power-on-reset equivalent, so it wipes every
  // register the table then puts back -- applying a subset here would leave
  // RX_WAIT_TIME, RX_NO_RESPONSE_WAIT_TIME, RX_SPECIAL_SETTINGS and
  // SPECIAL_FUNCTION_1 sitting at silicon defaults for the rest of the poll.
  // The RF field is deliberately not touched here; every caller sets it.
  this->send_direct_command_(CMD_SOFT_INIT);
  this->send_direct_command_(CMD_IDLE);
  this->send_direct_command_(CMD_RESET_FIFO);
  this->init_registers_();
  this->read_irq_status_();
}

bool TRF7962AComponent::reset_chip_() {
  this->reinit_chip_();

  // Chip-presence check. Everything above is write-only, so without this a
  // dead or miswired chip looks exactly like a working one with no tag on
  // it -- which is how this driver silently did nothing for weeks. Reading
  // the value back turns that into a specific error at boot.
  uint8_t val = this->read_register_(REG_CHIP_STATUS_CTRL);
  if (val != TRF7962A_CHIP_STATUS_EXPECTED) {
    ESP_LOGE(TAG, "Chip not found: REG_CHIP_STATUS_CTRL reads 0x%02X, expected 0x%02X", val,
             TRF7962A_CHIP_STATUS_EXPECTED);
    if (val == 0x00 || val == 0xFF) {
      ESP_LOGE(TAG, "  0x%02X usually means MISO is stuck -- check wiring/power", val);
    }
    return false;
  }
  return true;
}

void TRF7962AComponent::set_field(bool enabled) {
  if (!this->chip_ok_) {
    return;
  }
  // Write the whole register, as RvX_TRF7962A's turnFieldOn/Off do:
  // 0b00100001 on, 0b00000001 off. A masked read-modify-write is not
  // equivalent here -- the reference sets the byte outright.
  this->write_register_(REG_CHIP_STATUS_CTRL, enabled ? 0x21 : 0x01);
  if (enabled) {
    // ISO15693-3 requires the VCD to wait after activating the field before
    // the first request, so the VICCs are ready to receive it. The reference
    // notes 1ms is not enough in practice and uses 10ms.
    delay(10);
  }
}

void TRF7962AComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up TRF7962A...");

  // Take the CS pin away from the delegate and drive it by hand. A register
  // read spans two SPI devices (mode 0 for the address, mode 1 for the data)
  // and CS must stay low across both, which no single delegate can do.
  GPIOPin *cs = this->cs_;
  this->cs_ = nullptr;
  this->cs_pin_ = cs;
  if (this->cs_pin_ != nullptr) {
    this->cs_pin_->setup();
    this->cs_pin_->digital_write(true);
  }

  this->spi_setup();
  // Second delegate on the same bus, mode 1, for the read phase.
  this->read_client_.setup_read_client(this->parent_);

  if (this->irq_pin_ != nullptr) {
    this->irq_pin_->setup();
    // Latch every IRQ pulse in an ISR so read_packet_'s poll loop can tell
    // "an edge happened since I last checked" from "nothing happened" --
    // see Trf7962aIrqStore. Without this, an edge landing between two
    // register polls is simply lost.
    this->irq_store_.setup(this->irq_pin_);
  }

  this->chip_ok_ = this->reset_chip_();
  if (!this->chip_ok_) {
    ESP_LOGE(TAG, "TRF7962A init failed; NFC will be unavailable");
    this->mark_failed();
    return;
  }

  // Field is left down here on purpose: update() brings it up for each poll
  // and drops it again afterwards, matching RvX_TRF7962A::loop().
  this->set_field(false);

  this->read_back_init_registers_();

  ESP_LOGCONFIG(TAG, "TRF7962A initialised OK");
}

void TRF7962AComponent::read_back_init_registers_() {
  for (size_t i = 0; i < TRF7962A_INIT_REGS_COUNT; i++) {
    this->init_readback_[i] = this->read_register_(TRF7962A_INIT_REGS[i].reg);
  }
}

bool TRF7962AComponent::write_packet_(const uint8_t *data, uint8_t length) {
  // ONE unbroken CS assertion for reset + transmit + length + payload. This
  // mirrors RvX_TRF7962A::sendDataTag, which builds a single buffer
  //   8F 91 3D <len_hi> <len_lo> <payload...>
  // and pushes it with one sendRaw() call.
  //
  // >>> THE ONE THING THAT MUST NOT CHANGE: the burst below stays inside a
  // >>> SINGLE CS assertion. <<<
  // Dropping CS between issuing TRANSMIT_CRC and loading the payload sends
  // the frame out before -- or without -- its data, because the chip starts
  // transmitting on the command byte. That was the real cause of "the tag
  // never answers", with every Rx pass measuring avail=0 over a genuinely
  // empty FIFO:
  //   PROBE pass irq=0x40 avail=0 len=0
  // A comment here used to blame the two direct commands below for that and
  // told the next reader to keep them deleted. They were never the problem:
  // teddybox issues both (trf7962a_write_packet) and still transmits fine,
  // because its own burst is likewise atomic. Restored 2026-08-06.
  //
  // 0x3D is REG_TX_LENGTH_BYTE1 (0x1D) with the continuous bit set. In
  // continuous mode the chip auto-increments 0x1D -> 0x1E -> 0x1F, so the
  // two length bytes land in the length registers and everything after
  // flows into the FIFO (0x1F) on its own. Dropping CS aborts that walk.

  // Start from a known-clean IRQ register. teddybox never needs this because
  // its read_packet loop has no early exit: it polls the IRQ register in a
  // tight spin until a 5ms inter-byte gap expires, so the register was last
  // read (and cleared) microseconds before it returns. read_packet_ here
  // returns the instant it sees a clean RX_COMPLETE, which leaves the chip
  // free to latch more bits afterwards -- measured after every successful
  // GET_RANDOM as a leftover 0x42 (RX_COMPLETE|COLLISION) still sitting in
  // the register when the NEXT exchange started. Clearing it here, one SPI
  // transaction before the frame goes out, is the narrowest possible window;
  // the old reinit_field_() cleared it 10ms early and then held the receiver
  // armed across that whole delay for fresh noise to re-latch into.
  this->read_irq_status_();

  // teddybox's trf7962a_write_packet() opens with these two direct commands
  // before it builds the FIFO burst. An earlier version of this driver
  // dropped them, blaming them for the "tag never answered" failure -- the
  // actual cause was the CS window inside the burst below, which is fixed and
  // stays fixed. Idling first also stops any reception still in progress.
  this->send_direct_command_(CMD_IDLE);
  this->send_direct_command_(CMD_RESET_FIFO);

  uint8_t header[5];
  header[0] = addr_byte_(CMD_RESET_FIFO, true, false, false);
  header[1] = addr_byte_(CMD_TRANSMIT_CRC, true, false, false);
  header[2] = addr_byte_(REG_TX_LENGTH_BYTE1, false, false, true);
  header[3] = length >> 4;
  header[4] = length << 4;

  uint8_t first = length < TRF7962A_FIFO_SIZE ? length : TRF7962A_FIFO_SIZE;
  this->cs_low_();
  this->enable();
  this->write_array(header, sizeof(header));
  this->write_array(data, first);
  this->disable();
  this->cs_high_();

  uint32_t sent = first;
  uint32_t fill_start = micros();
  while (sent < length) {
    // Subsequent chunks go straight into the FIFO, sized by how much room
    // the chip reports. Unreachable for a 3-byte INVENTORY, which fits in
    // one FIFO load, but multi-block SLIX reads will come through here.
    uint8_t fifo = this->read_fifo_status_();
    uint8_t avail = TRF7962A_FIFO_SIZE - (fifo & 0x0F);
    if (avail == 0) {
      // Bounded on purpose: a chip that stops draining its FIFO must not
      // spin the ESPHome main loop forever. The reference driver can get
      // away with an unbounded wait because it owns a FreeRTOS task.
      if (micros() - fill_start > TX_TIMEOUT_US) {
        ESP_LOGW(TAG, "FIFO never drained, %u/%u bytes sent", (unsigned) sent, length);
        return false;
      }
      continue;
    }
    fill_start = micros();
    uint8_t chunk = (length - sent) < avail ? (length - sent) : avail;
    this->cs_low_();
    this->enable();
    this->write_byte(addr_byte_(REG_FIFO, false, false, true));
    this->write_array(&data[sent], chunk);
    this->disable();
    this->cs_high_();
    sent += chunk;
  }

  uint32_t start = micros();
  while (true) {
    this->irq_status_ = this->read_irq_status_();

    if (this->irq_status_ & IRQ_ERROR_MASK) {
      ESP_LOGW(TAG, "Tx failed (IRQ 0x%02X)", this->irq_status_ & IRQ_ERROR_MASK);
      return false;
    }
    if (this->irq_status_ & IRQ_TX_COMPLETE) {
      // CONSUME TX_COMPLETE before returning. Without this the bit is still
      // set when read_packet_ starts, so its first pass reads 0xC0
      // (TX_COMPLETE | RX_COMPLETE) instead of a clean 0x40 -- measured:
      //   PROBE pass irq=0xC0 avail=1 len=0
      //   PROBE pass irq=0x40 avail=3 len=1
      //   PROBE frame (4 bytes): 00.6E.7C.62   CRC mismatch
      // That first pass drains a partially-filled FIFO mid-reception and the
      // rest arrives on the next one, splitting a single 5-byte response into
      // a 4-byte fragment. RvX never hits this because waitTxIRQ() consumes
      // the TX interrupt before waitRxIRQ() ever looks at the register.
      this->read_irq_status_();
      // NOTE: sloa248b.pdf 4.6 says the FIFO *must* be reset here. Doing so
      // breaks reception entirely, and the original datasheet does not ask
      // for it. Deliberately not done -- see the reference driver's comment.
      //
      // Nothing else happens between Tx completing and read_packet_ arming:
      // the reference falls straight through, and an added settle delay only
      // burns part of the window the tag answers in.
      return true;
    }
    if (micros() - start > TX_TIMEOUT_US) {
      // Seen on the 8-byte SET_PASSWORD frame: IRQ 0x00 and FIFO 0x00 means
      // the chip never reported TX_COMPLETE for a multi-byte payload, unlike
      // the 3-byte GET_RANDOM which completes fine.
      ESP_LOGW(TAG, "Tx timeout after %u bytes, IRQ 0x%02X FIFO 0x%02X", length, this->irq_status_,
               this->read_fifo_status_());
      return false;
    }
  }
}

bool TRF7962AComponent::read_packet_(uint8_t *data, uint8_t *length, uint8_t expected) {
  uint32_t timeout = RX_TIMEOUT_US;
  bool started = false;
  *length = 0;

  // >>> RX_COMPLETE MEANS "A FRAME STARTED", NOT "A FRAME FINISHED". <<<
  //
  // This loop drains the FIFO REPEATEDLY and ends the frame on a quiet gap,
  // never on an IRQ bit. teddybox's trf7962a_read_packet has no early exit at
  // all -- its only way out is the timeout -- and that is not an accident of
  // its threading model, it is the whole mechanism.
  //
  // An earlier version here took ONE snapshot 400us after RX_COMPLETE, read
  // FIFO_STATUS + 1, drained that, sent RESET_FIFO and returned. Everything
  // that landed in the FIFO after the snapshot was discarded. Measured
  // 2026-08-06 at RSSI 0x7F (tag firmly coupled, so not a coupling problem):
  //   PROBE frame (4 bytes): 00.83.ED.83   CRC mismatch, residue 0x0FDB
  //   PROBE frame (4 bytes): 00.F6.59.50   CRC mismatch, residue 0x0FA6
  //   ... 8 of 10 such frames became CRC-valid with EXACTLY ONE byte appended
  //   at the tail, none with a byte inserted anywhere else. Brute-forced, not
  //   inferred -- a 1-in-256 coincidence hitting 8 times is not a coincidence.
  // The same build read a 12-byte INVENTORY response as 6 bytes, and got a
  // complete 5-byte GET_RANDOM roughly one poll in twenty. A fixed off-by-N
  // cannot be sometimes-right and cannot lose six bytes; a snapshot taken
  // mid-reception does exactly both.
  //
  // The "+1" that used to be added to every FIFO_STATUS read is GONE. The one
  // genuinely missing byte is teddybox's straggler read on the timeout path
  // below -- once per frame, not once per drain.
  uint32_t start = micros();
  while (true) {
    if (micros() - start > timeout) {
      break;
    }

    // Re-read every iteration; reading the register also clears it.
    uint8_t irq = this->read_irq_status_();
    this->irq_status_ = irq;

    if (!started) {
      if (irq & (IRQ_RX_COMPLETE | IRQ_FIFO_HIGH_OR_LOW)) {
        // Reception has begun. Fall through to the drain gate -- which will
        // deliberately NOT drain on this pass, see below.
        started = true;
      } else if (irq == IRQ_NO_RESPONSE) {
        // Ordinary empty-field case, not a fault: no reset, no log.
        return false;
      } else if (irq & (IRQ_COLLISION_ERROR | IRQ_CRC_ERROR | IRQ_FRAMING_ERROR)) {
        // >>> ERRORS ARE ONLY FATAL BEFORE RECEPTION STARTS. <<<
        // Once `started` is set they are ignored, because a polled reader
        // sees bits ACCUMULATED over the whole window: a good reception here
        // routinely reads back as 0x42 (RX_COMPLETE|COLLISION) or 0x46
        // (|FRAMING) because a noise flag latched alongside the real
        // completion. RvX can check errors first only because it is
        // interrupt-driven and sees each event alone. Porting its order
        // verbatim broke this driver completely (every read 0 bytes).
        this->log_rx_error_(irq, *length);
        this->reinit_chip_();
        return false;
      } else {
        continue;  // 0x00 idle, or a leftover Tx bit -- nothing yet.
      }
    }

    // teddybox's drain gate, verbatim: `if ((irq_status & 0x60) != 0x40)`.
    // On a BARE RX_COMPLETE pass it does not drain -- it lets the loop go
    // round once more, by which time reading the register has cleared it to
    // 0x00 and the drain runs. That extra lap is the settle: it is why
    // teddybox needs no delayMicroseconds() here and why the 400us fixed wait
    // this driver used instead is gone.
    if ((irq & (IRQ_RX_COMPLETE | IRQ_FIFO_HIGH_OR_LOW)) == IRQ_RX_COMPLETE) {
      continue;
    }

    uint8_t avail = this->read_fifo_status_() & 0x0F;

    // A fill state at or past the FIFO size is not a real count -- a 12-byte
    // FIFO cannot hold 13. Keep what was collected and let the CRC judge it.
    if (avail >= TRF7962A_FIFO_SIZE) {
      ESP_LOGD(TAG, "Rx FIFO fill state %u, stopping with %u bytes", avail, *length);
      break;
    }
    if (avail == 0) {
      continue;  // nothing new since the last drain; keep waiting for the gap
    }

    // >>> NEVER DRAIN PAST THE END OF THE FRAME WE ASKED FOR. <<<
    // Once the response has fully arrived, FIFO_STATUS keeps reporting a
    // stale non-zero count and those reads come back as 0xFF filler, which
    // then gets appended to a frame that was already perfect and fails its
    // CRC. Measured 2026-08-06, a correct 12-byte INVENTORY drowned in junk:
    //   PROBE drain avail=10 len=10 irq=0x60
    //   PROBE drain avail=10 len=20 irq=0x66
    //   Rx buffer full (20 + 8)
    //   frame (20 bytes): 00.00.uu.uu.uu.uu.uu.uu.uu.uu.cc.cc.FF.FF.FF.FF...
    //   (uu = the 8 UID bytes, cc = CRC -- a perfectly good 12-byte frame
    //    with 8 bytes of 0xFF filler stuck on the end)
    // That cost a real tag read roughly one poll in three, and three misses
    // in a row drop the figure and stop playback.
    //
    // The FIFO is a FIFO: the genuine bytes always come out before the junk,
    // so clamping to `expected` keeps exactly the frame and discards the
    // tail. `expected` is the ON-WIRE length including the CRC -- 5 for
    // GET_RANDOM, 3 for SET_PASSWORD, 12 for INVENTORY -- not the length the
    // caller checks after read_packet_ strips the two CRC bytes.
    if (avail > expected - *length) {
      avail = expected - *length;
    }
    if (*length + avail > TRF7962A_RX_BUFFER_SIZE) {
      ESP_LOGW(TAG, "Rx buffer full (%u + %u)", *length, avail);
      break;
    }

    this->read_fifo_(&data[*length], avail);
    (*length) += avail;
    // One line per drain, so a multi-pass reception is visible as several
    // lines. A 12-byte INVENTORY should show more than one. VERBOSE because
    // this is the instrument for re-measuring Rx timing, not a routine log --
    // at `logger: level: DEBUG` it compiles out entirely.
    ESP_LOGV(TAG, "drain avail=%u len=%u irq=0x%02X", avail, *length, irq);

    // Bytes are still flowing: restart the clock on the short inter-byte gap
    // rather than the long wait-for-anything one. At ISO15693 high bitrate
    // (26.48 kbps) one byte is ~302us on the wire, so RX_CONTINUE_TIMEOUT_US
    // is several byte-times of silence -- long enough to be sure the frame
    // ended, short enough that it does not dominate the poll budget.
    if (*length >= expected) {
      break;  // whole frame in hand; no reason to sit out the gap timeout
    }
    start = micros();
    timeout = RX_CONTINUE_TIMEOUT_US;
  }

  // teddybox: "hack. we are always reading one less than in buffer because
  // this chip sucks. read that remaining byte here, if we already received
  // data." FIFO_STATUS's count really is one short on this chip's Rx path --
  // that part of the old per-drain "+1" comment was right, it was applying it
  // to every drain that was wrong.
  //
  // Deliberately AFTER the loop rather than inside the timeout branch, so it
  // runs however the loop ended. A stale `avail >= 12` fill state can abort a
  // frame sitting one byte from complete, and that exit used to skip the
  // straggler and hand a guaranteed CRC failure to the caller.
  if (*length > 0 && *length < expected) {
    this->read_fifo_(&data[*length], 1);
    (*length)++;
  }

  // Nothing was ever drained: the tag stayed silent, or RX_COMPLETE latched
  // over an empty FIFO. Either way there is no frame -- and no diagnostic
  // value in logging it, since an idle field does this on every poll.
  if (*length == 0) {
    return false;
  }

  // RSSI IS PART OF EVERY FRAME LOG FROM HERE ON. A short or CRC-failing
  // frame means something completely different at RSSI 0x7E (tag firmly
  // coupled -- a real driver problem) than at 0x40-0x42 (nothing coupling --
  // noise, and the log says nothing about the driver at all). Two sessions
  // have now been spent reasoning about frame contents captured at unknown
  // coupling; reading one extra register here ends that permanently. Read
  // after the drain loop, never during it, so it cannot steal Rx time.
  uint8_t rssi = this->read_register_(REG_RSSI_LEVELS);

  if (*length < 3) {
    ESP_LOGD(TAG, "Runt frame (%u bytes, IRQ 0x%02X, RSSI 0x%02X): %s", *length, this->irq_status_, rssi,
             format_hex_pretty(data, *length).c_str());
    return false;
  }

  // Dump every assembled frame before the CRC check, so a full-length frame
  // that merely fails CRC is distinguishable from one never captured. VERBOSE:
  // this fires on every successful poll, which buries the log while a tag
  // sits on the box. The CRC-mismatch line below carries the RSSI for the
  // failure case, which is the one worth seeing at DEBUG.
  ESP_LOGV(TAG, "frame (%u bytes, RSSI 0x%02X): %s", *length, rssi,
           format_hex_pretty(data, *length).c_str());

  // VERIFY THE CRC. This is not optional hygiene -- it is the only thing
  // standing between a corrupted frame and a bogus tag UID. Without it,
  // frames whose first 9 bytes were correct but whose tail was garbage were
  // accepted as valid inventories, producing a different "tag" UID on every
  // poll and re-triggering playback each time:
  //   00 00 F8 4D 78 1B 50 03 04 E0 FF 49  -> E00403501B784DF8  (real)
  //   00 00 F8 4D 78 1B 50 03 04 1C 00 00  -> 1C0403501B784DF8  (garbage)
  // Both of those were checked against this function: the first gives the
  // 0xF0B8 residue, the second does not.
  uint16_t residue = iso15693_crc(data, *length);
  if (residue != ISO15693_CRC_RESIDUE) {
    // RSSI stays on this line even though the frame dump above is VERBOSE --
    // see the note on reading it: a CRC failure at 0x7E is a driver problem,
    // the same failure at 0x40-0x42 is just nothing coupling.
    ESP_LOGD(TAG, "CRC mismatch (%u bytes, residue 0x%04X, RSSI 0x%02X)", *length, residue, rssi);
    return false;
  }

  // Drop the two trailing CRC bytes now that they have been checked. RvX
  // makes the same subtraction (its "-2 //WTF?"), which is what makes its
  // exact length checks -- 3 for GET_RANDOM, 10 for INVENTORY -- line up.
  *length -= 2;

  // A good read re-arms error logging so the next fault is reported once.
  this->last_rx_error_ = 0;
  return true;
}

void TRF7962AComponent::log_rx_error_(uint8_t irq, uint8_t length) {
  // An idle field produces errors constantly, so log only the FIRST
  // occurrence of a given code and stay quiet until it changes. Otherwise
  // this prints twice a second forever and buries everything else.
  if (irq != this->last_rx_error_) {
    ESP_LOGD(TAG, "Rx failed (IRQ 0x%02X), %u bytes -- suppressing repeats", irq, length);
    this->last_rx_error_ = irq;
  }
}

bool TRF7962AComponent::transceive_(const uint8_t *tx, uint8_t tx_length, uint8_t *rx, uint8_t *rx_length,
                                    uint8_t expected) {
  if (!this->chip_ok_) {
    return false;
  }
  if (!this->write_packet_(tx, tx_length)) {
    // Every write_packet_ failure already logs at warning level, but say which
    // phase gave up so a Tx fault is never mistaken for an unanswered Rx.
    ESP_LOGD(TAG, "Exchange aborted in Tx phase");
    return false;
  }
  return this->read_packet_(rx, rx_length, expected);
}

bool TRF7962AComponent::get_random_(uint8_t *rand) {
  // The reference driver (main/nfc.c, STATE_SEARCHING) sends GET_RANDOM_NUMBER
  // before it will attempt an inventory at all, and resets the chip if it
  // fails. Note the flags byte is 0x02, NOT the 0x26 used for inventory.
  const uint8_t request[] = {0x02, ISO_GET_RANDOM_NUMBER, 0x04};

  uint8_t response[TRF7962A_FIFO_SIZE + 8];
  uint8_t response_length = 0;

  // 5 on the wire: flags + 2 random + 2 CRC.
  if (!this->transceive_(request, sizeof(request), response, &response_length, 5)) {
    return false;
  }
  // Exactly 3 bytes: flags + 2 bytes of random. read_packet_ strips the CRC,
  // so this matches RvX_TRF7962A's trfRxLength == 3 check directly.
  if (response_length != 3 || response[0] != 0x00) {
    ESP_LOGD(TAG, "GET_RANDOM: %u bytes, flags 0x%02X", response_length, response_length ? response[0] : 0);
    return false;
  }
  rand[0] = response[1];
  rand[1] = response[2];
  ESP_LOGD(TAG, "GET_RANDOM ok: %02X %02X", rand[0], rand[1]);
  return true;
}

bool TRF7962AComponent::set_password_(uint32_t password) {
  // SLIX-L privacy mode: the tag stays silent to INVENTORY until it has been
  // unlocked. Ported from RvX_TRF7962A::ISO15693_setPassSlixL -- GET_RANDOM
  // first, then SET_PASSWORD with the password XORed against that random.
  uint8_t rand[2] = {0, 0};
  if (!this->get_random_(rand)) {
    return false;
  }

  // NOTHING BETWEEN THE TWO HALVES. teddybox (nfc.c STATE_SEARCHING) does its
  // full nfc_reset() and field cycle BEFORE the GET_RANDOM, then fires
  // SET_PASSWORD immediately after it -- no delay, no register write, no
  // field re-arm. This driver used to call reinit_field_() here, copied from
  // RvX_TRF7962A::ISO15693_setPassSlixL, which writes CHIP_STATUS_CTRL again
  // and sleeps 10ms. That 10ms leaves the receiver armed with no request
  // outstanding -- a window for a spurious collision to latch in -- and it is
  // exactly where every SET_PASSWORD failure has landed:
  //   GET_RANDOM ok: 11 7F
  //   PROBE reinit_field irq_before=0x42 elapsed=10133us
  //   PROBE tx ok len=8 irq=0x80
  //   Rx failed (IRQ 0x02), 0 bytes
  // NOT yet proven to be the cause -- what IS established is that it is a
  // divergence from teddybox at the precise failing step, so teddybox wins.
  // The IRQ clear that reinit_field_() also did has moved into
  // write_packet_, where it happens immediately before the frame instead of
  // 10ms ahead of it. Where the two references disagree, follow teddybox --
  // it is the one running ESP-IDF on this exact Toniebox.
  uint8_t pw[4];
  pw[0] = (password >> 0) & 0xFF;
  pw[1] = (password >> 8) & 0xFF;
  pw[2] = (password >> 16) & 0xFF;
  pw[3] = (password >> 24) & 0xFF;
  if (rand[0] || rand[1]) {
    pw[0] ^= rand[0];
    pw[1] ^= rand[1];
    pw[2] ^= rand[0];
    pw[3] ^= rand[1];
  }

  uint8_t request[8];
  request[0] = 0x02;               // high data rate
  request[1] = ISO_SET_PASSWORD;   // 0xB3
  request[2] = 0x04;               // NXP manufacturer code
  request[3] = 0x04;               // password identifier (privacy)
  memcpy(&request[4], pw, 4);

  uint8_t response[TRF7962A_FIFO_SIZE + 8];
  uint8_t response_length = 0;
  // 3 on the wire: flags + 2 CRC.
  if (!this->transceive_(request, sizeof(request), response, &response_length, 3)) {
    return false;
  }
  // Reference checks trfRxLength == 1 (flags only), which now matches ours
  // directly since read_packet_ strips the CRC.
  if (response_length != 1 || response[0] != 0x00) {
    ESP_LOGD(TAG, "SET_PASSWORD %08" PRIX32 ": %u bytes, flags 0x%02X", password, response_length,
             response_length ? response[0] : 0);
    return false;
  }
  ESP_LOGI(TAG, "SLIX unlocked with password %08" PRIX32, password);
  return true;
}

bool TRF7962AComponent::inventory_(std::vector<uint8_t> &uid) {
  // ISO15693 request flags: 0x26 = high data rate, inventory, one slot.
  // Followed by the INVENTORY opcode and a zero-length mask.
  const uint8_t request[] = {0x26, ISO_INVENTORY, 0x00};

  uint8_t response[TRF7962A_FIFO_SIZE + 8];
  uint8_t response_length = 0;

  // 12 on the wire: flags + DSFID + 8 UID + 2 CRC.
  if (!this->transceive_(request, sizeof(request), response, &response_length, 12)) {
    // No reset here: read_packet_ already calls reinit_chip_() on the faults
    // that need it, and update() re-inits and cycles the field every poll, so
    // a third reset in this path would only fight those two.
    return false;
  }

  // Expected response: flags(1) + DSFID(1) + UID(8) = exactly 10, after
  // read_packet_ has stripped the CRC. RvX_TRF7962A requires the same.
  if (response_length != 10) {
    ESP_LOGD(TAG, "Short inventory response (%u bytes)", response_length);
    return false;
  }
  // A non-zero flags byte means the tag reported an error.
  if (response[0] != 0x00) {
    ESP_LOGD(TAG, "Inventory error flags 0x%02X", response[0]);
    return false;
  }

  // UID is 8 bytes, LSB first on the wire, and is kept in wire order here.
  // Anything rendering it as a string must decide byte order explicitly.
  uid.assign(&response[2], &response[2] + 8);
  // A good read re-arms error logging, so a later fault is reported once.
  this->last_rx_error_ = 0;
  this->no_response_logged_ = false;
  return true;
}

void TRF7962AComponent::update() {
  if (!this->chip_ok_) {
    return;
  }

  // teddybox's nfc_reset(): full reset + whole register table, then a clean
  // field off -> on edge with the ISO15693-3 settle. The table itself raises
  // the field (CHIP_STATUS_CTRL is its first entry), so drop it again and
  // bring it up deliberately once the rest of the registers are right.
  this->reinit_chip_();
  this->set_field(false);
  this->set_field(true);

  std::vector<uint8_t> uid;
  bool found = false;
  // GET_RANDOM's answer is only used as a liveness probe here -- the random
  // that SET_PASSWORD needs is fetched fresh inside set_password_().
  uint8_t rand[2] = {0, 0};

  if (this->tag_present_) {
    // Already unlocked: a plain GET_RANDOM confirms the tag is still there.
    if (this->get_random_(rand)) {
      found = this->inventory_(uid);
    }
  } else if (this->get_random_(rand)) {
    // >>> TRY A PLAIN INVENTORY FIRST. <<<
    // This whole branch used to jump straight to SET_PASSWORD, so INVENTORY
    // was reachable ONLY through a successful unlock -- a tag that answers
    // without one could never be read no matter how well GET_RANDOM worked.
    // teddybox does not assume privacy mode either: nfc.c STATE_SEARCHING
    // sends GET_RANDOM, then INVENTORY, and only calls the tag "locked" and
    // starts the password loop once that INVENTORY has actually failed.
    found = this->inventory_(uid);

    if (!found) {
      // Locked. teddybox runs a full nfc_reset() before each password
      // attempt, so the GET_RANDOM feeding SET_PASSWORD is a fresh one taken
      // on a freshly reset chip -- not the random left over from the
      // INVENTORY probe above. set_password_() fetches its own.
      //
      // One password per poll rather than all three: each attempt costs a
      // reset plus the 10ms ISO15693-3 settle, and three in one update()
      // blows the 120ms component budget.
      uint32_t password = SLIX_DEFAULT_PASSWORDS[this->password_index_];
      this->password_index_ = (this->password_index_ + 1) % (sizeof(SLIX_DEFAULT_PASSWORDS) / sizeof(uint32_t));

      this->reinit_chip_();
      this->set_field(true);
      if (this->set_password_(password)) {
        // Straight into INVENTORY, as teddybox does on entering STATE_TAG.
        found = this->inventory_(uid);
      }
    }
  }

  if (!found && this->fail_count_++ % 20 == 0) {
    uint8_t rssi = this->read_register_(REG_RSSI_LEVELS);
    ESP_LOGD(TAG, "No tag (%" PRIu32 " consecutive), RSSI 0x%02X (main %u, aux %u)", this->fail_count_, rssi,
             rssi & 0x07,
             (rssi >> 3) & 0x07);
  }
  if (found) {
    this->fail_count_ = 0;
  }
  // Field down at the end of every poll, as the reference does.
  this->set_field(false);

  if (found) {
    this->miss_count_ = 0;
    if (!this->tag_present_ || uid != this->current_uid_) {
      this->tag_present_ = true;
      this->current_uid_ = uid;
      ESP_LOGD(TAG, "Tag present: %02X%02X%02X%02X%02X%02X%02X%02X", uid[7], uid[6], uid[5], uid[4], uid[3],
               uid[2], uid[1], uid[0]);
      for (auto *listener : this->listeners_) {
        listener->on_tag_present(uid);
      }
    }
    return;
  }

  if (this->tag_present_) {
    // Debounce removal -- one missed poll is not proof the figure is gone.
    if (++this->miss_count_ < miss_threshold_for(this->removal_debounce_ms_, this->get_update_interval())) {
      return;
    }
    this->tag_present_ = false;
    this->miss_count_ = 0;
    this->current_uid_.clear();
    ESP_LOGD(TAG, "Tag removed");
    for (auto *listener : this->listeners_) {
      listener->on_tag_removed();
    }
  }
}

void TRF7962AComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "TRF7962A:");
  LOG_PIN("  IRQ Pin: ", this->irq_pin_);
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  Removal debounce: %" PRIu32 "ms (%u polls)", this->removal_debounce_ms_,
                miss_threshold_for(this->removal_debounce_ms_, this->get_update_interval()));
  if (!this->chip_ok_) {
    ESP_LOGE(TAG, "  Chip: NOT DETECTED");
    return;
  }
  ESP_LOGCONFIG(TAG, "  Chip: detected");

  // Print the init-table readback taken at the end of setup(). Every register
  // in the table is write-only as far as the rest of this driver is
  // concerned, so a table that silently failed to apply -- wrong mask, a
  // CMD_SOFT_INIT wiping it afterwards, an SPI write that did not land --
  // looks identical to one that worked. This turns that into one line at
  // boot, costs nothing at runtime, and is the fastest way to separate "the
  // driver is misconfigured" from "the tag is not coupling" without a tag
  // present at all.
  //
  // The values are cached rather than read here because dump_config() must
  // not touch hardware -- see read_back_init_registers_().
  //
  // CHIP_STATUS_CTRL reads 0x01, not the table's 0x21: setup() drops the RF
  // field after init and update() owns raising it per poll. That is correct,
  // not a failed write.
  for (size_t i = 0; i < TRF7962A_INIT_REGS_COUNT; i++) {
    const auto &entry = TRF7962A_INIT_REGS[i];
    uint8_t actual = this->init_readback_[i];
    bool ok;
    if (entry.reg == REG_CHIP_STATUS_CTRL) {
      // Field state, not a static setting -- no verdict to give.
      ok = true;
    } else if (entry.mask == 0) {
      ok = actual == entry.value;  // blind write: must match exactly
    } else {
      // Read-modify-write: the OR'd bits must be set, and nothing outside
      // (mask | value) may survive. The pre-write value is unknown, so this
      // is as strong a check as the write itself allows.
      ok = (actual & entry.value) == entry.value && (actual & ~(entry.mask | entry.value)) == 0;
    }
    ESP_LOGCONFIG(TAG, "  reg 0x%02X = 0x%02X%s", entry.reg, actual,
                  ok ? "" : "   <-- NOT WHAT THE TABLE ASKED FOR");
  }
}

}  // namespace trf7962a
}  // namespace esphome
