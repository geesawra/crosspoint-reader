#pragma once
#include <ReadItLaterArticle.h>

#include <vector>

#include "Provider.h"
#include "activities/Activity.h"

/**
 * Bulk-downloads the text for every article in a provided list, building the
 * cached EPUB for each. Drives a progress bar. Useful before going offline
 * (e.g. a commute).
 *
 * Entered with WiFi already connected (caller is ArticleListActivity
 * which has already performed WiFi acquisition).
 */
class DownloadAllActivity final : public Activity {
 public:
  DownloadAllActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Provider* provider,
                      std::vector<ReadItLaterArticle> articles)
      : Activity("DownloadAll", renderer, mappedInput), provider(provider), articles(std::move(articles)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == RUNNING; }

 private:
  enum State : uint8_t {
    WIFI_SELECTION,
    RUNNING,
    DONE,
    CANCELLED,
  };

  Provider* provider = nullptr;
  std::vector<ReadItLaterArticle> articles;
  State state = RUNNING;
  volatile bool cancelRequested = false;
  int completed = 0;
  int skipped = 0;
  int failed = 0;

  void performDownloads();
  void onWifiSelectionComplete(bool success);
};
