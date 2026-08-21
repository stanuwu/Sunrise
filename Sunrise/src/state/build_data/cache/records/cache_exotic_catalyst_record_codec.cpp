#include "codec.h"

namespace sunrise::state::build_data::cache::records {

bool encode(const items::catalysts::Definition& value, ExoticCatalystRecord& record) noexcept {
    record = {};
    if (!items::catalysts::valid_availability(value.availability)) {
        return false;
    }
    record.itemDefinitionHash = value.itemDefinitionHash;
    record.itemDefinitionIndex = value.itemDefinitionIndex;
    record.completedPlugDefinitionIndex = value.completedPlugDefinitionIndex;
    record.progressPlugDefinitionIndex = value.progressPlugDefinitionIndex;
    record.effectDefinitionIndex = value.effectDefinitionIndex;
    record.acquisitionDefinitionIndex = value.acquisitionDefinitionIndex;
    record.completionFlagDefinitionIndices = value.completion.flags;
    for (std::size_t index = 0; index < value.completion.values.size(); ++index) {
        record.completionValueIndices[index] = value.completion.values[index].index;
        record.completionValues[index] = value.completion.values[index].minimum;
    }
    record.objectiveDefinitionIndex = value.objective.definitionIndex;
    record.socketLane = value.socketLane;
    record.availability = static_cast<std::uint8_t>(value.availability);
    record.completionFlagCount = value.completion.flagCount;
    record.completionValueCount = value.completion.valueCount;
    record.objectiveValue = value.objective.value;
    return true;
}

bool decode(const ExoticCatalystRecord& record, items::catalysts::Definition& value) noexcept {
    value = {};
    const auto availability = static_cast<items::catalysts::Availability>(record.availability);
    if (!items::catalysts::valid_availability(availability)) {
        return false;
    }
    value.itemDefinitionHash = record.itemDefinitionHash;
    value.itemDefinitionIndex = record.itemDefinitionIndex;
    value.completedPlugDefinitionIndex = record.completedPlugDefinitionIndex;
    value.progressPlugDefinitionIndex = record.progressPlugDefinitionIndex;
    value.effectDefinitionIndex = record.effectDefinitionIndex;
    value.acquisitionDefinitionIndex = record.acquisitionDefinitionIndex;
    value.completion.flags = record.completionFlagDefinitionIndices;
    for (std::size_t index = 0; index < value.completion.values.size(); ++index) {
        value.completion.values[index] = {record.completionValueIndices[index],
                                         record.completionValues[index]};
    }
    value.completion.flagCount = record.completionFlagCount;
    value.completion.valueCount = record.completionValueCount;
    value.objective = {record.objectiveDefinitionIndex, record.objectiveValue};
    value.socketLane = record.socketLane;
    value.availability = availability;
    return true;
}

} // namespace sunrise::state::build_data::cache::records
