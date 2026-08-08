import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c
from esphome.const import CONF_ID

CODEOWNERS = ["@chunckus-aurelius"]
DEPENDENCIES = ["i2c"]
MULTI_CONF = True

dac3100_ns = cg.esphome_ns.namespace("dac3100")
DAC3100Component = dac3100_ns.class_("DAC3100Component", cg.Component, i2c.I2CDevice)

# TLV320DAC3100 default I2C address per datasheet (7-bit 0x18).
DAC3100_I2C_ADDR = 0x18

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DAC3100Component),
    }
).extend(i2c.i2c_device_schema(DAC3100_I2C_ADDR))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)
