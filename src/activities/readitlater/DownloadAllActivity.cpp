#include "DownloadAllActivity.h"

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

void DownloadAllActivity::onEnter() {
  Activity::onEnter();
  completed = 0;
  skipped = 0;
  failed = 0;
  cancelRequested = false;

  if (!provider) {
    finish();
    return;
  }

  // WiFi is mandatory here — launching straight into fetchText() without it
  // triggers a null-mutex assert deep in lwIP because the TCPIP core lock is
  // only initialized on the first WiFi.begin().
  if (WiFi.status() != WL_CONNECTED) {
    state = WIFI_SELECTION;
    requestUpdate();
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& r) { onWifiSelectionComplete(!r.isCancelled); });
    return;
  }

  state = RUNNING;
  requestUpdate();
  performDownloads();
}

void DownloadAllActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    finish();
    return;
  }
  state = RUNNING;
  requestUpdate();
  performDownloads();
}

void DownloadAllActivity::onExit() { Activity::onExit(); }

void DownloadAllActivity::performDownloads() {
  for (size_t i = 0; i < articles.size(); i++) {
    if (cancelRequested) {
      RenderLock lock(*this);
      state = CANCELLED;
      requestUpdate(true);
      return;
    }

    const ReadItLaterArticle& a = articles[i];

    // Skip if already cached.
    const std::string epubPath = provider->epubPathFor(a.id, a.title);
    if (Storage.exists(epubPath.c_str())) {
      skipped++;
      {
        RenderLock lock(*this);
        completed = static_cast<int>(i + 1);
      }
      requestUpdate();
      continue;
    }

    char htmlTmpBuf[128];
    std::snprintf(htmlTmpBuf, sizeof(htmlTmpBuf), "/.crosspoint/%s/tmp_html_%llu",
                  provider->cacheDirName(), static_cast<unsigned long long>(a.id));
    const std::string htmlTmpPath(htmlTmpBuf);

    // Ensure the cache directory exists (build() also does this, but fetchText
    // writes a temp file there first and needs it to already exist).
    {
      char cacheDir[64];
      std::snprintf(cacheDir, sizeof(cacheDir), "/.crosspoint/%s", provider->cacheDirName());
      Storage.mkdir(cacheDir);
    }

    const auto r = provider->fetchText(a.id, htmlTmpPath);
    if (r != Provider::Result::OK) {
      LOG_ERR("RIL", "Download-all: %s failed (%s)", a.title, provider->errorString(r));
      Storage.remove(htmlTmpPath.c_str());
      failed++;
    } else {
      const std::string out =
          EpubBuilder::build(provider->cacheDirName(), provider->coverPngData(), provider->coverPngLen(), a.id,
                             a.title, a.author, htmlTmpPath.c_str(), nullptr, nullptr,
                             SETTINGS.readItLaterImages, epubPath.c_str());
      Storage.remove(htmlTmpPath.c_str());
      if (out.empty()) {
        failed++;
      }
    }

    {
      RenderLock lock(*this);
      completed = static_cast<int>(i + 1);
    }
    requestUpdate();
  }

  RenderLock lock(*this);
  state = DONE;
  requestUpdate(true);
}

void DownloadAllActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (state == RUNNING) {
      cancelRequested = true;
    } else {
      finish();
    }
  }
}

void DownloadAllActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_INSTAPAPER_DOWNLOAD_ALL));

  const int total = static_cast<int>(articles.size());

  char status[64];
  if (state == WIFI_SELECTION) {
    std::snprintf(status, sizeof(status), "Waiting for WiFi...");
  } else if (state == RUNNING) {
    std::snprintf(status, sizeof(status), "%d / %d", completed, total);
  } else if (state == DONE) {
    std::snprintf(status, sizeof(status), "Done: %d OK, %d skipped, %d failed", completed - failed - skipped, skipped,
                  failed);
  } else {
    std::snprintf(status, sizeof(status), "Cancelled (%d / %d)", completed, total);
  }

  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, status, true, EpdFontFamily::BOLD);

  const int barWidth = pageWidth - 80;
  GUI.drawProgressBar(renderer, Rect{40, pageHeight / 2, barWidth, 12}, completed, total > 0 ? total : 1);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
