#include "mission_script_carrier_plan.h"

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace sunrise::server::activity::mission {
namespace {

namespace auth = middleware::bap::activity_message::scriptable_auth;
namespace combatant = middleware::bap::activity_message::combatant_auth;
namespace squad_auth = middleware::bap::activity_message::squad_auth;
using state::activity::mission::CarrierPlan;
using state::activity::mission::CarrierStage;
using state::activity::mission::CarrierStep;
using state::activity::mission::CarrierWait;
using state::activity::mission::IntentKind;

/** Every counter a step body carries is this template until the Host writes its own. */
constexpr std::uint32_t kTemplateCounter = 1;

/** Appends one actor step carrying an encoded type-2 body. */
void append_actor_step(CarrierPlan& plan,
                       const CarrierDefinition& definition,
                       IntentKind kind,
                       std::span<const std::byte> body,
                       std::size_t bits,
                       CarrierStage stage,
                       CarrierWait wait,
                       bool spawn = false) {
    CarrierStep step{};
    step.intent = definition.actor;
    step.intent.kind = kind;
    step.intent.active = spawn;
    step.intent.authBody.assign(body.begin(), body.end());
    step.intent.authByteCount = static_cast<std::uint16_t>(body.size());
    step.intent.authBitCount = static_cast<std::uint16_t>(bits);
    step.stage = stage;
    step.wait = wait;
    plan.steps.push_back(std::move(step));
}

/**
 * Appends one actor program step. A creating program carries the flight's temperaments, so the
 * new actor never runs without them.
 * @return False when the program does not encode.
 */
[[nodiscard]] bool append_program(CarrierPlan& plan,
                                  const CarrierDefinition& definition,
                                  const auth::Type2AtomProgram& atoms,
                                  CarrierStage stage,
                                  CarrierWait wait,
                                  bool spawn = false) {
    auth::Type2Body body{};
    if (spawn) {
        body.channels = definition.controls;
    }
    body.channels.revision = kTemplateCounter;
    body.atoms = atoms;
    body.atoms.generation = kTemplateCounter;
    std::array<std::byte, auth::kType2MaximumBodyByteCount> bytes{};
    std::size_t written = 0;
    std::size_t bits = 0;
    if (!auth::encode_type2_body(body, bytes, written, bits)) {
        return false;
    }
    append_actor_step(plan,
                      definition,
                      IntentKind::runActorProgram,
                      std::span(bytes).first(written),
                      bits,
                      stage,
                      wait,
                      spawn);
    return true;
}

/** @return False when the program has no lane left. */
[[nodiscard]] bool add_lane(auth::Type2AtomProgram& atoms, auth::Type2LanePrimary primary) {
    if (atoms.count == atoms.lanes.size()) {
        return false;
    }
    atoms.lanes[atoms.count++].primary = primary;
    return true;
}

/** Adds a move to every point of a path after its start, or to its only point. */
[[nodiscard]] bool add_path_points(auth::Type2AtomProgram& atoms, const CarrierPath& path) {
    for (std::uint8_t point = path.points > 1 ? 1 : 0; point < path.points; ++point) {
        if (!add_lane(atoms, auth::Type2LaneRefByteBool{path.reference, point, true})) {
            return false;
        }
    }
    return true;
}

/** Appends the creation and arrival steps the entry mode names. */
[[nodiscard]] bool append_entry(CarrierPlan& plan, const CarrierDefinition& definition) {
    const CarrierPath& entry = definition.entry;
    auth::Type2AtomProgram arrival{};
    if (definition.entryMode == CarrierEntryMode::ability
        || definition.entryMode == CarrierEntryMode::abilityThenPath) {
        const bool thenPath = definition.entryMode == CarrierEntryMode::abilityThenPath;
        const auto origin = static_cast<std::uint8_t>(thenPath ? 0 : entry.points - 1);
        if (!add_lane(arrival, auth::Type2LaneRefByte{entry.reference, origin})
            || !add_lane(arrival, definition.entryAbility)
            || !append_program(
                plan, definition, arrival, CarrierStage::entry, CarrierWait::program, true)) {
            return false;
        }
        auth::Type2AtomProgram path{};
        return !thenPath
               || (add_path_points(path, entry)
                   && append_program(
                       plan, definition, path, CarrierStage::entry, CarrierWait::program));
    }
    // A relocated actor appears on the path's start; otherwise it flies there first.
    const bool start =
        definition.relocateStart
            ? add_lane(arrival, auth::Type2LaneRefByte{entry.reference, 0})
            : add_lane(arrival, auth::Type2LaneRefByteBool{entry.reference, 0, true});
    if (!start || !add_path_points(arrival, entry)
        || !append_program(
            plan, definition, arrival, CarrierStage::entry, CarrierWait::program, true)) {
        return false;
    }
    auth::Type2AtomProgram ability{};
    return definition.entryMode != CarrierEntryMode::pathThenAbility
           || (add_lane(ability, definition.entryAbility)
               && append_program(
                   plan, definition, ability, CarrierStage::entry, CarrierWait::program));
}

/** Appends the controls step that opens or closes the doors, keeping the temperaments. */
[[nodiscard]] bool append_doors(CarrierPlan& plan, const CarrierDefinition& definition, bool open) {
    auth::Type2Body body{};
    body.channels = definition.controls;
    body.channels.revision = kTemplateCounter;
    std::array<std::byte, auth::kType2MaximumBodyByteCount> bytes{};
    std::size_t written = 0;
    std::size_t bits = 0;
    if (!auth::set_type2_channel(body.channels, definition.doorChannel, open ? 1.0F : 0.0F)
        || !auth::encode_type2_body(body, bytes, written, bits)) {
        return false;
    }
    append_actor_step(plan,
                      definition,
                      IntentKind::setActorControls,
                      std::span(bytes).first(written),
                      bits,
                      open ? CarrierStage::unloading : CarrierStage::departure,
                      CarrierWait::transport);
    return true;
}

/** Appends one reservation per cargo squad, then the manifests that unload them. */
[[nodiscard]] bool append_cargo(CarrierPlan& plan, const CarrierDefinition& definition) {
    const auto cargo = std::span(definition.cargo).first(definition.cargoCount);
    for (const CarrierCargo& squad : cargo) {
        CarrierStep reserve{};
        reserve.intent.kind = IntentKind::reserveCarrierCargo;
        reserve.intent.firstRow = squad.squadRow;
        reserve.intent.squadCounts = squad.counts;
        reserve.intent.squadCount = squad.memberCount;
        reserve.intent.squadMode = static_cast<std::uint8_t>(squad_auth::Mode::reserve);
        reserve.stage = CarrierStage::unloading;
        reserve.wait = CarrierWait::transport;
        plan.steps.push_back(std::move(reserve));
    }
    const std::size_t perManifest = definition.serial ? 1 : cargo.size();
    for (std::size_t first = 0; first < cargo.size(); first += perManifest) {
        std::array<combatant::SquadReference, combatant::kMaximumManifestSquads> manifest{};
        for (std::size_t index = 0; index < perManifest; ++index) {
            manifest[index] = cargo[first + index].reference;
        }
        std::array<std::byte, combatant::kDeliveryMaximumBytes> bytes{};
        std::size_t written = 0;
        std::size_t bits = 0;
        if (!combatant::encode_delivery(kTemplateCounter,
                                        kTemplateCounter,
                                        std::span(manifest).first(perManifest),
                                        bytes,
                                        written,
                                        bits)) {
            return false;
        }
        append_actor_step(plan,
                          definition,
                          IntentKind::deliverPassengers,
                          std::span(bytes).first(written),
                          bits,
                          CarrierStage::unloading,
                          CarrierWait::delivery);
    }
    return true;
}

/** Appends the departure along the exit path, the exit ability and the retirement. */
[[nodiscard]] bool append_departure(CarrierPlan& plan, const CarrierDefinition& definition) {
    auth::Type2AtomProgram departure{};
    const bool route =
        definition.exitMarker >= 0
            ? add_lane(departure,
                       auth::Type2LaneRefByteBool{definition.exit.reference,
                                                  static_cast<std::uint8_t>(definition.exitMarker),
                                                  true})
            : add_path_points(departure, definition.exit);
    auth::Type2AtomProgram exitAbility{};
    if (!route
        || !append_program(
            plan, definition, departure, CarrierStage::departure, CarrierWait::program)
        || !add_lane(exitAbility, definition.exitAbility)
        || !append_program(
            plan, definition, exitAbility, CarrierStage::departure, CarrierWait::programRetires)) {
        return false;
    }
    auth::Type2Body retired{};
    retired.channels.revision = kTemplateCounter;
    retired.atoms.generation = kTemplateCounter;
    std::array<std::byte, auth::kType2MaximumBodyByteCount> bytes{};
    std::size_t written = 0;
    std::size_t bits = 0;
    if (!auth::encode_type2_body(retired, bytes, written, bits)) {
        return false;
    }
    append_actor_step(plan,
                      definition,
                      IntentKind::retireActor,
                      std::span(bytes).first(written),
                      bits,
                      CarrierStage::retirement,
                      CarrierWait::transport);
    return true;
}

} // namespace

/** Composes a flight's native steps in order; null when a step does not fit its body. */
std::shared_ptr<const CarrierPlan> compile_carrier_plan(const CarrierDefinition& definition) {
    auto plan = std::make_shared<CarrierPlan>();
    if (!append_entry(*plan, definition) || !append_doors(*plan, definition, true)
        || !append_cargo(*plan, definition) || !append_doors(*plan, definition, false)
        || !append_departure(*plan, definition)) {
        return {};
    }
    return plan;
}

} // namespace sunrise::server::activity::mission
