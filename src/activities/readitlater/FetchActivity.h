#pragma once
#include <ReadItLaterArticle.h>

#include "Provider.h"
#include "activities/Activity.h"

/**
 * Fetches a Read-it-Later article's text over the provider API, synthesizes a minimal
 * EPUB on SD, and transitions to the standard EpubReaderActivity.
 *
 * Entered with WiFi already connected — launched from ArticleListActivity
 * which performs the WiFi acquisition.
 */
class FetchActivity final : public Activity {
 public:
  FetchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Provider* provider,
                ReadItLaterArticle article)
      : Activity("Fetch", renderer, mappedInput), provider(provider), article(article) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == FETCHING_TEXT || state == BUILDING_EPUB; }

 private:
  enum State : uint8_t {
    WIFI_SELECTION,
    FETCHING_TEXT,
    BUILDING_EPUB,
    FAILED,
  };

  Provider* provider = nullptr;
  ReadItLaterArticle article;
  State state = FETCHING_TEXT;
  std::string errorMessage;
  std::string epubPath;
  int buildPercent = 0;
  std::string buildLabel;

  void performWork();
  void onWifiSelectionComplete(bool success);
  static void onBuildProgress(void* ctx, int percent, const char* label);
};
