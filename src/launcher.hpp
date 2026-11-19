#ifndef OPENWAR_LAUNCHER_HPP
#define OPENWAR_LAUNCHER_HPP

#include "campaign.hpp"

namespace openwar {

struct LaunchSelection {
    CampaignState campaign;
    std::optional<FactionId> faction;
};

std::filesystem::path launcher_data_directory();
std::optional<LaunchSelection> show_launcher(const std::filesystem::path& data_directory,
                                             std::string startup_error = {});

}  // namespace openwar

#endif
