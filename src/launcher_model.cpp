#include "launcher_model.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <stdexcept>

namespace openwar {
namespace fs = std::filesystem;
namespace {

bool read_provenance_string(std::istream& input, std::string& result)
{
    if (input.get() != '"')
        return false;
    const auto hex_code = [&]() -> std::optional<std::uint32_t> {
        std::array<char, 4> hex{};
        if (!input.read(hex.data(), hex.size()))
            return {};
        std::uint32_t code{};
        const auto end = hex.data() + hex.size();
        const auto parsed = std::from_chars(hex.data(), end, code, 16);
        return parsed.ec == std::errc{} && parsed.ptr == end ? std::optional{code} : std::nullopt;
    };
    for (char ch; input.get(ch);) {
        if (ch == '"')
            return true;
        if (ch != '\\') {
            result.push_back(ch);
            continue;
        }
        if (!input.get(ch))
            break;
        if (ch != 'u') {
            result.push_back(ch == '"' || ch == '\\' || ch == '/' ? ch : ' ');
            continue;
        }
        auto code = hex_code();
        if (!code)
            return false;
        if (*code >= 0xd800 && *code <= 0xdbff) {
            if (input.get() != '\\' || input.get() != 'u')
                return false;
            const auto low = hex_code();
            if (!low || *low < 0xdc00 || *low > 0xdfff)
                return false;
            *code = 0x10000 + ((*code - 0xd800) << 10) + (*low - 0xdc00);
        } else if (*code >= 0xdc00 && *code <= 0xdfff) {
            return false;
        }
        if (*code < 0x80) {
            result.push_back(*code < 32 ? ' ' : static_cast<char>(*code));
            continue;
        }
        const int bytes = *code < 0x800 ? 2 : *code < 0x10000 ? 3 : 4;
        constexpr std::array prefixes{0, 0, 0xc0, 0xe0, 0xf0};
        result.push_back(static_cast<char>(prefixes[bytes] | (*code >> (6 * (bytes - 1)))));
        for (int byte = bytes - 2; byte >= 0; --byte)
            result.push_back(static_cast<char>(0x80 | ((*code >> (6 * byte)) & 0x3f)));
    }
    return false;
}

// The importer writes a JSON object. Only a few top-level display fields are needed.
std::map<std::string, std::string> provenance_fields(std::string_view provenance)
{
    std::istringstream input{std::string(provenance)};
    std::map<std::string, std::string> fields;
    if ((input >> std::ws).get() != '{')
        return fields;
    int depth = 1;
    for (char ch; depth > 0 && input >> ch;) {
        if (ch == '{' || ch == '[') {
            ++depth;
        } else if (ch == '}' || ch == ']') {
            --depth;
        } else if (ch == '"') {
            input.unget();
            std::string token;
            if (!read_provenance_string(input, token))
                break;
            if (depth != 1 || (token != "name" && token != "epsg" && token != "vertical_datum" && token != "origin_m") ||
                (input >> std::ws).peek() != ':')
                continue;
            input.get();
            const auto kind = (input >> std::ws).peek();
            std::string value;
            if (kind == '"') {
                if (!read_provenance_string(input, value))
                    break;
            } else if (kind == '[' && token == "origin_m") {
                input.get();
                std::getline(input, value, ']');
            } else if (token == "epsg") {
                std::uint32_t epsg{};
                if (input >> epsg)
                    value = std::to_string(epsg);
            }
            if (value.size() <= 160 && !value.empty())
                fields.try_emplace(std::move(token), std::move(value));
        }
    }
    return fields;
}

std::vector<std::string> world_dimensions(const GeographicHeader& header)
{
    const auto scale = header.sector_mm / 1000;
    std::ostringstream extent;
    extent << static_cast<double>(header.columns * scale) / 1000 << " x "
           << static_cast<double>(header.rows * scale) / 1000 << " km";
    std::ostringstream raster;
    raster << static_cast<double>(header.tile_mm) / 1000 << " m";
    return {extent.str(), std::to_string(header.columns) + " x " + std::to_string(header.rows) + " strategic sectors",
            "Terrain resolution " + raster.str()};
}

std::string faction_list(const std::set<FactionId>& factions)
{
    std::string list;
    for (const auto faction : factions) {
        if (!list.empty())
            list += ", ";
        list += faction_name(faction);
    }
    return list.empty() ? "Participating factions unspecified" : "Factions " + list;
}

struct CheckpointClaim {
    fs::path directory;

