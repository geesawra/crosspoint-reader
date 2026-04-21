#include "InstapaperFetchActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InstapaperClient.h>
#include <InstapaperEpubBuilder.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void InstapaperFetchActivity::onEnter() {
  Activity::onEnter();

  // If the article is already cached on SD, skip the API call and jump
  // straight to the reader. Flush button state first so the Confirm that
  // got us here doesn't ride through to the reader's first loop.
  const std::string cached = InstapaperEpubBuilder::pathFor(article.id);
  if (Storage.exists(cached.c_str())) {
    LOG_DBG("INSTA", "Using cached EPUB: %s", cached.c_str());
    mappedInput.update();
    delay(10);
    mappedInput.update();
    activityManager.goToReader(cached);
    return;
  }

  // Network work ahead — require WiFi. Without it, the lwIP core mutex may
  // not be initialized yet (cached-list path lets the user reach here
  // offline), and any DNS lookup would assert inside lwIP.
  if (WiFi.status() != WL_CONNECTED) {
    state = WIFI_SELECTION;
    requestUpdate();
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& r) { onWifiSelectionComplete(!r.isCancelled); });
    return;
  }

  state = FETCHING_TEXT;
  requestUpdate();
  performWork();
}

void InstapaperFetchActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    finish();
    return;
  }
  state = FETCHING_TEXT;
  requestUpdate();
  performWork();
}

void InstapaperFetchActivity::onExit() { Activity::onExit(); }

void InstapaperFetchActivity::performWork() {
  {
    RenderLock lock(*this);
    state = FETCHING_TEXT;
  }
  requestUpdateAndWait();

  std::string html;
  const auto fetchResult = InstapaperClient::getText(article.id, html);
  if (fetchResult != InstapaperClient::OK) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = InstapaperClient::errorString(fetchResult);
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock(*this);
    state = BUILDING_EPUB;
    buildPercent = 0;
    buildLabel = tr(STR_INSTAPAPER_BUILDING);
  }
  requestUpdateAndWait();

  const std::string path =
      InstapaperEpubBuilder::build(article.id, article.title, article.author, html.c_str(), &onBuildProgress, this);
  if (path.empty()) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = "EPUB build failed";
    requestUpdate(true);
    return;
  }

  // Free the HTML buffer before entering the reader to maximize headroom
  // for the EPUB indexing pass.
  html.clear();
  html.shrink_to_fit();

  // Drain any button-edge events accumulated while fetch+build blocked.
  // Without this, the Confirm press→release edge from the user tapping to
  // open the article fires on the reader's first loop() and immediately
  // opens the reader menu. Two updates settle prev==curr so no fresh edge
  // is visible to the reader.
  mappedInput.update();
  delay(10);
  mappedInput.update();

  epubPath = path;
  activityManager.goToReader(epubPath);
}

void InstapaperFetchActivity::onBuildProgress(void* ctx, int percent, const char* label) {
  auto* self = static_cast<InstapaperFetchActivity*>(ctx);
  {
    RenderLock lock(*self);
    self->buildPercent = percent;
    if (label) {
      self->buildLabel = label;
    }
  }
  self->requestUpdate(true);
}

void InstapaperFetchActivity::loop() {
  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}

void InstapaperFetchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == WIFI_SELECTION) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_FETCHING), true, EpdFontFamily::BOLD);
  } else if (state == FETCHING_TEXT) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_FETCHING), true, EpdFontFamily::BOLD);
  } else if (state == BUILDING_EPUB) {
    // Phase label, then centered progress bar. A static "Building EPUB..."
    // screen looked hung on slow SD cards / articles with several images —
    // the bar gives the user a clear "still working" signal.
    const char* label = buildLabel.empty() ? tr(STR_INSTAPAPER_BUILDING) : buildLabel.c_str();
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, label, true, EpdFontFamily::BOLD);

    const int barWidth = pageWidth - 80;
    GUI.drawProgressBar(renderer, Rect{40, pageHeight / 2 + 10, barWidth, 12}, buildPercent, 100);

    char pctStr[8];
    std::snprintf(pctStr, sizeof(pctStr), "%d%%", buildPercent);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, pctStr);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, tr(STR_INSTAPAPER_AUTH_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_AUTH_HINT));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
