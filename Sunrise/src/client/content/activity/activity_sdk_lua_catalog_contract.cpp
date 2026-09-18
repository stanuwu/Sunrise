#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {
namespace {

struct RowSpec final {
    std::uint32_t sectionIndex{};
    std::string_view section{};
    std::string_view collection{};
    std::string_view view{};
    std::string_view fields{};
};

/** Every StringRef field declared by the row table below. */
constexpr std::size_t kDecodedStringFieldCount = 55;
/** Section index of the activity-owned binding-tag rows, which have no direct collection. */
constexpr std::uint32_t kBindingTagSectionIndex = 24;
/** Section index of the binding-locator rows, projected both directly and per activity. */
constexpr std::uint32_t kBindingLocatorSectionIndex = 25;

/** Compact type:name tokens are the single native inventory for all raw pack rows. */
constexpr std::array<RowSpec, 40> kRows{{
    {1,
     "activities",
     "activities",
     "CatalogActivityView",
     "s:id;s:internal_name;s:display_name;r:aliases;r:capabilities;"
     "r:activity_root_candidate_tags;r:scenario_name_candidate_tags;r:evidence_root_tags;"
     "r:binding_locators;u32:activity_index;u32:definition_hash;u32:scenario_index;"
     "u32:flags;u32:selected_activity_root_tag;u32:selected_scenario_tag;"
     "u32:matchmaking_config_tag;u32:join_status;u32:binding_disposition;"
     "u32:binding_reason;u32:binding_evidence_basis;u32:runnable_status;u32:binding_flags"},
    {2,
     "scenarios",
     "scenarios",
     "CatalogScenarioView",
     "s:id;s:name;r:bubbles;r:states;r:occurrences;u32:tag;u32:reserved"},
    {3,
     "bubbles",
     "bubbles",
     "CatalogBubbleView",
     "s:id;s:name;r:states;u32:scenario_index;u32:bubble_ordinal;u32:name_hash;u32:reserved"},
    {4,
     "states",
     "states",
     "CatalogStateView",
     "s:id;s:entry_id;s:registry_id;u32:scenario_index;u32:bubble_index;"
     "u32:state_ordinal;u32:entry_index;u32:slice_set_index;u32:map_bubble_index;"
     "u32:state_hash;u32:public_value;u32:flags;u32:registry_tag"},
    {5,
     "objects",
     "objects",
     "CatalogObjectView",
     "s:id;r:slots;u32:object_tag;u32:object_key;u32:config_count;u32:descriptor_count;"
     "u32:placed_subblock_count;u32:placed_leaf_count;u32:placed_hop_count;"
     "u32:bare_target_count;u32:replicated_placement_count"},
    {6,
     "occurrences",
     "occurrences",
     "CatalogOccurrenceView",
     "s:id;s:context_registry_key;s:registry_id;s:entry_id;u32:scenario_index;"
     "u32:bubble_index;u32:state_index;u32:object_index;u32:registry_field;"
     "u32:object_ordinal"},
    {7,
     "slots",
     "slots",
     "CatalogSlotView",
     "s:id;s:name;s:sense_schema_id;s:auth_schema_id;r:aliases;r:capabilities;"
     "u32:object_index;u32:slot_index;u32:slot_type;u32:component_class;"
     "u32:sense_schema;u32:auth_schema;u32:flags;u32:reserved"},
    {8, "texts", "texts", "CatalogTextView", "s:value;u32:kind;u32:reserved"},
    {9,
     "capabilities",
     "capabilities",
     "CatalogCapabilityView",
     "s:id;s:operation;s:value_schema_id;r:gates;r:refusals;u32:subject_kind;"
     "u32:subject_index;u32:exposure_flags;u32:candidate_exposure_flags"},
    {10,
     "gates",
     "gates",
     "CatalogGateView",
     "s:gate;s:status;s:reason_code;s:required;s:observed;s:would_confirm"},
    {11,
     "refusals",
     "refusals",
     "CatalogRefusalView",
     "s:id;s:exposure;s:status;r:reason_codes;u32:capability_index;u32:reserved"},
    {12,
     "actor_classes",
     "actor_classes",
     "CatalogActorClassView",
     "s:id;r:descriptors;u32:definition_tag;u32:name_hash;u32:rsat_tag;"
     "u32:rsat_reverse_definition_tag;u32:object_type;u32:descriptor_array_offset;"
     "u32:descriptor_array_header_offset;u32:descriptor_array_data_offset;"
     "u32:descriptor_element_class;u32:dynamic_presence_tail_count;"
     "u32:authored_profile_0;u32:authored_profile_1;u32:authored_profile_2;"
     "u32:authored_profile_3;"
     "i64:descriptor_array_relative"},
    {13,
     "rsat_descriptors",
     "rsat_descriptors",
     "CatalogRsatDescriptorView",
     "s:id;u32:actor_class_index;u32:rsat_tag;u32:descriptor_ordinal;"
     "u32:descriptor_offset;u32:descriptor_element_class;u32:component_tag;"
     "u32:schema_index;u32:schema_tag;u32:schema_field_count;"
     "u32:schema_first_field_runtime_gate;u32:schema_first_field_raw_u32_at_10;"
     "u32:flags;u32:dynamic_presence_tail_ordinal;b32:raw_row"},
    {14,
     "rsat_schemas",
     "rsat_schemas",
     "CatalogRsatSchemaView",
     "s:id;r:fields;u32:schema_tag;u32:schema_class;u32:field_count;"
     "u32:field_array_offset;u32:field_array_header_offset;u32:field_array_data_offset;"
     "u32:field_element_class;u32:first_field_runtime_gate;"
     "u32:first_field_raw_u32_at_10;u32:flags;i64:field_array_relative"},
    {15, "rsat_fields", "rsat_fields", "CatalogRsatFieldView", "b40:raw_row"},
    {16,
     "squads",
     "squads",
     "CatalogSquadView",
     "s:id;r:members;r:anchors;u32:scenario_index;u32:object_index;u32:slot_index;"
     "u32:spawner_config_tag;u32:spawn_rule_config_tag;u32:flags;u32:occurrence_index"},
    {17,
     "squad_members",
     "squad_members",
     "CatalogSquadMemberView",
     "s:id;u32:squad_index;u32:member_ordinal;u32:member_key;u32:actor_class_index;"
     "u32:flags;i32:default_count;u16:candidate_count_0;u16:candidate_count_1;"
     "u16:candidate_count_2;u16:candidate_count_3;u16:candidate_count_4;"
     "u16:candidate_count_5"},
    {18,
     "squad_anchors",
     "squad_anchors",
     "CatalogSquadAnchorView",
     "s:id;u32:squad_index;u32:point_ordinal;u32:object_list_tag;"
     "u32:placement_ordinal;u32:flags;u32:position_bits_x;u32:position_bits_y;"
     "u32:position_bits_z;u64:placed_entry_identity"},
    {19,
     "authored_scene_resources",
     "authored_scene_resources",
     "CatalogAuthoredSceneResourceView",
     "s:id;u32:slot_index;u32:config_tag;u32:descriptor_offset;"
     "u32:resource_field_offset;u32:resource_tag;u32:resource_class;u32:flags;u32:reserved"},
    {20,
     "authored_scene_squad_edges",
     "authored_scene_squad_edges",
     "CatalogAuthoredSceneSquadEdgeView",
     "s:id;u32:scene_slot_index;u32:squad_slot_index;u32:config_tag;"
     "u32:descriptor_offset;u32:reference_field_offset;u32:target_object_key;"
     "u32:flags;u32:reserved"},
    {21,
     "task_targets",
     "task_targets",
     "CatalogTaskTargetView",
     "s:id;u32:task_slot_index;u32:objective_slot_index;u32:config_tag;"
     "u32:descriptor_offset;u32:reference_field_offset;u32:target_object_key;"
     "u32:bit_index;u32:flags;u32:reserved"},
    {22,
     "dialogue_cue_texts",
     "dialogue_cue_texts",
     "CatalogDialogueCueTextView",
     "s:id;s:text;u32:slot_index;u32:cue_index;u32:definition_hash;u32:container_tag;"
     "u32:string_hash"},
    {23,
     "directive_elements",
     "directive_elements",
     "CatalogDirectiveElementView",
     "s:id;s:title;s:description;u32:slot_index;u32:name_hash;i32:element_index;"
     "u32:element_count;u32:title_container_tag;u32:title_string_hash;"
     "u32:description_container_tag;u32:description_string_hash;s:progress;"
     "u32:progress_container_tag;u32:progress_string_hash;u32:flags"},
    {25,
     "activity_binding_locators",
     "activity_binding_locators",
     "CatalogActivityBindingLocatorView",
     "u32:tag;u32:reserved;u64:offset"},
    {26,
     "behavior_programs",
     "behavior_programs",
     "CatalogBehaviorProgramView",
     "r:inputs;r:channel_writes;u32:root_tag;u32:node_count;u32:expression_count"},
    {27,
     "behavior_inputs",
     "behavior_inputs",
     "CatalogBehaviorInputView",
     "u32:program_index;u32:node_offset;u32:expression_offset;u32:channel_hash;"
     "i32:native_override;u32:active_field;u16:reserved;u8:selector;u8:role;"
     "u64:input_or_mode"},
    {28,
     "behavior_channel_writes",
     "behavior_channel_writes",
     "CatalogBehaviorChannelWriteView",
     "u32:program_index;u32:node_offset;u32:channel_hash;u32:reserved"},
    {29,
     "behavior_owners",
     "behavior_owners",
     "CatalogBehaviorOwnerView",
     "u32:program_index;u32:actor_class_index;u32:config_tag;u32:config_field_offset;"
     "u32:build_ordinal;u32:descriptor_ordinal;u32:submitter_subtype;u32:submission_kind"},
    {30,
     "behavior_activity_bindings",
     "behavior_activity_bindings",
     "CatalogBehaviorActivityBindingView",
     "u32:owner_index;u32:squad_index;u32:squad_member_index;u32:scenario_index;"
     "u32:occurrence_index;u32:state_index;u32:object_index;u32:reserved"},
    {31,
     "actor_message_schemas",
     "actor_message_schemas",
     "CatalogActorMessageSchemaView",
     "s:name;r:commands;u32:definition_handle;u32:durable_key;u32:owner_class;"
     "u32:handler_slot;u32:body_type;u32:provenance;u32:flags;u32:reserved"},
    {32,
     "actor_command_definitions",
     "actor_command_definitions",
     "CatalogActorCommandDefinitionView",
     "s:name;s:faction_none_name;s:faction_removed_name;s:faction_hostile_to_all_name;"
     "u32:selector;u32:payload_handle;u32:effect;u32:provenance;i32:faction_none;"
     "i32:faction_removed;i32:faction_hostile_to_all;u32:flags"},
    {33,
     "actor_behavior_profiles",
     "actor_behavior_profiles",
     "CatalogActorBehaviorProfileView",
     "u32:actor_class_index;u32:behavior_config_tag;u32:behavior_config_class;"
     "u32:behavior_config_offset;i32:default_faction;u32:behavior_provenance;"
     "u32:faction_provenance;u32:flags"},
    {34,
     "simulation_event_definitions",
     "simulation_event_definitions",
     "CatalogSimulationEventDefinitionView",
     "s:name;u32:event_type;u32:primary_schema;u32:secondary_schema;u32:provenance;"
     "u32:flags;u32:reserved;u64:descriptor_evidence_address;u64:primary_evidence_address;"
     "u64:secondary_evidence_address"},
    {35,
     "runtime_schemas",
     "runtime_schemas",
     "CatalogRuntimeSchemaView",
     "r:fields;u32:handle;u32:decoded_size;u32:definition_hash;u32:provenance;"
     "u32:definition_class;u32:codec_families;u32:flags;u32:array_element_count;"
     "u64:evidence_address"},
    {36,
     "runtime_fields",
     "runtime_fields",
     "CatalogRuntimeFieldView",
     "u32:schema_index;u32:ordinal;u32:struct_offset;u32:alternate_offset;u32:type_code;"
     "u32:nested_handle;u32:bits;u32:codec_parameter_0;u32:codec_parameter_1;"
     "u32:codec_parameter_2;u32:codec_parameter_3;u32:flags;i64:bias"},
    {37,
     "sobject_rsats",
     "sobject_rsats",
     "CatalogSobjectRsatView",
     "r:descriptors;u32:rsat_tag;u32:reverse_definition_tag;u32:descriptor_array_offset;"
     "u32:descriptor_array_header_offset;u32:descriptor_array_data_offset;"
     "u32:descriptor_element_class;u32:dynamic_presence_tail_count;u32:provenance;"
     "u32:flags;u32:reserved;i64:descriptor_array_relative"},
    {38,
     "sobject_rsat_descriptors",
     "sobject_rsat_descriptors",
     "CatalogSobjectRsatDescriptorView",
     "u32:rsat_index;u32:descriptor_ordinal;u32:descriptor_offset;u32:component_tag;"
     "u32:schema_index;u32:schema_tag;u32:schema_field_count;"
     "u32:schema_first_field_runtime_gate;u32:flags;u32:dynamic_presence_tail_ordinal;"
     "b32:raw_row"},
    {39,
     "entity_type_definitions",
     "entity_type_definitions",
     "CatalogEntityTypeDefinitionView",
     "s:name;u32:entity_type;u32:baseline_schema;u32:update_schema;u32:provenance;"
     "u32:flags;u32:reserved;u64:vtable_evidence_address;u64:baseline_evidence_address;"
     "u64:update_evidence_address"},
    {40,
     "sobject_rsat_field_bindings",
     "sobject_rsat_field_bindings",
     "CatalogSobjectRsatFieldBindingView",
     "u32:rsat_field_index;u32:runtime_schema_handle;u32:parameter_14;u32:parameter_18;"
     "u32:definition_class;u32:codec_families;u32:provenance;u32:flags;u64:decoded_offset"},
    {41,
     "runtime_type_definitions",
     "runtime_type_definitions",
     "CatalogRuntimeTypeDefinitionView",
     "s:name;u32:codec_families;u32:type_code;u32:decoded_size;u32:fixed_bits;"
     "u32:minimum_bits;u32:maximum_bits;u32:flags;u32:reserved;"
     "u64:writer_evidence_address;u64:reader_evidence_address"},
}};

void sort_keys(Value::Object& value) {
    std::sort(value.begin(), value.end(), [](const auto& first, const auto& second) {
        return first.first < second.first;
    });
}

/** Splits the semicolon list into type:name pairs; false on a bad token or a refusing callback. */
template <typename Callback>
[[nodiscard]] bool for_each_field(std::string_view fields, Callback&& callback) {
    while (!fields.empty()) {
        const std::size_t end = fields.find(';');
        const std::string_view token = fields.substr(0, end);
        const std::size_t separator = token.find(':');
        if (separator == std::string_view::npos || separator == 0 || separator + 1 >= token.size()
            || !callback(token.substr(0, separator), token.substr(separator + 1))) {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        fields.remove_prefix(end + 1);
    }
    return true;
}

void add_field(Value::Object& fields, std::string_view name, std::string_view type) {
    fields.push_back({std::string(name), string(type)});
}

void add_suffix_field(Value::Object& fields,
                      std::string_view name,
                      std::string_view suffix,
                      std::string_view type) {
    std::string key(name);
    key.append(suffix);
    fields.push_back({std::move(key), string(type)});
}

/** Expands one type code into its declared field names; false when the code is not one we emit. */
[[nodiscard]] bool
append_raw_field(Value::Object& fields, std::string_view code, std::string_view name) {
    if (code == "s") {
        add_field(fields, name, "decoded_utf8_string_or_nil");
        add_suffix_field(fields, name, "_string_offset", "u32");
        add_suffix_field(fields, name, "_string_length", "u32");
    } else if (code == "r") {
        add_suffix_field(fields, name, "_first_index", "u32_zero_based");
        add_suffix_field(fields, name, "_count", "u32");
    } else if (code == "u32") {
        add_field(fields, name, "u32");
    } else if (code == "i32") {
        add_field(fields, name, "i32");
    } else if (code == "u64") {
        add_field(fields, name, "u64_decimal_string");
    } else if (code == "i64") {
        add_field(fields, name, "i64_decimal_string");
    } else if (code == "u16") {
        add_field(fields, name, "u16");
    } else if (code == "u8") {
        add_field(fields, name, "u8");
    } else if (code == "b32") {
        add_field(fields, name, "hex_32_bytes");
    } else if (code == "b40") {
        add_field(fields, name, "hex_40_bytes");
    } else {
        return false;
    }
    return true;
}

/** Builds one row view contract: its declared fields, its row scope, and its stale-read rule. */
[[nodiscard]] Value row_view(const RowSpec& spec) {
    Value::Object fields;
    add_field(fields, "row", "u32_1_based_section");
    if (!for_each_field(spec.fields, [&fields](std::string_view code, std::string_view name) {
            return append_raw_field(fields, code, name);
        })) {
        return {};
    }
    if (spec.sectionIndex == 1) {
        add_field(fields, "activity_root_candidate_tags", "CatalogOwnedTagCollection");
        add_field(fields, "scenario_name_candidate_tags", "CatalogOwnedTagCollection");
        add_field(fields, "evidence_root_tags", "CatalogOwnedTagCollection");
        add_field(fields,
                  "binding_locators",
                  "CatalogOwnedRowCollection<CatalogActivityBindingLocatorView>");
    }
    sort_keys(fields);
    std::string scope("activity_sdk.runtime_pack.");
    scope.append(spec.section);
    return object({
        {"fields", object(std::move(fields))},
        {"immutable", boolean(true)},
        {"row_scope", string(scope)},
        {"stale_read", string("throws")},
    });
}

/** Describes a one-based collection with a count field and an at() method of the given return. */
[[nodiscard]] Value collection_contract(std::string_view returns) {
    return object({
        {"fields", object({{"count", string("u32")}})},
        {"index_base", number(1)},
        {"methods",
         object({{"at",
                  object({{"arguments", string_array({"row"})},
                          {"errors", string("throws_unavailable_or_stale")},
                          {"returns", string(returns)}})}})},
    });
}

/** Ledger entry mapping one pack section to the catalog collection surface it appears on. */
[[nodiscard]] Value direct_projection(const RowSpec& spec) {
    std::string surface("context.sdk.catalog.");
    surface.append(spec.collection);
    return object({
        {"projection_mode", string("row_collection")},
        {"section", string(spec.section)},
        {"section_index", number(spec.sectionIndex)},
        {"source_artifact", string("activity_sdk.runtime_pack")},
        {"surface", string(surface)},
        {"view", string(spec.view)},
    });
}

/** Ledger entry for the strings section; the view field appears only in the projection ledger. */
[[nodiscard]] Value strings_projection(bool includeView) {
    Value::Object fields{
        {"projection_mode", string("authenticated_parent_bounded_string_refs")},
        {"section", string("strings")},
        {"section_index", number(0)},
        {"source_artifact", string("activity_sdk.runtime_pack")},
        {"surface", string("context.sdk.catalog.<all 54 StringRef value/offset/length fields>")},
    };
    if (includeView) {
        fields.push_back({"view", string("decoded_utf8_string_or_nil_with_raw_StringRef")});
    }
    return object(std::move(fields));
}

/** Ledger entry for one activity-owned tag member of the activities collection. */
[[nodiscard]] Value tags_projection(std::string_view member) {
    std::string surface("context.sdk.catalog.activities[].");
    surface.append(member);
    return object({
        {"projection_mode", string("authenticated_activity_owned_tag_ranges")},
        {"section", string("activity_binding_tags")},
        {"section_index", number(kBindingTagSectionIndex)},
        {"source_artifact", string("activity_sdk.runtime_pack")},
        {"surface", string(surface)},
        {"view", string("u32_tag")},
    });
}

/** One coverage entry standing for all three activity-owned tag members. */
[[nodiscard]] Value tags_section_projection() {
    return object({
        {"projection_mode", string("authenticated_activity_owned_tag_ranges")},
        {"section", string("activity_binding_tags")},
        {"section_index", number(kBindingTagSectionIndex)},
        {"source_artifact", string("activity_sdk.runtime_pack")},
        {"surface",
         string("context.sdk.catalog.activities[].{activity_root_candidate_tags,"
                "scenario_name_candidate_tags,evidence_root_tags}")},
    });
}

[[nodiscard]] Value locator_enrichment_projection() {
    return object({
        {"projection_mode", string("authenticated_activity_owned_locator_range")},
        {"section", string("activity_binding_locators")},
        {"section_index", number(kBindingLocatorSectionIndex)},
        {"source_artifact", string("activity_sdk.runtime_pack")},
        {"surface", string("context.sdk.catalog.activities[].binding_locators")},
        {"view", string("CatalogActivityBindingLocatorView")},
    });
}

/** Collects every decoded string field path and fails unless the count matches the declared one. */
[[nodiscard]] bool decoded_string_fields(Value::Array& output) {
    output.clear();
    for (const RowSpec& spec : kRows) {
        if (!for_each_field(spec.fields,
                            [&output, &spec](std::string_view code, std::string_view name) {
                                if (code != "s") {
                                    return true;
                                }
                                std::string path("context.sdk.catalog.");
                                path.append(spec.collection);
                                path.append("[].");
                                path.append(name);
                                output.push_back(string(path));
                                return true;
                            })) {
            return false;
        }
    }
    return output.size() == kDecodedStringFieldCount;
}

/** Builds the top-level catalog view and one row view per section, sorted by name. */
[[nodiscard]] Value catalog_views() {
    Value::Object catalogFields{
        {"sdk_build_sha256", string("hex_sha256")},
        {"sdk_payload_sha256", string("hex_sha256")},
        {"content_key_sha256", string("hex_sha256")},
        {"logical_ir_sha256", string("hex_sha256")},
        {"activity_client_generation", string("u64_decimal_string")},
        {"activity_row", string("u32_1_based_section")},
        {"scenario_row", string("u32_1_based_section")},
        {"definition_hash", string("u32")},
        {"scenario_tag", string("u32")},
    };
    for (const RowSpec& spec : kRows) {
        std::string type("CatalogRowCollection<");
        type.append(spec.view);
        type.push_back('>');
        catalogFields.push_back({std::string(spec.collection), string(type)});
    }
    sort_keys(catalogFields);

    Value::Object views;
    views.push_back({"CatalogView",
                     object({
                         {"fields", object(std::move(catalogFields))},
                         {"immutable", boolean(true)},
                         {"row_scope", string("exact_bound_runtime_pack_generation")},
                         {"stale_read", string("throws")},
                     })});
    for (const RowSpec& spec : kRows) {
        views.push_back({std::string(spec.view), row_view(spec)});
    }
    sort_keys(views);
    return object(std::move(views));
}

/** One entry per projected surface, in section order, with the tag and locator entries inserted. */
[[nodiscard]] Value projection_ledger() {
    Value::Array output;
    output.reserve(kRows.size() + 5);
    output.push_back(strings_projection(true));
    for (const RowSpec& spec : kRows) {
        // The tag section has no direct collection, so its entries take its place in section order.
        if (spec.sectionIndex == kBindingLocatorSectionIndex) {
            output.push_back(tags_projection("activity_root_candidate_tags"));
            output.push_back(tags_projection("evidence_root_tags"));
            output.push_back(tags_projection("scenario_name_candidate_tags"));
            output.push_back(locator_enrichment_projection());
        }
        output.push_back(direct_projection(spec));
    }
    return array(std::move(output));
}

/** One entry per retained pack section, so every section is accounted for once. */
[[nodiscard]] Value section_coverage_ledger() {
    Value::Array output;
    output.reserve(kRows.size() + 2);
    output.push_back(strings_projection(false));
    for (const RowSpec& spec : kRows) {
        // The tag section has no direct collection, so its entry takes its place in section order.
        if (spec.sectionIndex == kBindingLocatorSectionIndex) {
            output.push_back(tags_section_projection());
        }
        output.push_back(direct_projection(spec));
    }
    return array(std::move(output));
}

} // namespace

/** Builds the catalog contract value; false if a string field check or an allocation fails. */
bool build_catalog_sdk_contract_value(Value& output) noexcept {
    try {
        Value::Array decodedFields;
        if (!decoded_string_fields(decodedFields)) {
            output = {};
            return false;
        }
        output = object({
            {"collection_contracts",
             object({
                 {"activity_owned_rows", collection_contract("declared_view")},
                 {"activity_owned_tags", collection_contract("u32_tag")},
                 {"rows", collection_contract("declared_view")},
             })},
            {"direct_collection_count", number(kRows.size())},
            {"exact_generation",
             object({
                 {"handle_fields",
                  string_array({"sdk_build_sha256",
                                "sdk_payload_sha256",
                                "content_key_sha256",
                                "logical_ir_sha256",
                                "activity_client_generation",
                                "activity_row",
                                "scenario_row",
                                "definition_hash",
                                "scenario_tag"})},
                 {"read_policy", string("revalidate_all_fields_before_every_borrowed_row_read")},
                 {"stale_policy", string("throw_without_rebinding")},
             })},
            {"projection_ledger", projection_ledger()},
            {"projection_scope", string("all_retained_runtime_pack_sections")},
            {"schema", string("sunrise-runtime-pack-lua-catalog-v1")},
            {"section_count", number(format::kSectionCount)},
            {"section_coverage_ledger", section_coverage_ledger()},
            {"string_bank_coverage",
             object({
                 {"absent_reference",
                  object({{"decoded_value", string("nil")},
                          {"length", number(0)},
                          {"offset", number(0xffffffffU)}})},
                 {"decoded_field_count", number(decodedFields.size())},
                 {"decoded_fields", array(std::move(decodedFields))},
                 {"declared_value_offset_length_field_count",
                  number(kDecodedStringFieldCount * 3U)},
                 {"empty_reference",
                  object({{"decoded_value", string("empty_string")},
                          {"length", number(0)},
                          {"offset", number(0)}})},
                 {"interval_partition_proof",
                  object({
                      {"deduplicate_by", string_array({"offset", "length"})},
                      {"final_end", string("authenticated_strings_section_size")},
                      {"first_offset", number(0)},
                      {"include", string("all_nonempty_bounded_StringRef_intervals")},
                      {"ordering", string("offset_then_length")},
                      {"require_adjacent", boolean(true)},
                      {"verified_by",
                       string_array(
                           {"native_runtime_pack_artifact_test", "python_runtime_pack_test"})},
                  })},
                 {"reference_field_count", number(kDecodedStringFieldCount)},
                 {"section", string("strings")},
                 {"section_index", number(0)},
             })},
            {"views", catalog_views()},
        });
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