    ~CheckpointClaim()
    {
        std::error_code ignored;
        fs::remove(directory, ignored);
    }
};

bool content_file(const fs::path& file)
{
    const auto extension = file.extension();
    return extension == ".owgeo" || extension == ".owscenario" || extension == ".toml";
}

}  // namespace

std::string content_name(const fs::path& file)
{
    auto name = file.stem().string();
    bool initial = true;
    for (auto& ch : name) {
        if (ch == '-' || ch == '_')
            ch = ' ';
        if (initial)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        initial = ch == ' ';
    }
    return name;
}

fs::path CampaignCatalog::add(const fs::path& file)
{
    const auto path = fs::canonical(file);
    if (worlds.contains(path) || scenarios.contains(path))
        return path;
    if (!fs::is_regular_file(path))
        throw std::runtime_error(path.string() + ": expected a regular content file");
    if (path.extension() == ".owgeo") {
        add_geography(path, false);
    } else if (path.extension() == ".owscenario") {
        const auto summary = inspect_military_scenario(path);
        const auto world = fs::canonical(summary.world);
        if (!worlds.contains(world)) {
            if (!fs::is_regular_file(world))
                throw std::runtime_error(world.string() + ": expected a regular geographic file");
            add_geography(world, true);
        }
        auto name = summary.name.empty() ? content_name(path) : summary.name;
        std::vector<std::string> details;
        if (!summary.description.empty())
            details.push_back(summary.description);
        details.push_back(faction_list(summary.factions));
        if (summary.time_limit_seconds) {
            const auto seconds = *summary.time_limit_seconds;
            details.push_back(seconds == 0 ? "No scenario time limit" :
                seconds % 3600 == 0 ? "Time limit " + std::to_string(seconds / 3600) + " h" :
                "Time limit " + std::to_string(seconds) + " s");
        }
        for (const auto& [side, target] : summary.objectives)
            details.push_back(std::string(faction_name(side)) + " objective / " + target);
        scenarios.emplace(path, ScenarioListing{std::move(name), world, std::move(details), summary.factions});
    } else if (path.extension() == ".toml") {
        const auto settings = read_procedural_settings(path);
        scenarios.emplace(path, ScenarioListing{content_name(path), {}, {
            "Procedural campaign", "Factions WEST, EAST",
            std::to_string(settings.width) + " x " + std::to_string(settings.height) + " strategic sectors",
            "Seed " + std::to_string(settings.seed), "Time limit " + std::to_string(settings.time_limit_minutes) + " min"
        }, {}});
    } else {
        throw std::runtime_error("Choose an .owgeo, .owscenario or procedural .toml file");
    }
    return path;
}

void CampaignCatalog::add_geography(const fs::path& path, bool world_only)
{
    const auto header = inspect_geographic_header(path);
    if (world_only && header.version != 2)
        throw std::runtime_error(path.string() + ": expected an OWGEO 2 geographic world");
    auto details = world_dimensions(header);
    const auto fields = provenance_fields(header.provenance);
    const auto name = fields.contains("name") ? fields.at("name") : content_name(path);
    for (const auto& [field, label] : std::array{
             std::pair{"epsg", "EPSG "}, std::pair{"origin_m", "Projected origin "},
             std::pair{"vertical_datum", "Vertical datum "}}) {
        if (const auto value = fields.find(field); value != fields.end())
            details.push_back(label + value->second);
    }
    if (header.version == 2) {
        worlds.emplace(path, WorldListing{name, std::move(details)});
    } else {
        const std::set factions{FactionId::west, FactionId::east};
        details.insert(details.begin(), {"Combined OWGEO 1 demonstration", faction_list(factions)});
        scenarios.emplace(path, ScenarioListing{name + " (combined demo)", {}, std::move(details), factions});
    }
}

void CampaignCatalog::discover(std::span<const fs::path> roots, bool recursive)
{
    std::set<fs::path> candidates;
    for (const auto& root : roots) {
        std::error_code error;
        fs::recursive_directory_iterator entry(root, fs::directory_options::skip_permission_denied, error);
        for (; !error && entry != fs::recursive_directory_iterator{}; entry.increment(error)) {
            if (!recursive)
                entry.disable_recursion_pending();
            std::error_code status_error;
            if (content_file(entry->path()) && entry->is_regular_file(status_error)) {
                auto path = fs::weakly_canonical(entry->path(), status_error);
                candidates.insert(status_error ? entry->path().lexically_normal() : std::move(path));
            }
        }
    }

    for (const auto& file : candidates) {
        try {
            add(file);
        } catch (const std::exception& error) {
            const auto diagnostic = file.string() + " / " + error.what();
            if (std::ranges::find(issues, diagnostic) == issues.end())
                issues.push_back(diagnostic);
        }
    }
}

fs::path save_new_checkpoint(const CampaignState& campaign, const fs::path& directory)
{
    fs::create_directories(directory);
    for (unsigned number = 1; number <= 999'999; ++number) {
        auto path = directory / ("campaign-" + std::to_string(number) + ".ows");
        if (fs::symlink_status(path).type() != fs::file_type::not_found)
            continue;
        auto claim_path = path;
        claim_path += ".lock";
        if (!fs::create_directory(claim_path))
            continue;
        const CheckpointClaim claim{std::move(claim_path)};
        if (fs::symlink_status(path).type() != fs::file_type::not_found)
            continue;
        campaign.save(path);
        return path;
    }
    throw std::runtime_error("No free checkpoint name in " + directory.string());
}

}  // namespace openwar
