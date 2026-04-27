#pragma once

#include <ctime>
#include <vector>

#include "Provider.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Shows the article list for a provider folder. Acquires WiFi on entry
 * if not already connected, fetches the list over the provider API,
 * and displays it in a scrollable list with title + domain subtitle.
 */
class ArticleListActivity final : public Activity {
 public:
  ArticleListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Provider* provider,
                      Provider::FolderInfo folder)
      : Activity("ArticleList", renderer, mappedInput), provider(provider), folder(folder) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == FETCHING; }

 private:
  enum State : uint8_t {
    WIFI_SELECTION,
    FETCHING,
    SHOWING_LIST,
    EMPTY_LIST,
    FETCH_FAILED,
  };

  Provider* provider = nullptr;
  Provider::FolderInfo folder;
  State state = WIFI_SELECTION;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
  std::vector<ReadItLaterArticle> articles;
  std::string errorMessage;
  time_t lastSyncedAt = 0;
  bool offline = false;
  bool pendingRefresh = false;

  void onWifiSelectionComplete(bool success);
  void performFetch(bool showProgress = false);
  void openSelected();
};
