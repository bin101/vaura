#include "charging_decision.h"

bool updateChargingDecision(ChargingDecisionState &state, int16_t currentMa, uint32_t nowMs,
                             int16_t enterMa, int16_t exitMa, uint32_t minDwellMs) {
  if (!state.charging) {
    if (currentMa >= enterMa) {
      state.charging = true;
      state.lastAboveExitMs = nowMs;
    }
    return state.charging;
  }

  if (currentMa >= exitMa) {
    // Still charging at a meaningful rate -- reset the dwell clock.
    state.lastAboveExitMs = nowMs;
  } else if (nowMs - state.lastAboveExitMs >= minDwellMs) {
    // Current has stayed below the exit threshold for the full dwell window.
    state.charging = false;
  }
  return state.charging;
}
