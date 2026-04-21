#pragma once
#include <InstapaperArticle.h>
#include <InstapaperClient.h>

#include <ctime>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Shows the bookmark list for an Instapaper folder. Acquires WiFi on entry
 * (via WifiSelectionActivity) if not already connected, fetches the list
 * over the Instapaper API, and displays it in a scrollable list with
 * title + domain subtitle.
 *
 * On Confirm: pushes InstapaperFetchActivity for the selected article.
 */
class InstapaperArticleListActivity final : public Activity {
 public:
  InstapaperArticleListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, InstapaperFolder folder)
      : Activity("InstapaperArticleList", renderer, mappedInput), folder(folder) {}

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

  InstapaperFolder folder;
  State state = WIFI_SELECTION;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
  std::vector<InstapaperArticle> articles;
  std::string errorMessage;
  time_t lastSyncedAt = 0;
  bool offline = false;  // true when the list is shown from cache w/o a live refresh

  void onWifiSelectionComplete(bool success);
  void performFetch();
  void openSelected();
  const char* folderTitleKey() const;
};
