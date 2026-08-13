import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.pins as pins
from esphome import automation
from esphome.components import spi
from esphome.const import CONF_ID, CONF_IRQ_PIN, CONF_TRIGGER_ID

CODEOWNERS = ["@chunckus-aurelius"]
DEPENDENCIES = ["spi"]
MULTI_CONF = True

trf7962a_ns = cg.esphome_ns.namespace("trf7962a")
TRF7962AComponent = trf7962a_ns.class_(
    "TRF7962AComponent", cg.PollingComponent, spi.SPIDevice
)

TagPresentTrigger = trf7962a_ns.class_(
    "TagPresentTrigger", automation.Trigger.template(cg.std_vector.template(cg.uint8))
)
TagRemovedTrigger = trf7962a_ns.class_(
    "TagRemovedTrigger", automation.Trigger.template()
)

CONF_ON_TAG_PRESENT = "on_tag_present"
CONF_ON_TAG_REMOVED = "on_tag_removed"
CONF_REMOVAL_DEBOUNCE = "removal_debounce"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TRF7962AComponent),
            # The driver configures this pin and never samples it -- reception
            # ends on a quiet gap, not an IRQ edge. Kept in the schema because
            # dropping a key breaks every existing config, and kept as
            # internal_gpio_input_pin_schema (not gpio_input_pin_schema)
            # because an interrupt-driven Rx would need a native MCU pin to
            # attach to, and widening later is easier than narrowing.
            cv.Optional(CONF_IRQ_PIN): cv.All(
                cv.only_on_esp32, pins.internal_gpio_input_pin_schema
            ),
            # How long a tag must stay unseen before on_tag_removed fires.
            # A knock, or a figure shifting a few mm, loses reads for a poll
            # or two, and flapping the media player is far worse than
            # reacting late. Rounded down to whole polls, minimum one.
            cv.Optional(
                CONF_REMOVAL_DEBOUNCE, default="1500ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_TAG_PRESENT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(TagPresentTrigger),
                }
            ),
            cv.Optional(CONF_ON_TAG_REMOVED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(TagRemovedTrigger),
                }
            ),
        }
    )
    .extend(cv.polling_component_schema("500ms"))
    .extend(spi.spi_device_schema(cs_pin_required=True))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)

    cg.add(var.set_removal_debounce(config[CONF_REMOVAL_DEBOUNCE]))

    if CONF_IRQ_PIN in config:
        irq_pin = await cg.gpio_pin_expression(config[CONF_IRQ_PIN])
        cg.add(var.set_irq_pin(irq_pin))

    for conf in config.get(CONF_ON_TAG_PRESENT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_vector.template(cg.uint8), "x")], conf
        )

    for conf in config.get(CONF_ON_TAG_REMOVED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
