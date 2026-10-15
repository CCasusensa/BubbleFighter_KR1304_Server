#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_TRAINING_CLAN_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_TRAINING_CLAN_H_

#include "features/clans/clan_mutation.h"
#include "protocol/clans/clan_training_messages.h"

namespace bubble_fighter::server::features::clans {
enum class TrainingClanAction {
  kJoin,
  kManualLeave,
  kGraduate,
  kReconcile,
  kUnsupported
};
struct TrainingClanResult {
  bool handled = true;
  bool graduated = false;
  ClanMutationResult mutation;
};
using TrainingClanHandler = std::function<TrainingClanResult(
    const ClanIdentity&, TrainingClanAction, const ClanMutationAuthority&)>;
using TrainingClanReconciler = std::function<bool(const ClanIdentity&)>;
}  // namespace bubble_fighter::server::features::clans

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_TRAINING_CLAN_H_
