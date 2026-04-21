#include "InstapaperFetchActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InstapaperClient.h>
#include <InstapaperEpubBuilder.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void InstapaperFetchActivity::onEnter() {
  Activity::onEnter();

  // If the article is already cached on SD, skip the API call and jump
  // straight to the reader.
  const std::string cached = InstapaperEpubBuilder::pathFor(article.id);
  if (Storage.exists(cached.c_str())) {
    LOG_DBG("INSTA", "Using cached EPUB: %s", cached.c_str());
    activityManager.goToReader(cached);
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
  }
  requestUpdateAndWait();

  const std::string path = InstapaperEpubBuilder::build(article.id, article.title, article.author, html.c_str());
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

  epubPath = path;
  activityManager.goToReader(epubPath);
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
  const auto pageHeight = renderer.getScreenHeight();

  if (state == FETCHING_TEXT) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_FETCHING), true, EpdFontFamily::BOLD);
  } else if (state == BUILDING_EPUB) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_BUILDING), true, EpdFontFamily::BOLD);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_INSTAPAPER_AUTH_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
