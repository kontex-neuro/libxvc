#include <spdlog/spdlog.h>
#include <validator.h>

#include <nlohmann/json-schema.hpp>

using nlohmann::json;
using nlohmann::json_schema::json_validator;

static auto camera_schema = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "required": ["id", "device_id", "name", "caps"],
    "properties": {
        "id": { "type": "integer" },
        "device_id": { "type": "string" },
        "name": { "type": "string" },
        "caps": {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["media_type", "width", "height", "framerate"],
                "properties": {
                    "media_type": { "type": "string" },
                    "format": { "type": "string" },
                    "width": { "type": "integer", "minimum": 1 },
                    "height": { "type": "integer", "minimum": 1 },
                    "framerate": {
                        "type": "string",
                        "pattern": "^[0-9]+/[0-9]+$"
                    }
                }
            }
        }
    }
}
)"_json;

std::expected<void, std::string> validate_camera(const nlohmann::json &json)
{
    static auto validator = [] -> json_validator {
        json_validator v;
        try {
            v.set_root_schema(camera_schema);
        } catch (const std::exception &e) {
            spdlog::error("Validation of schema failed: {}", e.what());
        }
        return v;
    }();

    try {
        validator.validate(json);
    } catch (const std::exception &e) {
        return std::unexpected(e.what());
    }
    return {};
}
