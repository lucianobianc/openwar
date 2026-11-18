#ifndef OPENWAR_MILITARY_IDENTITY_HPP
#define OPENWAR_MILITARY_IDENTITY_HPP

#include <array>
#include <cstddef>
#include <string_view>

namespace openwar {

enum class MilitaryDomain { unspecified, land, air, sea };
enum class MilitaryFunction {
    unspecified, infantry, armor, mechanized_infantry, reconnaissance, artillery, engineer, signals, transport, support
};
enum class MilitaryEchelon { unspecified, platoon, company, battalion, regiment, brigade, division };

struct MilitaryIdentity {
    MilitaryDomain domain = MilitaryDomain::unspecified;
    MilitaryFunction function = MilitaryFunction::unspecified;
    MilitaryEchelon echelon = MilitaryEchelon::unspecified;
    bool operator==(const MilitaryIdentity&) const = default;
};

inline constexpr std::array<std::string_view, 4> military_domains{"unspecified", "land", "air", "sea"};
inline constexpr std::array<std::string_view, 10> military_functions{
    "unspecified", "infantry", "armor", "mechanized-infantry", "reconnaissance", "artillery",
    "engineer", "signals", "transport", "support"
};
inline constexpr std::array<std::string_view, 7> military_echelons{
    "unspecified", "platoon", "company", "battalion", "regiment", "brigade", "division"
};

inline bool valid_military_identity(MilitaryIdentity identity)
{
    return static_cast<std::size_t>(identity.domain) < military_domains.size() &&
        static_cast<std::size_t>(identity.function) < military_functions.size() &&
        static_cast<std::size_t>(identity.echelon) < military_echelons.size();
}

}  // namespace openwar

#endif
