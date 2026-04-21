#pragma once
#include <InstapaperArticle.h>

#include <vector>

#include "activities/Activity.h"

/**
 * Bulk-downloads the text for every article in a provided list, building the
 * cached EPUB for each. Drives a progress bar. Useful before going offline
 * (e.g. a commute).
 *
 * Entered with WiFi already connected (caller is InstapaperArticleListActivity
 * which has already performed WiFi acquisition).
 */
class InstapaperDownloadAllActivity final : public Activity {
 public:
  InstapaperDownloadAllActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                std::vector<InstapaperArticle> articles)
      : Activity("InstapaperDownloadAll", renderer, mappedInput), articles(std::move(articles)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == RUNNING; }

 private:
  enum State : uint8_t {
    RUNNING,
    DONE,
    CANCELLED,
  };

  std::vector<InstapaperArticle> articles;
  State state = RUNNING;
  volatile bool cancelRequested = false;
  int completed = 0;
  int skipped = 0;
  int failed = 0;

  void performDownloads();
};
