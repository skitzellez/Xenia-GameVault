// Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
// PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
// Edge plays its achievement sound with miniaudio, which the Xbox app's API set lacks (COM desktop calls): on the
// Xbox the achievement notice shows without the sound.
#include "xenia/ui/audio_helper.h"

namespace xe {
namespace ui {

AudioHelper& AudioHelper::Instance() {
  static AudioHelper helper;
  return helper;
}

AudioHelper::AudioHelper() = default;
AudioHelper::~AudioHelper() = default;

void AudioHelper::PlayAchievementSound() {}

}  // namespace ui
}  // namespace xe
