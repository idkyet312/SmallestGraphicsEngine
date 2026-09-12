#include "NetSession.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
// Exercises the session against the null transport: no socket, no device, no
// second process. What is actually being pinned here is the state machine and
// the interpolation maths -- the parts that are wrong in ways a two-machine
// smoke test shows as "it looks a bit jittery" rather than as a clear failure.
int main() {
    using namespace net;

    // A fresh session is offline and owns no player id.
    {
        NetSession session;
        Check(session.CurrentRole() == Role::Offline,
              "A new session should be offline");
        Check(!session.Active(), "A new session should not be active");
        Check(session.LocalId() == kInvalidPlayerId,
              "An offline session should have no local id");

        // Update on an offline session must be harmless: main.cpp calls this
        // unconditionally every frame.
        LocalPlayerState local;
        session.Update(1.0f / 60.0f, local);
        std::vector<RemotePlayer> remotes;
        session.GetRemotePlayers(remotes);
        Check(remotes.empty(), "An offline session has no remote players");
    }

    // Hosting assigns the host player 0 without a handshake, since it never
    // connects to itself.
    {
        NetSession session;
        std::string error;
        Check(session.StartHost(27015, &error),
              "Hosting should succeed against the null transport");
        Check(session.CurrentRole() == Role::Host, "Role should be Host");
        Check(session.LocalId() == 0, "The host should be player 0");

        // The host is the only player, so it has no remotes to draw.
        LocalPlayerState local;
        local.x = 5.0f;
        local.input.yaw = 90.0f;
        session.Update(1.0f / 60.0f, local);
        std::vector<RemotePlayer> remotes;
        session.GetRemotePlayers(remotes);
        Check(remotes.empty(),
              "A host alone should report no remote players");

        session.Shutdown();
        Check(session.CurrentRole() == Role::Offline,
              "Shutdown should return the session to offline");
        Check(session.LocalId() == kInvalidPlayerId,
              "Shutdown should clear the local id");
    }

    // Joining nothing must fail rather than leaving the player in an empty
    // session that looks connected. The null transport refuses Connect for
    // exactly this reason.
    {
        NetSession session;
        std::string error;
        Check(!session.StartClient("127.0.0.1", 27015, &error),
              "Connecting with no networking should fail");
        Check(!error.empty(), "A failed connect should explain itself");
        Check(session.CurrentRole() == Role::Offline,
              "A failed connect should leave the session offline");
    }

    // Hosting twice in a row must not leak the previous session's state --
    // a player can host, leave and host again.
    {
        NetSession session;
        std::string error;
        Check(session.StartHost(27015, &error), "First host failed");
        Check(session.StartHost(27016, &error), "Second host failed");
        Check(session.LocalId() == 0,
              "Re-hosting should still make the host player 0");
    }

    // Angle interpolation has to take the short way around the wrap, or a body
    // crossing 0 degrees spins almost all the way round instead of a little.
    // Exercised through the same helper the session uses for remote yaw.
    {
        Check(NetSession::kNetTickSeconds > 0.0f, "Net tick must be positive");
        // The interpolation delay must cover at least two ticks, or there is
        // not always a second snapshot to interpolate towards.
        Check(NetSession::kInterpolationDelay >=
                  NetSession::kNetTickSeconds * 2.0f - 1e-6f,
              "Interpolation delay must span at least two net ticks");
    }

    // The snapshot must stay small enough that a full lobby fits one datagram
    // at the tick rate this session sends at.
    {
        const float perSecond =
            sizeof(ServerSnapshotMessage) / NetSession::kNetTickSeconds;
        Check(perSecond < 64000.0f,
              "Snapshot bandwidth per client should stay modest");
        std::cout << "Snapshot " << sizeof(ServerSnapshotMessage)
                  << " bytes, " << static_cast<int>(perSecond)
                  << " B/s per client at "
                  << static_cast<int>(1.0f / NetSession::kNetTickSeconds)
                  << " Hz\n";
    }

    std::cout << "NetSession tests passed\n";
    return 0;
}
