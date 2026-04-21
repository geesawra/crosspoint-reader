#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Top-level Instapaper screen: lists the Unread / Starred / Archive folders.
 *
 * Phase A (current): static list, shows token-loading state and missing-token
 *   error. Confirm is a no-op stub until InstapaperArticleListActivity lands.
 * Phase B: will push the article list on Confirm.
 */
class InstapaperFolderActivity final : public Activity {
 public:
  explicit InstapaperFolderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("InstapaperFolder", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State : uint8_t {
    LOADING,
    NO_TOKENS,
    READY,
  };

  State state = LOADING;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
};
