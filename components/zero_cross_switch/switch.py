import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import esp32, sensor, switch, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_FREQUENCY,
    STATE_CLASS_MEASUREMENT,
    UNIT_HERTZ,
)

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["sensor"]

CONF_CHIP_TYPE = "chip_type"
CONF_CHIP_ID = "chip_id"
CONF_RELAY_PIN = "relay_pin"
CONF_ZX_PIN = "zx_pin"
CONF_ZX_EDGE = "zx_edge"
CONF_ON_TIME_US = "on_time_us"
CONF_OFF_TIME_US = "off_time_us"
CONF_MIN_FREQUENCY = "min_frequency"
CONF_MAX_FREQUENCY = "max_frequency"
CONF_EDGES_NEEDED = "edges_needed"
CONF_ZX_EDGES_PER_PERIOD = "zx_edges_per_period"
CONF_ISR_OFFSET_US = "isr_offset_us"
CONF_FREQUENCY = "frequency"

CHIP_NONE = "none"
CHIP_BL0942 = "bl0942"

zero_cross_switch_ns = cg.esphome_ns.namespace("zero_cross_switch")
ZeroCrossSwitch = zero_cross_switch_ns.class_("ZeroCrossSwitch", switch.Switch, cg.Component)
Bl0942ZxChip = zero_cross_switch_ns.class_("Bl0942ZxChip", uart.UARTDevice)

gpio_ns = cg.esphome_ns.namespace("gpio")
ZX_EDGES = {
    "RISING": gpio_ns.INTERRUPT_RISING_EDGE,
    "FALLING": gpio_ns.INTERRUPT_FALLING_EDGE,
    "ANY": gpio_ns.INTERRUPT_ANY_EDGE,
}

if hasattr(switch, "switch_schema"):
    _SWITCH_SCHEMA = switch.switch_schema(ZeroCrossSwitch)
else:  # older ESPHome
    _SWITCH_SCHEMA = switch.SWITCH_SCHEMA.extend({cv.GenerateID(): cv.declare_id(ZeroCrossSwitch)})

# Options every chip_type shares.
BASE_SCHEMA = (
    _SWITCH_SCHEMA.extend(
        {
            cv.Required(CONF_RELAY_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_ZX_PIN): pins.internal_gpio_input_pin_schema,
            # Which edge of the zero-cross pin marks ONE zero crossing. Decide with the zx-probe results.
            cv.Required(CONF_ZX_EDGE): cv.enum(ZX_EDGES, upper=True),
            # Mechanical relay times. The defaults are the numbers of the Shelly Plug M Gen3 stock firmware (not measured here).
            # What ONE edge of zx_edge stands for: 2 = one edge per zero crossing (half period apart), 1 = one edge per mains
            # period (like the stock Shelly firmware, which uses only the falling edge of a signal that toggles at every crossing).
            cv.Optional(CONF_ZX_EDGES_PER_PERIOD, default=2): cv.one_of(1, 2, int=True),
            cv.Optional(CONF_ON_TIME_US, default=6500): cv.int_range(min=0, max=50000),
            cv.Optional(CONF_OFF_TIME_US, default=7800): cv.int_range(min=0, max=50000),
            cv.Optional(CONF_MIN_FREQUENCY, default=45.0): cv.float_range(min=10.0, max=100.0),
            cv.Optional(CONF_MAX_FREQUENCY, default=65.0): cv.float_range(min=10.0, max=100.0),
            cv.Optional(CONF_EDGES_NEEDED, default=8): cv.int_range(min=3, max=100),
            cv.Optional(CONF_ISR_OFFSET_US, default=0): cv.int_range(min=-2000, max=2000),
            cv.Optional(CONF_FREQUENCY): sensor.sensor_schema(
                unit_of_measurement=UNIT_HERTZ,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_FREQUENCY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

# Chip specific options: required, optional or not allowed per chip_type (an unknown option raises "not allowed here").
CONFIG_SCHEMA = cv.typed_schema(
    {
        # The hardware already provides a clean zero-cross signal on zx_pin.
        CHIP_NONE: BASE_SCHEMA,
        # BL0942 (e.g. Shelly Plug M Gen3): its CF/ZX pin carries the zero crossing only after register OT_FUNX = 2 was
        # written over the UART of the bl0942 sensor.
        CHIP_BL0942: BASE_SCHEMA.extend(
            {cv.GenerateID(CONF_CHIP_ID): cv.declare_id(Bl0942ZxChip)}
        ).extend(uart.UART_DEVICE_SCHEMA),
    },
    key=CONF_CHIP_TYPE,
    default_type=CHIP_NONE,
    lower=True,
)


def _validate(config):
    if config[CONF_MIN_FREQUENCY] >= config[CONF_MAX_FREQUENCY]:
        raise cv.Invalid("min_frequency must be lower than max_frequency")
    if str(config[CONF_ZX_EDGE]) == "ANY" and config[CONF_ZX_EDGES_PER_PERIOD] != 2:
        raise cv.Invalid("zx_edge: ANY marks every zero crossing, so zx_edges_per_period must be 2")
    return config


FINAL_VALIDATE_SCHEMA = cv.Schema(_validate, extra=cv.ALLOW_EXTRA)


async def to_code(config):
    if hasattr(switch, "new_switch"):
        var = await switch.new_switch(config)
    else:  # older ESPHome
        var = cg.new_Pvariable(config[CONF_ID])
        await switch.register_switch(var, config)
    await cg.register_component(var, config)

    relay = await cg.gpio_pin_expression(config[CONF_RELAY_PIN])
    cg.add(var.set_relay_pin(relay))
    zx = await cg.gpio_pin_expression(config[CONF_ZX_PIN])
    cg.add(var.set_zx_pin(zx))
    cg.add(var.set_zx_edge(config[CONF_ZX_EDGE]))
    cg.add(var.set_on_time_us(config[CONF_ON_TIME_US]))
    cg.add(var.set_off_time_us(config[CONF_OFF_TIME_US]))
    cg.add(var.set_min_frequency(config[CONF_MIN_FREQUENCY]))
    cg.add(var.set_max_frequency(config[CONF_MAX_FREQUENCY]))
    cg.add(var.set_edges_needed(config[CONF_EDGES_NEEDED]))
    cg.add(var.set_edges_per_period(config[CONF_ZX_EDGES_PER_PERIOD]))
    cg.add(var.set_isr_offset_us(config[CONF_ISR_OFFSET_US]))

    if config.get(CONF_CHIP_TYPE, CHIP_NONE) == CHIP_BL0942:
        # The chip files are compiled only with this define (they need the uart component, which is part of the config here).
        cg.add_define("USE_ZERO_CROSS_BL0942")
        cg.add_global(cg.RawStatement('#include "esphome/components/zero_cross_switch/bl0942.h"'))
        chip = cg.new_Pvariable(config[CONF_CHIP_ID])
        await uart.register_uart_device(chip, config)
        cg.add(var.set_chip(chip))

    if CONF_FREQUENCY in config:
        sens = await sensor.new_sensor(config[CONF_FREQUENCY])
        cg.add(var.set_frequency_sensor(sens))

    # The hardware timer driver is a separate IDF component; newer ESPHome only builds components that are requested.
    if hasattr(esp32, "include_builtin_idf_component"):
        for component in ("esp_driver_gptimer", "esp_driver_gpio", "esp_timer"):
            esp32.include_builtin_idf_component(component)
