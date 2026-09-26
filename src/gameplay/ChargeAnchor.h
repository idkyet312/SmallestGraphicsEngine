#ifndef SGE_CHARGE_ANCHOR_H
#define SGE_CHARGE_ANCHOR_H

#include <cstdint>

namespace SGE {

enum class ChargeAnchorPart : uint8_t {
    World = 0,
    AATurretBase,
    AATurretTraverse,
    AATurretElevation,
};

inline bool ValidChargeAnchorPart(ChargeAnchorPart part) {
    return part >= ChargeAnchorPart::World &&
           part <= ChargeAnchorPart::AATurretElevation;
}

} // namespace SGE

#endif
