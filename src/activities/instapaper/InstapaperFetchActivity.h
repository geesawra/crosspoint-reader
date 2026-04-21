#pragma once
#include <InstapaperArticle.h>

#include "activities/Activity.h"

/**
 * Fetches an Instapaper article's text over the API, synthesizes a minimal
 * EPUB on SD, and transitions to the standard EpubReaderActivity.
 *
 * Entered with WiFi already connected — launched from InstapaperArticleList
 * which performs the WiFi acquisition.
 */
class InstapaperFetchActivity final : public Activity {
 public:
  InstapaperFetchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, InstapaperArticle article)
      : Activity("InstapaperFetch", renderer, mappedInput), article(article) {}

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

  InstapaperArticle article;
  State state = FETCHING_TEXT;
  std::string errorMessage;
  std::string epubPath;
  int buildPercent = 0;
  std::string buildLabel;

  void performWork();
  void onWifiSelectionComplete(bool success);
  static void onBuildProgress(void* ctx, int percent, const char* label);
};
