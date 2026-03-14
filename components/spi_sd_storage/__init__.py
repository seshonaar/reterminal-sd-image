import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

spi_sd_storage_ns = cg.esphome_ns.namespace("spi_sd_storage")
SpiSdStorage = spi_sd_storage_ns.class_("SpiSdStorage", cg.Component)

CONF_CLK_PIN = "clk_pin"
CONF_MISO_PIN = "miso_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_CS_PIN = "cs_pin"
CONF_ENABLE_PIN = "enable_pin"
CONF_DETECT_PIN = "detect_pin"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SpiSdStorage),
        cv.Required(CONF_CLK_PIN): cv.int_,
        cv.Required(CONF_MISO_PIN): cv.int_,
        cv.Required(CONF_MOSI_PIN): cv.int_,
        cv.Required(CONF_CS_PIN): cv.int_,
        cv.Optional(CONF_ENABLE_PIN): cv.int_,
        cv.Optional(CONF_DETECT_PIN): cv.int_,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_clk_pin(config[CONF_CLK_PIN]))
    cg.add(var.set_miso_pin(config[CONF_MISO_PIN]))
    cg.add(var.set_mosi_pin(config[CONF_MOSI_PIN]))
    cg.add(var.set_cs_pin(config[CONF_CS_PIN]))
    if CONF_ENABLE_PIN in config:
        cg.add(var.set_enable_pin(config[CONF_ENABLE_PIN]))
    if CONF_DETECT_PIN in config:
        cg.add(var.set_detect_pin(config[CONF_DETECT_PIN]))
