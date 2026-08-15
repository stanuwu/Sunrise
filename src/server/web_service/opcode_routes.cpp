#include "opcode_routes.h"

#include <algorithm>
#include <array>

namespace sunrise::server::web_service {
namespace {

/** Opcodes whose response descriptor holds only the 5-bit biased status. */
constexpr auto kStatusOnlyOpcodes = std::to_array<std::uint16_t>({
    206,  304,  305,  1101, 1102, 1214, 1218, 1225, 1226, 1231, 1232, 1233,
    1234, 1235, 1237, 1239, 1241, 1242, 1244, 1245, 1301, 1822, 2100,
});

/** Opcodes whose response descriptor holds the shared 37-bit status pair. */
constexpr auto kStatusPairOpcodes = std::to_array<std::uint16_t>({
    105,  106,  303,  402,  403,  404,  405,  406,  502,  504,  506,  701,  702,  801,
    802,  803,  804,  902,  903,  904,  905,  1103, 1201, 1202, 1203, 1205, 1206, 1207,
    1208, 1209, 1210, 1211, 1212, 1213, 1215, 1216, 1219, 1220, 1221, 1222, 1223, 1224,
    1227, 1228, 1229, 1230, 1236, 1238, 1240, 1243, 1307, 1309, 1310, 1401, 1615, 1616,
    1617, 1618, 1701, 1702, 1801, 1802, 1803, 1820, 1821, 1901, 2002, 2200, 2300, 2400,
});

/** Opcodes whose status pair has one required trailing boolean field. */
constexpr auto kStatusPairBoolOpcodes = std::to_array<std::uint16_t>({104, 901});

template <std::size_t Size>
[[nodiscard]] bool contains(const std::array<std::uint16_t, Size>& values,
                            std::uint16_t opcode) noexcept {
    return std::binary_search(values.begin(), values.end(), opcode);
}

} // namespace

/** Picks one stateless response descriptor for a request opcode. */
void resolve_response_shape(std::uint16_t opcode,
                            middleware::web_service::ResponseShape& shape) noexcept {
    if (contains(kStatusOnlyOpcodes, opcode)) {
        shape = middleware::web_service::ResponseShape::statusOnly;
        return;
    }
    if (contains(kStatusPairOpcodes, opcode)) {
        shape = middleware::web_service::ResponseShape::statusPair;
        return;
    }
    if (contains(kStatusPairBoolOpcodes, opcode)) {
        shape = middleware::web_service::ResponseShape::statusPairWithBool;
        return;
    }
    // Unknown opcodes still get a correlated echo so the client task completes.
    shape = middleware::web_service::ResponseShape::generic;
}

} // namespace sunrise::server::web_service
