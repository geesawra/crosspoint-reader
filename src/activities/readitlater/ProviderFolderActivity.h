#pragma once

#include <vector>

#include "Provider.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Shows the folder list for a provider (e.g. Unread / Starred / Archive).
 * Selecting a folder pushes ArticleListActivity.
 */
class ProviderFolderActivity final : public Activity {
 public:
  ProviderFolderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Provider* provider)
      : Activity("ProviderFolder", renderer, mappedInput), provider(provider) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  Provider* provider = nullptr;
  std::vector<Provider::FolderInfo> folders;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
};
