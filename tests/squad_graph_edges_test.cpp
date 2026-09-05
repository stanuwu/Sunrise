#include <cassert>
#include <cstdint>
#include <iostream>
#include "../Sunrise/src/client/content/activity/activity_sdk_squad_graph_internal.h"
#include "../Sunrise/src/middleware/content/packages/tables/authored_squad_reader.h"

namespace inventory = sunrise::client::content::activity::sdk_generation::squad_inventory;
namespace topology = sunrise::client::content::activity::sdk_generation::topology_inventory;
namespace tables = sunrise::middleware::content::packages::tables;

// Exercise the production graph join, including decoding and descriptor resolution.
struct Fixture {
    topology::Snapshot topology;
    inventory::GraphSnapshot graph;

    static std::uint64_t reference(std::uint16_t slot) {
        return 0x12345678ULL | (std::uint64_t{tables::kAuthoredSquadRuleSlotType} << 32)
               | (std::uint64_t{slot} << 48);
    }

    Fixture() {
        topology.objects.resize(1);
        topology.objects[0].objectTag = 0x80000001;
        topology.objects[0].objectKey = 0x12345678;
        topology.occurrences.resize(1);
        topology.occurrences[0].objectIndex = 0;
        topology.occurrences[0].scenarioIndex = 0;
        topology.slots.resize(3);
        graph.descriptors.resize(3);
        for (std::uint32_t i = 0; i < 3; ++i) {
            auto& slot = topology.slots[i];
            slot.objectIndex = 0;
            slot.slotIndex = i;
            slot.slotType = i == 0 ? tables::kAuthoredSquadSpawnerSlotType
                                  : tables::kAuthoredSquadRuleSlotType;
            slot.id.value[0] = static_cast<char>('a' + i);
            slot.id.length = 1;
            auto& descriptor = graph.descriptors[i];
            descriptor.id = std::to_string(i);
            descriptor.configTag = i == 0 ? 0x80BE932D : (i == 1 ? 0x80BE9312 : 0x80BE9316);
            descriptor.objectIndex = 0;
            descriptor.slotIndex = i;
            descriptor.componentClass = i == 0 ? tables::kAuthoredSquadSpawnerPrimaryClass
                                               : tables::kAuthoredSquadRulePrimaryClass;
            descriptor.complete = true;
        }
        graph.spawners.resize(1);
        auto& spawner = graph.spawners[0];
        spawner.id = "spawner";
        spawner.configTag = 0x80BE932D;
        spawner.rawReference98 = reference(1);
        spawner.rawReferenceA0 = reference(2);
        spawner.complete = true;
        graph.rules.resize(2);
        graph.points.resize(2);
        graph.pointContexts.resize(2);
        graph.configContexts.resize(2);
        for (std::uint32_t i = 0; i < 2; ++i) {
            graph.rules[i].id = "rule" + std::to_string(i);
            graph.rules[i].configTag = graph.descriptors[i + 1].configTag;
            graph.rules[i].points = {i, 1};
            graph.rules[i].complete = true;
            graph.points[i].contexts = {i, 1};
            graph.pointContexts[i].pointRow = i;
            graph.pointContexts[i].configContextRow = i;
            graph.pointContexts[i].matches.count = 1;
            graph.pointContexts[i].status = inventory::PointContextStatus::exact;
            graph.configContexts[i].ruleRow = i;
            graph.configContexts[i].objectIndex = 0;
            graph.configContexts[i].scenarioIndex = 0;
        }
    }

    void run() {
        inventory::Facts facts;
        facts.spawners.resize(graph.spawners.size());
        facts.rules.resize(graph.rules.size());
        facts.descriptors.resize(graph.descriptors.size());
        assert(inventory::detail::build_graph_edges(topology, facts, graph));
    }
};

int main() {
    {
        Fixture f;
        f.run();
        assert(f.graph.edges.size() == 2);
        for (const auto& edge : f.graph.edges) {
            assert(edge.associationExact);
            assert(edge.scenarioContexts.count == 1);
            assert(edge.referenceMask == (1U << edge.ruleRow));
        }
    }
    {
        Fixture f;
        f.graph.spawners[0].rawReferenceA0 = 0;
        f.run();
        assert(f.graph.edges.size() == 1 && f.graph.edges[0].associationExact);
    }
    {
        Fixture f;
        f.graph.spawners[0].rawReferenceA0 = Fixture::reference(1);
        f.run();
        assert(f.graph.edges.size() == 1 && f.graph.edges[0].associationExact);
        assert(f.graph.edges[0].referenceMask == 3);
    }
    {
        Fixture f;
        f.graph.spawners[0].rawReference98 = 0;
        f.graph.spawners[0].rawReferenceA0 = 0;
        f.graph.spawners[0].hasInlinePointSet = true;
        f.graph.spawners[0].inlineRuleRow = 0;
        f.graph.rules[0].inlineForm = true;
        f.run();
        assert(f.graph.edges.size() == 1 && f.graph.edges[0].associationExact);
        assert(f.graph.edges[0].referenceMask == 0);
    }
    {
        Fixture f;
        f.graph.spawners[0].rawReferenceA0 = Fixture::reference(99);
        f.run();
        assert(f.graph.edges.size() == 1 && !f.graph.edges[0].associationExact);
        assert(f.graph.references[1].status == inventory::ReferenceResolutionStatus::targetMissing);
    }
    {
        Fixture f;
        f.graph.descriptors[2].componentClass = 0;
        f.run();
        assert(f.graph.edges.size() == 1 && !f.graph.edges[0].associationExact);
        assert(f.graph.references[1].status == inventory::ReferenceResolutionStatus::targetDescriptorMismatch);
    }
    {
        Fixture f;
        auto duplicate = f.graph.descriptors[2];
        duplicate.id = "conflicting-rule";
        duplicate.configTag = f.graph.rules[0].configTag;
        f.graph.descriptors.push_back(duplicate);
        f.run();
        assert(f.graph.edges.size() == 1 && !f.graph.edges[0].associationExact);
        assert(f.graph.references[1].status == inventory::ReferenceResolutionStatus::targetAmbiguous);
    }
    {
        Fixture f;
        f.graph.descriptors.push_back(f.graph.descriptors[0]);
        f.run();
        assert(f.graph.edges.empty());
        assert(f.graph.spawners[0].sourceDescriptorStatus == inventory::SourceDescriptorStatus::ambiguous);
    }
    std::cout << "squad graph edges: all regression cases passed\n";
}
