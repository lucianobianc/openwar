#ifndef OPENWAR_LAUNCHER_MODEL_HPP
#define OPENWAR_LAUNCHER_MODEL_HPP

#include "scenario.hpp"

namespace openwar {

struct WorldListing {
    std::string name;
    std::vector<std::string> metadata;
};

struct ScenarioListing {
    std::string name;
    std::filesystem::path world;  // Empty for combined and procedural inputs.
    std::vector<std::string> details;
    std::set<FactionId> commanders;
};

struct CampaignCatalog {
    std::map<std::filesystem::path, WorldListing> worlds;
    std::map<std::filesystem::path, ScenarioListing> scenarios;
    std::vector<std::string> issues;

    std::filesystem::path add(const std::filesystem::path& file);
    void discover(std::span<const std::filesystem::path> roots, bool recursive = true);

private:
    void add_geography(const std::filesystem::path& path, bool world_only);
};

std::string content_name(const std::filesystem::path& file);
std::filesystem::path save_new_checkpoint(const CampaignState& campaign, const std::filesystem::path& directory);

}  // namespace openwar

#endif
