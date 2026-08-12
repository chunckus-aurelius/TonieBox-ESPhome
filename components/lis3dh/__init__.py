import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_ID,
    CONF_RANGE,
    CONF_THRESHOLD,
    CONF_TRIGGER_ID,
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
LIS3DHClickTrigger = lis3dh_ns.class_(
    "LIS3DHClickTrigger", automation.Trigger.template(cg.uint8)
)

CONF_CLICK = "click"
CONF_DOUBLE = "double"
CONF_TIME_LIMIT = "time_limit"
CONF_TIME_LATENCY = "time_latency"
CONF_TIME_WINDOW = "time_window"
CONF_ON_CLICK = "on_click"

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


# Timing registers are counted in ODR periods, not milliseconds, so at the
# 100Hz set in setup() one count is 10ms. Defaults are a starting point for
# tuning and nothing more -- there is no reference driver to inherit known-good
# values from. Expect to move them; see "Tap to skip" in docs/hardware-notes.md.
CLICK_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_DOUBLE, default=False): cv.boolean,
        cv.Optional(CONF_THRESHOLD, default=40): cv.int_range(min=1, max=127),
        cv.Optional(CONF_TIME_LIMIT, default=10): cv.int_range(min=0, max=255),
        cv.Optional(CONF_TIME_LATENCY, default=20): cv.int_range(min=0, max=255),
        cv.Optional(CONF_TIME_WINDOW, default=100): cv.int_range(min=0, max=255),
        cv.Optional(CONF_ON_CLICK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(LIS3DHClickTrigger)}
        ),
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LIS3DHComponent),
            cv.Optional(CONF_RANGE, default=2): cv.enum(RANGES, int=True),
            cv.Optional(CONF_CLICK): CLICK_SCHEMA,
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

    if CONF_CLICK in config:
        click = config[CONF_CLICK]
        cg.add(var.set_click_enabled(True))
        cg.add(var.set_click_double(click[CONF_DOUBLE]))
        cg.add(var.set_click_threshold(click[CONF_THRESHOLD]))
        cg.add(var.set_click_time_limit(click[CONF_TIME_LIMIT]))
        cg.add(var.set_click_time_latency(click[CONF_TIME_LATENCY]))
        cg.add(var.set_click_time_window(click[CONF_TIME_WINDOW]))
        for conf in click.get(CONF_ON_CLICK, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(
                trigger, [(cg.uint8, "src")], conf
            )
