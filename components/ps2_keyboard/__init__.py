from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_BYTES,
    CONF_CLK_PIN,
    CONF_DATA_PIN,
    CONF_DELAY,
    CONF_ID,
    CONF_KEY,
    CONF_TEXT,
    CONF_TRIGGER_ID,
)

CONF_KEYS = "keys"

CODEOWNERS = ["@miles"]
AUTO_LOAD = ["binary_sensor"]
MULTI_CONF = True

ps2_keyboard_ns = cg.esphome_ns.namespace("ps2_keyboard")
PS2Keyboard = ps2_keyboard_ns.class_("PS2Keyboard", cg.Component)

# Actions
PrintAction = ps2_keyboard_ns.class_(
    "PrintAction", automation.Action, cg.Parented.template(PS2Keyboard)
)
StrokeAction = ps2_keyboard_ns.class_(
    "StrokeAction", automation.Action, cg.Parented.template(PS2Keyboard)
)
PressAction = ps2_keyboard_ns.class_(
    "PressAction", automation.Action, cg.Parented.template(PS2Keyboard)
)
ReleaseAction = ps2_keyboard_ns.class_(
    "ReleaseAction", automation.Action, cg.Parented.template(PS2Keyboard)
)
CombinationAction = ps2_keyboard_ns.class_(
    "CombinationAction", automation.Action, cg.Parented.template(PS2Keyboard)
)
SendRawAction = ps2_keyboard_ns.class_(
    "SendRawAction", automation.Action, cg.Parented.template(PS2Keyboard)
)

# Triggers
LEDChangeTrigger = ps2_keyboard_ns.class_(
    "LEDChangeTrigger", automation.Trigger.template(cg.bool_, cg.bool_, cg.bool_)
)
HostResetTrigger = ps2_keyboard_ns.class_(
    "HostResetTrigger", automation.Trigger.template()
)

CONF_TASK_PRIORITY = "task_priority"
CONF_TASK_CORE = "task_core"
CONF_CAPS_LOCK = "caps_lock"
CONF_NUM_LOCK = "num_lock"
CONF_SCROLL_LOCK = "scroll_lock"
CONF_ON_LED_CHANGE = "on_led_change"
CONF_ON_HOST_RESET = "on_host_reset"


def validate_keys(value):
    if isinstance(value, list):
        return "+".join(str(x) for x in value)
    return str(value)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PS2Keyboard),
            cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_DATA_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_TASK_PRIORITY, default=10): cv.int_range(min=1, max=25),
            cv.Optional(CONF_TASK_CORE, default=1): cv.int_range(min=-1, max=1),
            cv.Optional(CONF_CAPS_LOCK): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_NUM_LOCK): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_SCROLL_LOCK): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_ON_LED_CHANGE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(LEDChangeTrigger),
                }
            ),
            cv.Optional(CONF_ON_HOST_RESET): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(HostResetTrigger),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    clk_pin = await cg.gpio_pin_expression(config[CONF_CLK_PIN])
    cg.add(var.set_clk_pin(clk_pin))

    data_pin = await cg.gpio_pin_expression(config[CONF_DATA_PIN])
    cg.add(var.set_data_pin(data_pin))

    cg.add(var.set_task_priority(config[CONF_TASK_PRIORITY]))
    cg.add(var.set_task_core(config[CONF_TASK_CORE]))

    if CONF_CAPS_LOCK in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_CAPS_LOCK])
        cg.add(var.set_caps_lock_sensor(sens))
    if CONF_NUM_LOCK in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_NUM_LOCK])
        cg.add(var.set_num_lock_sensor(sens))
    if CONF_SCROLL_LOCK in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_SCROLL_LOCK])
        cg.add(var.set_scroll_lock_sensor(sens))

    for conf in config.get(CONF_ON_LED_CHANGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.bool_, "caps"), (cg.bool_, "num"), (cg.bool_, "scroll")], conf
        )

    for conf in config.get(CONF_ON_HOST_RESET, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


@automation.register_action(
    "ps2_keyboard.print",
    PrintAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(PS2Keyboard),
            cv.Required(CONF_TEXT): cv.templatable(cv.string),
            cv.Optional(CONF_DELAY): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        },
        key=CONF_TEXT,
    ),
)
async def ps2_keyboard_print_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    text_ = await cg.templatable(config[CONF_TEXT], args, cg.std_string)
    cg.add(var.set_text(text_))
    if CONF_DELAY in config:
        delay_ = await cg.templatable(config[CONF_DELAY], args, cg.uint32)
        cg.add(var.set_delay(delay_))
    return var


@automation.register_action(
    "ps2_keyboard.stroke",
    StrokeAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(PS2Keyboard),
            cv.Required(CONF_KEY): cv.templatable(cv.string),
            cv.Optional(CONF_DELAY): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        },
        key=CONF_KEY,
    ),
)
async def ps2_keyboard_stroke_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    key_ = await cg.templatable(config[CONF_KEY], args, cg.std_string)
    cg.add(var.set_key(key_))
    if CONF_DELAY in config:
        delay_ = await cg.templatable(config[CONF_DELAY], args, cg.uint32)
        cg.add(var.set_delay(delay_))
    return var


@automation.register_action(
    "ps2_keyboard.press",
    PressAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(PS2Keyboard),
            cv.Required(CONF_KEY): cv.templatable(cv.string),
        },
        key=CONF_KEY,
    ),
)
async def ps2_keyboard_press_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    key_ = await cg.templatable(config[CONF_KEY], args, cg.std_string)
    cg.add(var.set_key(key_))
    return var


@automation.register_action(
    "ps2_keyboard.release",
    ReleaseAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(PS2Keyboard),
            cv.Required(CONF_KEY): cv.templatable(cv.string),
        },
        key=CONF_KEY,
    ),
)
async def ps2_keyboard_release_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    key_ = await cg.templatable(config[CONF_KEY], args, cg.std_string)
    cg.add(var.set_key(key_))
    return var


@automation.register_action(
    "ps2_keyboard.combination",
    CombinationAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(PS2Keyboard),
            cv.Required(CONF_KEYS): cv.templatable(validate_keys),
            cv.Optional(CONF_DELAY): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        },
        key=CONF_KEYS,
    ),
)
async def ps2_keyboard_combination_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    keys_ = await cg.templatable(config[CONF_KEYS], args, cg.std_string)
    cg.add(var.set_keys(keys_))
    if CONF_DELAY in config:
        delay_ = await cg.templatable(config[CONF_DELAY], args, cg.uint32)
        cg.add(var.set_delay(delay_))
    return var


@automation.register_action(
    "ps2_keyboard.send_raw",
    SendRawAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(PS2Keyboard),
            cv.Required(CONF_BYTES): cv.templatable(cv.ensure_list(cv.hex_uint8_t)),
        },
        key=CONF_BYTES,
    ),
)
async def ps2_keyboard_send_raw_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if cg.is_template(config[CONF_BYTES]):
        bytes_ = await cg.templatable(
            config[CONF_BYTES], args, cg.std_vector.template(cg.uint8)
        )
        cg.add(var.set_bytes(bytes_))
    else:
        cg.add(var.set_bytes(config[CONF_BYTES]))
    return var
