import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from . import (
    CONF_CAPS_LOCK,
    CONF_NUM_LOCK,
    CONF_SCROLL_LOCK,
    PS2Keyboard,
)

DEPENDENCIES = ["ps2_keyboard"]

CONF_PS2_KEYBOARD_ID = "ps2_keyboard_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_PS2_KEYBOARD_ID): cv.use_id(PS2Keyboard),
        cv.Optional(CONF_CAPS_LOCK): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_NUM_LOCK): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_SCROLL_LOCK): binary_sensor.binary_sensor_schema(),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_PS2_KEYBOARD_ID])

    if CONF_CAPS_LOCK in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_CAPS_LOCK])
        cg.add(parent.set_caps_lock_sensor(sens))
    if CONF_NUM_LOCK in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_NUM_LOCK])
        cg.add(parent.set_num_lock_sensor(sens))
    if CONF_SCROLL_LOCK in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_SCROLL_LOCK])
        cg.add(parent.set_scroll_lock_sensor(sens))
