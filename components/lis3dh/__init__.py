import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_ID,
    CONF_RANGE,
    CONF_X,
    CONF_Y,
    DEVICE_CLASS_EMPTY,
    STATE_CLASS_MEASUREMENT,
    UNIT_G,
)

CODEOWNERS = ["@chunckus-aurelius"]
DEPENDENCIES = ["i2c"]
MULTI_CONF = True

lis3dh_ns = cg.esphome_ns.namespace("lis3dh")
LIS3DHComponent = lis3dh_ns.class_(
    "LIS3DHComponent", cg.PollingComponent, i2c.I2CDevice
)
Lis3dhRange = lis3dh_ns.enum("Lis3dhRange")

# teddybox lis3dh_regs.h defines LIS3DH_ADDR 0x32, which is the 8-bit
# (shifted) form. ESPHome's i2c layer wants the 7-bit address: 0x32 >> 1.
# SA0 high would be 0x19; SA0 low is 0x18 -- but 0x18 collides with the
# DAC3100 on this board, and teddybox's 0x32 confirms SA0 is high here.
LIS3DH_I2C_ADDR = 0x19

# CONF_RANGE/CONF_X/CONF_Y come from esphome.const. CONF_Z is defined here
# because const.py has no CONF_Z -- do not "fix" this into an import.
CONF_Z = "z"

RANGES = {
    2: Lis3dhRange.RANGE_2G,
    4: Lis3dhRange.RANGE_4G,
    8: Lis3dhRange.RANGE_8G,
    16: Lis3dhRange.RANGE_16G,
}


def _accel_sensor_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_G,
        device_class=DEVICE_CLASS_EMPTY,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=3,
    )


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LIS3DHComponent),
            cv.Optional(CONF_RANGE, default=2): cv.enum(RANGES, int=True),
            cv.Optional(CONF_X): _accel_sensor_schema(),
            cv.Optional(CONF_Y): _accel_sensor_schema(),
            cv.Optional(CONF_Z): _accel_sensor_schema(),
        }
    )
    .extend(cv.polling_component_schema("100ms"))
    .extend(i2c.i2c_device_schema(LIS3DH_I2C_ADDR))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_range(config[CONF_RANGE]))

    if CONF_X in config:
        sens = await sensor.new_sensor(config[CONF_X])
        cg.add(var.set_x_sensor(sens))
    if CONF_Y in config:
        sens = await sensor.new_sensor(config[CONF_Y])
        cg.add(var.set_y_sensor(sens))
    if CONF_Z in config:
        sens = await sensor.new_sensor(config[CONF_Z])
        cg.add(var.set_z_sensor(sens))
