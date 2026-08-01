#ifndef GAME_COMMAND_QUEUE_H
#define GAME_COMMAND_QUEUE_H

#include <bitset>
#include <cstddef>

enum class GameCommand : std::size_t {
    ResetLevelRuntime,
    RespawnTurretGunner,
    RespawnBoatGunner,
    EditorBeginPlay,
    EditorStopPlay,
    EditorReturnToMenu,
    RebuildDDGI,
    ResetDDGIHistory,
    Count
};

class GameCommandQueue {
public:
    void Request(GameCommand command) { Set(command, true); }

    void Set(GameCommand command, bool pending) {
        commands_.set(Index(command), pending);
    }

    bool Pending(GameCommand command) const {
        return commands_.test(Index(command));
    }

    bool Consume(GameCommand command) {
        const std::size_t index = Index(command);
        const bool pending = commands_.test(index);
        commands_.reset(index);
        return pending;
    }

    void Clear() { commands_.reset(); }

private:
    static constexpr std::size_t Index(GameCommand command) {
        return static_cast<std::size_t>(command);
    }

    std::bitset<static_cast<std::size_t>(GameCommand::Count)> commands_;
};

#endif
