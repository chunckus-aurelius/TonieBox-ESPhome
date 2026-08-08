#pragma once

// TRF7962A SPI ISO15693 (SLIX) reader.
//
// Protocol/register knowledge referenced from the ESP-IDF driver in
// toniebox-reverse-engineering/teddybox (components/trf7962a), which itself
// credits https://github.com/electricimp/trf7962a for the original TRF7962A
// register map. This is a fresh ESPHome-native implementation, not a direct
// port of either -- ISO15693 and SLIX default passwords are public standards,
// not project-specific secrets.
//
// READ THE QUIRK COMMENTS IN trf7962a.cpp BEFORE CHANGING ANYTHING HERE.
// This chip has at least five documented misbehaviours that all present as
// "reads return garbage", and the reference driver's author burned 12+ hours
// on one of them alone. They are cited to TI app note sloa140b.pdf by section.

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/hal.h"
#include "esphome/components/spi/spi.h"

#include <vector>
#include <cstdint>

namespace esphome {
namespace trf7962a {

// TRF7962A direct commands (see TI TRF7962A datasheet, table "Direct Commands")
enum Trf7962aCommand : uint8_t {
  CMD_IDLE = 0x00,
  CMD_SOFT_INIT = 0x03,
  CMD_RESET_FIFO = 0x0F,
  CMD_TRANSMIT_NO_CRC = 0x10,
  CMD_TRANSMIT_CRC = 0x11,
  CMD_BLOCK_RECEIVER = 0x16,
  CMD_ENABLE_RECEIVER = 0x17,
  CMD_TEST_EXTERNAL_RF = 0x19,
  CMD_RUN_DECODERS = 0x1D,
};

// TRF7962A registers.
enum Trf7962aRegister : uint8_t {
  REG_CHIP_STATUS_CTRL = 0x00,
  REG_ISO_CONTROL = 0x01,
  REG_TX_PULSE_LENGTH_CTRL = 0x06,
  REG_RX_NO_RESPONSE_WAIT_TIME = 0x07,
  REG_RX_WAIT_TIME = 0x08,
  REG_MODULATOR_CTRL = 0x09,
  REG_RX_SPECIAL_SETTINGS = 0x0A,
  REG_REGULATOR_IO_CTRL = 0x0B,
  REG_RSSI_LEVELS = 0x0F,
  REG_IRQ_STATUS = 0x0C,
  REG_IRQ_MASK = 0x0D,
  REG_SPECIAL_FUNCTION_1 = 0x10,
  REG_FIFO_STATUS = 0x1C,
  REG_TX_LENGTH_BYTE1 = 0x1D,
  REG_TX_LENGTH_BYTE2 = 0x1E,
  REG_FIFO = 0x1F,
};

// IRQ status bits (REG_IRQ_STATUS).
enum Trf7962aIrq : uint8_t {
  IRQ_NO_RESPONSE = 0x01,
  IRQ_COLLISION_ERROR = 0x02,
  IRQ_FRAMING_ERROR = 0x04,
  IRQ_PARITY_ERROR = 0x08,
  IRQ_CRC_ERROR = 0x10,
  IRQ_FIFO_HIGH_OR_LOW = 0x20,
  IRQ_RX_COMPLETE = 0x40,
  IRQ_TX_COMPLETE = 0x80,
  IRQ_ERROR_MASK = 0x1F,
};

// ISO15693 (SLIX) command codes used for figure detection.
enum Iso15693Command : uint8_t {
  ISO_INVENTORY = 0x01,
  ISO_GET_SYSTEM_INFO = 0x2B,
  ISO_GET_RANDOM_NUMBER = 0xB2,
  ISO_SET_PASSWORD = 0xB3,
};

// SLIX ships with these public default passwords; a tag replies to
// SET_PASSWORD with one of them before its memory can be read.
// Order matters: RvX_TRF7962A tries 0x7FFD6E5B first, which is the one real
// Tonies are locked with, so the common case succeeds on the first attempt.
static const uint32_t SLIX_DEFAULT_PASSWORDS[] = {0x7FFD6E5B, 0x0F0F0F0F, 0x00000000};

// The chip's FIFO is 12 bytes. Transfers larger than this must be chunked.
static const uint8_t TRF7962A_FIFO_SIZE = 12;

// Rx assembly buffer. A frame arrives over several FIFO drains, so this is
// larger than the FIFO itself: the 12-byte INVENTORY response plus headroom.
// Every response[] in this driver is declared at exactly this size, and
// read_packet_ bounds-checks against it -- keep them in step.
static const uint8_t TRF7962A_RX_BUFFER_SIZE = TRF7962A_FIFO_SIZE + 8;

// Value REG_CHIP_STATUS_CTRL must read back as after a correct init. Used as
// the chip-presence check -- see reset_chip_() in the .cpp.
static const uint8_t TRF7962A_CHIP_STATUS_EXPECTED = 0x21;

// Masked register init sequence, applied in order by init_registers_().
// Each entry is {register, read-modify-write mask, bits to set}. A mask of
// 0x00 means "write value blindly"; a non-zero mask means "read the register,
// AND with mask, OR in value" -- the reference driver's trf7962a_set_mask().
struct Trf7962aInitReg {
  uint8_t reg;
  uint8_t mask;
  uint8_t value;
};

// FOLLOW teddybox, NOT RvX, for the register init.
//
// All nine entries match teddybox's TRF7962A_INIT_REGS macro exactly, in its
// order. CHIP_STATUS_CTRL is teddybox's FIRST entry and was missing here
// until 2026-08-06 -- reset_chip_() hand-wrote it afterwards instead, so the
// boot path happened to end up right and every other reset path did not.
//
// RvX_TRF7962A has the RX timing entries commented out in its initRFID(), and
// an earlier version of this driver deleted them to match -- on the strength
// of framing errors (IRQ 0x04) that were really caused by the broken transmit
// path (the CS window bug in write_packet_). With Tx fixed, they are needed.
//
// Prefer teddybox where the two references disagree: it is ESP-IDF on this
// exact Toniebox hardware, whereas RvX is an Arduino driver for a different
// board. RX_WAIT_TIME in particular decides when the receiver starts
// sampling; without it the window closes early and a 12-byte inventory
// arrives as 1-2 byte fragments at the wrong alignment, e.g.
//   Runt frame (2 bytes): F8.3F / F0.C1 / FC.41   <- real UID bytes, shifted
//
// REG_ISO_CONTROL = 0x82, NOT the datasheet's generic reset default of 0x02.
// TRIED 0x02 on 2026-08-06 after misreading the datasheet's rx_crc_n bit
// (SLOS757G 6.14.1.1.2, Table 6-17: B7 1="No RX CRC (CRC not present in the
// response)", 0="RX CRC (CRC is present in the response)") as meaning "0x82
// tells the chip there's no CRC to check, contradicting read_packet_'s CRC
// verification." That reasoning was backwards. Section 6.7's receiver
// description clarifies what "RX CRC" actually does at the hardware level:
// with it ON (bit clear, 0x02), the chip's own framing logic checks AND
// REMOVES the CRC bytes before data ever reaches the FIFO -- the FIFO gets
// CRC-stripped "clean" data. With it OFF (bit set, 0x82, our value), the
// chip passes the CRC bytes through untouched, which is what read_packet_'s
// own CRC verify-then-strip logic (iso15693_crc() over the assembled frame,
// then `*length -= 2`) has always required and been tested against --
// including every previously-successful GET_RANDOM capture in this
// project's history. Flashing 0x02 measurably regressed a known-good setup
// (Flipper at RSSI 0x7E, previously reliable 5-byte GET_RANDOM captures) to
// consistent 2-byte runt frames -- the chip was now stripping the CRC bytes
// our driver still expected to find and verify. Reverted same day. If this
// driver is ever rewritten to trust the chip's own CRC checking instead of
// verifying independently, 0x02 plus deleting iso15693_crc()/the `-2` strip
// would be the correct pairing -- do not set one without the other.
//
// ALL NINE ENTRIES, ON EVERY RESET -- including the per-poll one. A previous
// version split this into a 4-register "poll subset" on the stated grounds
// that "teddybox's full table is called exactly once, from trf7962a_init() at
// startup, never from a poll path." That claim is simply wrong, and re-reading
// nfc.c settles it: nfc_reset() calls trf7962a_reset(), which calls
// trf7962a_init_regs() -- the full table -- and nfc_reset() runs from the poll
// path constantly (nfc.c: on every INVENTORY failure, on every SYSINFO
// failure, and once before EVERY password attempt in STATE_SEARCHING).
// teddybox re-applies all nine registers many times a second. RvX_TRF7962A is
// the driver that writes only four per loop, and it also leaves the RX timing
// registers at their reset defaults deliberately -- so the split produced a
// hybrid that matched neither reference: teddybox's values at boot, RvX's
// register set afterwards, with a CMD_SOFT_INIT in between wiping the
// difference. Apply the whole table wherever the chip is reset.
static const Trf7962aInitReg TRF7962A_INIT_REGS[] = {
    {REG_CHIP_STATUS_CTRL, 0x00, 0x21},          // 5V operation, RF field on
    {REG_ISO_CONTROL, 0x00, 0x82},               // ISO15693 high bitrate, 1-out-of-4, no CRC
    {REG_IRQ_MASK, 0x00, 0x3E},                  // unmask the error + FIFO IRQs
    {REG_MODULATOR_CTRL, 0x00, 0x21},            // OOK 100%, 6.78MHz sys clock
    {REG_TX_PULSE_LENGTH_CTRL, 0x00, 0x80},
    {REG_RX_NO_RESPONSE_WAIT_TIME, 0x00, 0x14},
    {REG_RX_WAIT_TIME, 0x00, 0x1F},
    {REG_RX_SPECIAL_SETTINGS, 0x0F, 0x40},       // keep low nibble, set bandwidth
    {REG_SPECIAL_FUNCTION_1, 0xFF, 0x10},        // OR in bit 4, preserve the rest
};

static const size_t TRF7962A_INIT_REGS_COUNT = sizeof(TRF7962A_INIT_REGS) / sizeof(TRF7962A_INIT_REGS[0]);

class TRF7962AListener {
 public:
  // uid is 8 bytes, LSB first, as returned by ISO15693 INVENTORY.
  virtual void on_tag_present(const std::vector<uint8_t> &uid) = 0;
  virtual void on_tag_removed() = 0;
};

// Bare SPI client used only for the read phase of a register access.
//
// THE REASON THIS CLASS EXISTS (sloa140b.pdf section 1.1): the TRF7962A
// clocks the command/address byte you send it on one edge, but clocks the
// data it returns on the *other* edge. Writes need SPI mode 0 and reads need
// SPI mode 1, within a single CS-low transaction.
//
// ESPHome's SPIDevice template bakes the mode in at compile time and
// SPIComponent keys its delegate map by SPIClient pointer, so one object can
// only ever own one mode -- calling set_mode() after spi_setup() has no
// effect. Registering a second, separate client on the same bus is what gets
// us two modes without dropping to raw spi_bus_add_device().
//
// Its CS pin is nullptr (SPIDelegate substitutes NullPin), because CS is
// driven manually by the owning component -- see cs_low_()/cs_high_() and
// the bus-lock warning on read_registers_().
class TRF7962AReadClient final : public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                       spi::CLOCK_PHASE_TRAILING, spi::DATA_RATE_4MHZ> {
 public:
  void setup_read_client(spi::SPIComponent *parent) {
    this->set_spi_parent(parent);
    this->set_cs_pin(nullptr);
    this->spi_setup();
  }
};

// ISR-safe edge counter for the IRQ pin (no vtables, no ESPHome core calls --
// see esphome/components/gpio/binary_sensor for the pattern this follows).
//
// >>> NOTHING READS THIS ANY MORE. It is wired up but not consulted. <<<
//
// It was built to patch a symptom of the old read_packet_, which drained the
// FIFO once and then exited on a clean RX_COMPLETE. A poll can miss an edge
// entirely -- FIFO_HIGH_OR_LOW pulses, then RX_COMPLETE, and the poll only
// ever sees the latter -- so the counter let that exit be second-guessed.
//
// read_packet_ no longer exits on any IRQ bit at all: it drains repeatedly
// and ends the frame on a quiet gap, exactly as teddybox does. A missed edge
// therefore costs nothing, because no decision hangs on having seen it. The
// counter is kept only because GPIO13 genuinely pulses and a future
// interrupt-driven Rx would want it. If that never happens, delete this class
// and the setup() call -- do NOT reintroduce it into the drain loop.
class Trf7962aIrqStore final {
 public:
  // attach_interrupt()/to_isr() are only declared on InternalGPIOPin (a
  // native MCU pin), not the base GPIOPin -- an I2C expander pin (PCF8574,
  // MCP23017, etc.) has no ISR to attach to. Not an issue on this board:
  // GPIO13 is a native ESP32-S3 pin. See esphome/components/gpio's
  // binary_sensor for how a component that must also support expander pins
  // handles the fallback; this driver doesn't need that generality.
  void setup(InternalGPIOPin *pin) {
    this->isr_pin_ = pin->to_isr();
    pin->attach_interrupt(&Trf7962aIrqStore::isr_, this, gpio::INTERRUPT_RISING_EDGE);
  }
  uint32_t count() const { return this->edge_count_; }

