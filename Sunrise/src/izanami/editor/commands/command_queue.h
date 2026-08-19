#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../../core/ids.h"
#include "../../core/transform.h"
#include "../../project/scene/forge_object.h"

namespace sunrise::izanami::editor::commands {

enum class CommandKind : std::uint8_t {
    createObject,
    deleteObject,
    setTransform,
    renameObject,
    setObjectKind,
    setResource,
    setEditorFlags,
    reparentObject,
};

struct Command {
    CommandKind kind{CommandKind::createObject};
    core::ForgeUUID object{};
    core::ResourceId resource{};
    core::ObjectKind objectKind{core::ObjectKind::forgeOnly};
    core::Transform transform{};
    core::ForgeUUID parent{};
    std::string editorName{};
    bool editorVisible{true};
    bool editorLocked{};
    std::optional<project::scene::ForgeObject> objectSnapshot{};
};

struct Transaction {
    std::vector<Command> redoCommands{};
    std::vector<Command> undoCommands{};
    std::string author{};
    std::string label{};
    std::uint64_t timestamp{};
};

class CommandQueue final {
public:
    void push(Command command);
    [[nodiscard]] bool try_pop(Command& command);
    void clear();
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_{};
    std::deque<Command> queue_{};
};

class TransactionHistory final {
public:
    void commit(Transaction transaction);
    [[nodiscard]] bool can_undo() const;
    [[nodiscard]] bool can_redo() const;
    [[nodiscard]] std::size_t undo_count() const;
    [[nodiscard]] std::size_t redo_count() const;
    [[nodiscard]] bool undo(Transaction& transaction);
    [[nodiscard]] bool redo(Transaction& transaction);
    void clear();

private:
    std::vector<Transaction> undo_{};
    std::vector<Transaction> redo_{};
};

} // namespace sunrise::izanami::editor::commands
