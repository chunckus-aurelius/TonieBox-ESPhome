from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import nfc, spi
from esphome.components.spi import CONF_SPI_ID
import esphome.config_validation as cv
from esphome.const import (
    CONF_CS_PIN,
    CONF_DATA_RATE,
    CONF_ID,
    CONF_IRQ_PIN,
    CONF_ON_TAG,
    CONF_ON_TAG_REMOVED,
)
from esphome.types import ConfigType

AUTO_LOAD = ["nfc"]
CODEOWNERS = ["@chunckus-aurelius"]
DEPENDENCIES = ["spi"]
DOMAIN = "trf7962a_nfc"
MULTI_CONF = True

CONF_RF_POWER = "rf_power"
CONF_VIN = "vin"

trf7962a_ns = cg.esphome_ns.namespace("trf7962a_nfc")
TRF7962A = trf7962a_ns.class_("TRF7962A", nfc.Nfcc, cg.PollingComponent, spi.SPIDevice)

RF_POWER = {"FULL": False, "HALF": True}
VIN = {"3.3V": False, "5V": True}

_TAG_ARGS = [(cg.std_string, "x"), (nfc.NfcTagConstRef, "tag")]
_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(CONF_ON_TAG, "add_on_tag_callback", _TAG_ARGS),
    automation.CallbackAutomation(
        CONF_ON_TAG_REMOVED, "add_on_tag_removed_callback", _TAG_ARGS
    ),
)

# The chip needs a different SPI clock phase for reads than for writes while
# CS stays low, so the component drives CS itself and SPI mode isn't
# configurable.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TRF7962A),
        cv.GenerateID(CONF_SPI_ID): cv.use_id(spi.SPIComponent),
        cv.Required(CONF_CS_PIN): pins.gpio_output_pin_schema,
        cv.Required(CONF_IRQ_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_DATA_RATE, default="2MHz"): spi.SPI_DATA_RATE_SCHEMA,
        cv.Optional(CONF_VIN, default="5V"): cv.enum(VIN),
        cv.Optional(CONF_RF_POWER, default="FULL"): cv.enum(RF_POWER, upper=True),
        cv.Optional(CONF_ON_TAG): automation.validate_automation({}),
        cv.Optional(CONF_ON_TAG_REMOVED): automation.validate_automation({}),
    }
).extend(cv.polling_component_schema("500ms"))

FINAL_VALIDATE_SCHEMA = spi.final_validate_device_schema(
    "trf7962a_nfc", require_miso=True, require_mosi=True
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_spi_parent(await cg.get_variable(config[CONF_SPI_ID])))
    cg.add(var.set_data_rate(config[CONF_DATA_RATE]))
    cg.add(var.set_select_pin(await cg.gpio_pin_expression(config[CONF_CS_PIN])))
    cg.add(var.set_irq_pin(await cg.gpio_pin_expression(config[CONF_IRQ_PIN])))
    cg.add(var.set_supply_5v(config[CONF_VIN]))
    cg.add(var.set_half_power(config[CONF_RF_POWER]))
    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)
