import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from ..spi_sd_storage import SpiSdStorage

random_sd_image_ns = cg.esphome_ns.namespace("random_sd_image")
RandomSdImage = random_sd_image_ns.class_("RandomSdImage", cg.Component)

CONF_STORAGE_ID = "storage_id"
CONF_DIRECTORY = "directory"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RandomSdImage),
        cv.Required(CONF_STORAGE_ID): cv.use_id(SpiSdStorage),
        cv.Optional(CONF_DIRECTORY, default="/images"): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_STORAGE_ID])
    cg.add(var.set_storage(parent))
    cg.add(var.set_directory(config[CONF_DIRECTORY]))