 protected:
  static void isr_(Trf7962aIrqStore *store) { store->edge_count_++; }

  ISRInternalGPIOPin isr_pin_;
  volatile uint32_t edge_count_{0};
};

class TRF7962AComponent final : public PollingComponent,
                                public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                      spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_4MHZ> {
 public:
  void set_irq_pin(InternalGPIOPin *pin) { this->irq_pin_ = pin; }
  void add_listener(TRF7962AListener *listener) { this->listeners_.push_back(listener); }

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Turns the RF field on/off without a full re-init.
  void set_field(bool enabled);

 protected:
  // CS is driven by hand rather than by the SPI delegates. Both clients are
  // registered with a null CS pin, so enable()/disable() only take and drop
  // the IDF bus lock; these two assert the real line around a whole
  // multi-client sequence. cs_pin_ is the component's own configured pin.
  void cs_low_();
  void cs_high_();

  void write_register_(uint8_t reg, uint8_t value);
  // Reads count bytes starting at reg. Handles the dual-mode and 0xFF-MOSI
  // quirks; this is the only correct way to read from this chip.
  void read_registers_(uint8_t reg, uint8_t *data, size_t count);
  uint8_t read_register_(uint8_t reg);
  void set_register_mask_(uint8_t reg, uint8_t mask, uint8_t value);
  void send_direct_command_(uint8_t command);

  uint8_t read_irq_status_();
  uint8_t read_fifo_status_();
  void read_fifo_(uint8_t *data, uint8_t length);

  // Applies TRF7962A_INIT_REGS in order via set_register_mask_() --
  // teddybox's trf7962a_init_regs(). Every reset path uses this; there is no
  // lighter subset, see the table's comment.
  void init_registers_();
  // teddybox's trf7962a_reset() minus the presence check: soft reset, FIFO
  // reset, full register init, IRQ clear. Runs at boot, once per poll, and
  // after a fatal Rx error. Leaves the RF field to the caller.
  void reinit_chip_();
  // reinit_chip_() plus the read-back presence check. Returns false if the
  // chip does not answer, which is the single most useful diagnostic this
  // driver has. Boot only -- the check logs loudly and is not poll-safe.
  bool reset_chip_();
  // Reads every register in TRF7962A_INIT_REGS back and caches the values for
  // dump_config(). Called at the end of setup(), because dump_config() is
  // only allowed to print values already determined during setup.
  void read_back_init_registers_();

  bool write_packet_(const uint8_t *data, uint8_t length);
  // Drains one response. Decodes the full IRQ byte (see the comment in the
  // .cpp): several codes with error bits set are normal mid-reception states
  // and must NOT be treated as failures. Strips the trailing CRC, so *length
  // is the payload length the ISO15693 layer expects.
  bool read_packet_(uint8_t *data, uint8_t *length, uint8_t expected);
  // Deduplicated Rx error log -- an idle field errors twice a second.
  void log_rx_error_(uint8_t irq, uint8_t length);
  // Full request/response exchange. Returns false on any error or timeout.
  bool transceive_(const uint8_t *tx, uint8_t tx_length, uint8_t *rx, uint8_t *rx_length, uint8_t expected);

  // Sends SLIX GET_RANDOM_NUMBER. The reference driver requires this to
  // succeed before it will attempt an inventory -- see nfc.c STATE_SEARCHING.
  bool get_random_(uint8_t *rand);
  // Unlocks a privacy-mode SLIX-L: GET_RANDOM, then SET_PASSWORD with the
  // password XORed against that random, sent back to back with nothing in
  // between -- see the .cpp for why the field is NOT re-armed there.
  bool set_password_(uint32_t password);
  // Runs one ISO15693 INVENTORY pass; returns true and fills uid if a tag answered.
  bool inventory_(std::vector<uint8_t> &uid);

  TRF7962AReadClient read_client_;
  // The configured CS line, moved out of SPIClient::cs_ in setup() so this
  // component owns it rather than the delegate.
  GPIOPin *cs_pin_{nullptr};
  InternalGPIOPin *irq_pin_{nullptr};
  // Edge count as of read_packet_'s last drain, so it can tell "no edge
  // happened" apart from "an edge happened but the register already moved
  // on to a later state" -- see Trf7962aIrqStore's comment.
  Trf7962aIrqStore irq_store_;
  std::vector<TRF7962AListener *> listeners_;
  std::vector<uint8_t> current_uid_;
  bool tag_present_{false};
  bool chip_ok_{false};
  // Init-table readback taken at the end of setup(), printed by dump_config().
  uint8_t init_readback_[TRF7962A_INIT_REGS_COUNT]{};
  uint8_t irq_status_{0};
  // Last full Rx IRQ byte logged, so a repeating fault prints once instead of
  // twice a second forever. 0 = nothing suppressed.
  uint8_t last_rx_error_{0};
  // Separate from last_rx_error_ on purpose: sharing one variable between the
  // read_packet_ and inventory_ log sites makes each reset the other's state,
  // so both print every poll and neither suppression works.
  bool no_response_logged_{false};
  // Consecutive inventory failures. Drives a periodic heartbeat rather than a
  // latching suppression flag, so the poll loop is never silently invisible.
  uint32_t fail_count_{0};
  // Rotates through SLIX_DEFAULT_PASSWORDS one per poll. Trying all three in
  // a single update() exceeds the 120ms component watchdog budget.
  uint8_t password_index_{0};
  // Consecutive failed inventory passes. A tag is only reported gone after
  // several misses -- see update() for why.
  uint8_t miss_count_{0};
};

class TagPresentTrigger final : public Trigger<std::vector<uint8_t>>, public TRF7962AListener {
 public:
  explicit TagPresentTrigger(TRF7962AComponent *parent) { parent->add_listener(this); }
  void on_tag_present(const std::vector<uint8_t> &uid) override { this->trigger(uid); }
  void on_tag_removed() override {}
};

class TagRemovedTrigger final : public Trigger<>, public TRF7962AListener {
 public:
  explicit TagRemovedTrigger(TRF7962AComponent *parent) { parent->add_listener(this); }
  void on_tag_present(const std::vector<uint8_t> &uid) override {}
  void on_tag_removed() override { this->trigger(); }
};

}  // namespace trf7962a
}  // namespace esphome
