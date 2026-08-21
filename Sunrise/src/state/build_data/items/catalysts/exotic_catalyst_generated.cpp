#include <array>

#include "exotic_catalyst_builder.h"

namespace sunrise::state::build_data::items::catalysts {
namespace {

/** PE identity of the supported 86657.20.08.23 client. */
constexpr std::uint32_t kImageTimestamp = 0x5F43138BU;
constexpr std::uint32_t kImageSize = 0x08A5EA00U;
/**
 * Released Season 11 weapon hashes, in definition-hash order.
 * The installed build stages later catalyst sockets with the same item, socket, and visibility
 * fields as released catalysts. Runtime extraction can recover every relation except release date.
 * Default and completed plugs come from each exact socket pool, so they are not pinned here.
 */
constexpr std::array<std::uint32_t, 45> kReleasedWeaponHashes{
    0x012248BAU, 0x14B465B2U, 0x17D8FEABU, 0x2E43BDEEU, 0x3092080DU, 0x4F5CCF1DU, 0x50384F32U,
    0x50384F33U, 0x5141601FU, 0x59EFED62U, 0x5BDBCC56U, 0x634C6957U, 0x6F22FCECU, 0x70BEF156U,
    0x83A19696U, 0x8843C72AU, 0x8C8180D6U, 0x8CD074B1U, 0x93E680C3U, 0xA7DBFF3AU, 0xAA45882AU,
    0xAD4746D4U, 0xAD4746D5U, 0xB824C63DU, 0xBB46CCD2U, 0xBB46CCD3U, 0xBF704917U, 0xCB6F6266U,
    0xCB7B5EDFU, 0xCCE7D927U, 0xD15517D4U, 0xD38BCABAU, 0xD38BCABBU, 0xD5704484U, 0xD5704485U,
    0xD84E04AAU, 0xD84E04ABU, 0xE0794C51U, 0xE5296126U, 0xE86A25CFU, 0xEF7D3366U, 0xF0923C79U,
    0xF5DE4480U, 0xF9C0B6B0U, 0xFDA23E68U,
};

} // namespace

Facts generated_facts() noexcept {
    return {kImageTimestamp, kImageSize, kReleasedWeaponHashes};
}

} // namespace sunrise::state::build_data::items::catalysts
