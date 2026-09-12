#include <algorithm>
#include <array>
#include <limits>

#include "activity_sdk_pack_composer_internal.h"

namespace sunrise::client::content::activity::sdk_generation::pack_composer::detail {

/** Checks the retained activity and panel tables at the public composition boundary. */
bool validate_storage(const Inputs&,
                      const PreparedRanges& prepared,
                      const Storage& value) noexcept {
    const std::array<std::size_t, format::kSectionCount> counts{
        value.strings.size(),
        value.activities.size(),
        value.scenarios.size(),
        value.bubbles.size(),
        value.states.size(),
        value.objects.size(),
        value.occurrences.size(),
        value.slots.size(),
        value.texts.size(),
        value.capabilities.size(),
        value.gates.size(),
        value.refusals.size(),
        value.actorClasses.size(),
        value.rsatDescriptors.size(),
        value.rsatSchemas.size(),
        value.rsatFields.size(),
        value.squads.size(),
        value.squadMembers.size(),
        value.squadAnchors.size(),
        value.authoredSceneResources.size(),
        value.authoredSceneSquadEdges.size(),
        value.taskTargets.size(),
        value.dialogueCueTexts.size(),
        value.directiveElements.size(),
        value.activityBindingTags.size(),
        value.activityBindingLocators.size(),
        value.behaviorPrograms.size(),
        value.behaviorInputs.size(),
        value.behaviorChannelWrites.size(),
        value.behaviorOwners.size(),
        value.behaviorActivityBindings.size(),
        value.actorMessageSchemas.size(),
        value.actorCommandDefinitions.size(),
        value.actorBehaviorProfiles.size(),
        value.simulationEventDefinitions.size(),
        value.runtimeSchemas.size(),
        value.runtimeFields.size(),
        value.sobjectRsats.size(),
        value.sobjectRsatDescriptors.size(),
        value.entityTypeDefinitions.size(),
        value.sobjectRsatFieldBindings.size(),
        value.runtimeTypeDefinitions.size(),
        value.actorStateNames.size(),
        value.actorSequenceTables.size(),
        value.actorSequenceEntries.size(),
        value.actorSequenceBindings.size(),
    };
    const bool bounded = std::all_of(counts.begin(), counts.end(), [](std::size_t count) {
        return count <= (std::numeric_limits<std::uint32_t>::max)();
    });
    return bounded && !value.strings.empty() && !value.activities.empty()
           && !value.actorMessageSchemas.empty() && !value.actorCommandDefinitions.empty()
           && value.actorBehaviorProfiles.size() == value.actorClasses.size()
           && !value.simulationEventDefinitions.empty() && !value.runtimeSchemas.empty()
           && !value.runtimeFields.empty() && !value.sobjectRsats.empty()
           && !value.entityTypeDefinitions.empty()
           && value.sobjectRsatFieldBindings.size() == value.rsatFields.size()
           && !value.runtimeTypeDefinitions.empty()
           && prepared.scenarioBubbles.size() == value.scenarios.size()
           && prepared.scenarioStates.size() == value.scenarios.size()
           && prepared.scenarioOccurrences.size() == value.scenarios.size()
           && prepared.bubbleStates.size() == value.bubbles.size()
           && prepared.objectSlots.size() == value.objects.size();
}

} // namespace sunrise::client::content::activity::sdk_generation::pack_composer::detail
