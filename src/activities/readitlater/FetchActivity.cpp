#include "FetchActivity.h"

#include <EpubBuilder.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void FetchActivity::onEnter() {
  Activity::onEnter();

  if (!provider) {
    finish();
    return;
  }

  // If the article is already cached on SD, skip the API call and jump
  // straight to the reader. Flush button state first so the Confirm that
  // got us here doesn't ride through to the reader's first loop.
  const std::string cached = provider->epubPathFor(article.id, article.title);
  if (Storage.exists(cached.c_str())) {
    LOG_DBG("RIL", "Using cached EPUB: %s", cached.c_str());
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

void FetchActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    finish();
    return;
  }
  state = FETCHING_TEXT;
  requestUpdate();
  performWork();
}

void FetchActivity::onExit() { Activity::onExit(); }

void FetchActivity::performWork() {
  {
    RenderLock lock(*this);
    state = FETCHING_TEXT;
  }
  requestUpdateAndWait();

  // Ensure cache directory exists before trying to create temp files in it.
  char cacheDir[64];
  std::snprintf(cacheDir, sizeof(cacheDir), "/.crosspoint/%s", provider->cacheDirName());
  Storage.mkdir(cacheDir);

  // Stream HTML directly to SD — no in-memory accumulation.
  char htmlTmpBuf[128];
  std::snprintf(htmlTmpBuf, sizeof(htmlTmpBuf), "%s/tmp_html_%llu", cacheDir,
                static_cast<unsigned long long>(article.id));
  const std::string htmlTmpPath(htmlTmpBuf);

  const auto fetchResult = provider->fetchText(article.id, htmlTmpPath);
  if (!fetchResult.isOk()) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = Provider::errorString(fetchResult.code);
    errorDetail = fetchResult.message;
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

  const std::string outPath = provider->epubPathFor(article.id, article.title);
  const std::string path = EpubBuilder::build(
      provider->cacheDirName(), provider->coverPngData(), provider->coverPngLen(), article.id, article.title,
      article.author, htmlTmpPath.c_str(), &onBuildProgress, this, SETTINGS.readItLaterImages, outPath.c_str());

  // HTML temp file is no longer needed regardless of outcome.
  Storage.remove(htmlTmpPath.c_str());

  if (path.empty()) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = "EPUB build failed";
    requestUpdate(true);
    return;
  }

  // Drain any button-edge events accumulated while fetch+build blocked.
  mappedInput.update();
  delay(10);
  mappedInput.update();

  epubPath = path;
  activityManager.goToReader(epubPath);
}

void FetchActivity::onBuildProgress(void* ctx, int percent, const char* label) {
  auto* self = static_cast<FetchActivity*>(ctx);
  {
    RenderLock lock(*self);
    self->buildPercent = percent;
    if (label) {
      self->buildLabel = label;
    }
  }
  self->requestUpdate(true);
}

void FetchActivity::loop() {
  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}

void FetchActivity::render(RenderLock&&) {
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
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, errorMessage.c_str(), true,
                              EpdFontFamily::BOLD);
    if (!errorDetail.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 5, errorDetail.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
