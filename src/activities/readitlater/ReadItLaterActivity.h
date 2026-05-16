#pragma once

#include <memory>
#include <vector>

#include "Provider.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Top-level Read-it-Later screen: lists configured providers.
 *
 * Scans the SD card for provider credential files and shows only
 * providers that are successfully configured. Selecting a provider
 * pushes ProviderFolderActivity for that provider.
 */
class ReadItLaterActivity final : public Activity {
 public:
  explicit ReadItLaterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadItLater", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State : uint8_t {
    SCANNING,
    NO_PROVIDERS,
    SHOWING_LIST,
  };

  State state = SCANNING;
  int selectedIndex = 0;
  std::vector<std::unique_ptr<Provider>> providers;
  ButtonNavigator buttonNavigator;

  void discoverProviders();
};
