#pragma once

#include <cstdint>

struct HumveeCrewSeat {
    static constexpr int Capacity = 3;
    static constexpr uint8_t NoSeat = 0xFF;
    int vehicle = -1;
    int seat = -1; // 0 gunner, 1 front passenger, 2 rear passenger

    bool Mounted() const { return vehicle >= 0 && seat >= 0 && seat < Capacity; }
    bool Gunner() const { return Mounted() && seat == 0; }
    void Clear() { vehicle = seat = -1; }

    bool TryBoard(int vehicleIndex, uint8_t occupiedSeats) {
        if (Mounted() || vehicleIndex < 0 || vehicleIndex >= NoSeat) return false;
        for (int candidate = 0; candidate < Capacity; ++candidate) {
            if ((occupiedSeats & (1u << candidate)) != 0) continue;
            vehicle = vehicleIndex;
            seat = candidate;
            return true;
        }
        return false;
    }

    void ApplyNetwork(uint8_t vehicleIndex, uint8_t seatIndex) {
        Clear();
        if (vehicleIndex == NoSeat || seatIndex >= Capacity) return;
        vehicle = vehicleIndex;
        seat = seatIndex;
    }
};
