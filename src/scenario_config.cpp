#include "scenario.hpp"

#include <charconv>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace openwar {
namespace {

std::string_view trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

}  // namespace

ProceduralSettings read_procedural_settings(const std::filesystem::path& path)
{
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes > 64 * 1024) {
        throw std::invalid_argument("Cannot read scenario file or file exceeds 64 KiB: " + path.string());
    }
    std::ifstream input(path);
    if (!input) {
        throw std::invalid_argument("Cannot open scenario file: " + path.string());
    }
    static const std::map<std::string_view, std::uint32_t ProceduralSettings::*> fields{
        {"width", &ProceduralSettings::width}, {"height", &ProceduralSettings::height},
        {"sector_size_m", &ProceduralSettings::sector_size_m}, {"crossings", &ProceduralSettings::crossings},
        {"forest_percent", &ProceduralSettings::forest_percent}, {"water_percent", &ProceduralSettings::water_percent},
        {"west_armor", &ProceduralSettings::west_armor}, {"east_armor", &ProceduralSettings::east_armor},
        {"reserve_armor", &ProceduralSettings::reserve_armor}, {"scout_flights", &ProceduralSettings::scout_flights},
        {"oil", &ProceduralSettings::oil}, {"materials", &ProceduralSettings::materials},
        {"ammunition", &ProceduralSettings::ammunition}, {"hold_minutes", &ProceduralSettings::hold_minutes},
        {"time_limit_minutes", &ProceduralSettings::time_limit_minutes},
        {"require_relay", &ProceduralSettings::require_relay}, {"capitulation", &ProceduralSettings::capitulation},
    };
    ProceduralSettings settings;
    std::set<std::string> seen;
    std::size_t line_number = 0;
    const auto fail = [&](const std::string& message) {
        throw std::invalid_argument(path.string() + ":" + std::to_string(line_number) + ": " + message);
    };
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        const auto entry = trim(std::string_view(line).substr(0, line.find('#')));
        if (entry.empty()) {
            continue;
        }
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            fail("Expected key = unsigned integer");
        }
        const auto key = trim(entry.substr(0, equals));
        const auto number = trim(entry.substr(equals + 1));
        if (number.empty()) {
            fail("Expected unsigned integer for " + std::string(key));
        }
        std::uint64_t value = 0;
        const auto result = std::from_chars(number.data(), number.data() + number.size(), value);
        if (result.ec != std::errc{} || result.ptr != number.data() + number.size()) {
            fail("Expected unsigned integer for " + std::string(key));
        }
        if (!seen.emplace(key).second) {
            fail("Duplicate key: " + std::string(key));
        }
        if (key == "schema_version" || key == "generator_version") {
            const auto version = key == "schema_version" ? 1u : procedural_generator_version;
            if (value != version) {
                fail("Unsupported " + std::string(key));
            }
        } else if (key == "seed") {
            settings.seed = value;
        } else if (const auto field = fields.find(key); field != fields.end()) {
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                fail("Value exceeds 32 bits: " + std::string(key));
            }
            settings.*field->second = static_cast<std::uint32_t>(value);
        } else {
            fail("Unknown key: " + std::string(key));
        }
    }
    if (!input.eof()) {
        fail("Failed to read scenario file");
    }
    if (!seen.contains("schema_version")) {
        fail("Missing schema_version");
    }
    return settings;
}

}  // namespace openwar
