#include "InstapaperArticleListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <InstapaperEpubBuilder.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include "InstapaperActionsActivity.h"
#include "InstapaperDownloadAllActivity.h"
#include "InstapaperFetchActivity.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int LIST_LIMIT = 50;

// OAuth 1.0a requires a reasonably accurate timestamp. The ESP32-C3 has no
// battery-backed RTC, so after a cold boot the clock sits at epoch 0 until
// NTP syncs it. Block briefly the first time we come online.
void syncTimeOnce() {
  if (::time(nullptr) > 1600000000) {
    return;  // already synced (post-2020)
  }
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
  for (int i = 0; i < 50; i++) {  // ~5 s max
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) break;
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  if (::time(nullptr) < 1600000000) {
    LOG_ERR("INSTA", "NTP sync timed out; OAuth signatures may be rejected");
  } else {
    LOG_DBG("INSTA", "NTP synced, epoch=%lld", static_cast<long long>(::time(nullptr)));
  }
}

const char* folderLabelKey(InstapaperFolder f) {
  switch (f) {
    case InstapaperFolder::UNREAD:
      return tr(STR_INSTAPAPER_UNREAD);
    case InstapaperFolder::STARRED:
      return tr(STR_INSTAPAPER_STARRED);
    case InstapaperFolder::ARCHIVE:
      return tr(STR_INSTAPAPER_ARCHIVE);
  }
  return "";
}
}  // namespace

void InstapaperArticleListActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    state = FETCHING;
    requestUpdate();
    performFetch();
    return;
  }

  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& r) { onWifiSelectionComplete(!r.isCancelled); });
}

void InstapaperArticleListActivity::onExit() { Activity::onExit(); }

void InstapaperArticleListActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    finish();
    return;
  }
  state = FETCHING;
  requestUpdate();
  performFetch();
}

void InstapaperArticleListActivity::performFetch() {
  {
    RenderLock lock(*this);
    state = FETCHING;
  }
  requestUpdateAndWait();

  syncTimeOnce();

  const auto r = InstapaperClient::listBookmarks(folder, LIST_LIMIT, articles);
  {
    RenderLock lock(*this);
    if (r != InstapaperClient::OK) {
      state = FETCH_FAILED;
      errorMessage = InstapaperClient::errorString(r);
    } else if (articles.empty()) {
      state = EMPTY_LIST;
    } else {
      state = SHOWING_LIST;
      selectedIndex = 0;
    }
  }
  requestUpdate(true);
}

void InstapaperArticleListActivity::openSelected() {
  if (state != SHOWING_LIST || articles.empty()) return;
  const InstapaperArticle& a = articles[selectedIndex];
  activityManager.pushActivity(std::make_unique<InstapaperFetchActivity>(renderer, mappedInput, a));
}

void InstapaperArticleListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (state != SHOWING_LIST) return;

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(articles.size()));
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(articles.size()));
    requestUpdate();
  });
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    openSelected();
    return;
  }

  // Left: per-article actions menu (star/archive/delete).
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (articles.empty()) return;
    const InstapaperArticle a = articles[selectedIndex];
    startActivityForResult(std::make_unique<InstapaperActionsActivity>(renderer, mappedInput, a, folder),
                           [this](const ActivityResult& r) {
                             (void)r;
                             // Always refetch on return — the list may have changed (star, archive, delete).
                             performFetch();
                           });
    return;
  }

  // Right: bulk download of the current folder.
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (articles.empty()) return;
    startActivityForResult(std::make_unique<InstapaperDownloadAllActivity>(renderer, mappedInput, articles),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
}

void InstapaperArticleListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderLabelKey(folder));

  if (state == FETCHING || state == WIFI_SELECTION) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_FETCHING), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FETCH_FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_INSTAPAPER_AUTH_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == EMPTY_LIST) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_NO_RECENT_BOOKS), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(articles.size()), selectedIndex,
      [this](int index) { return std::string(articles[index].title); },
      [this](int index) { return std::string(articles[index].domain); }, nullptr);

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_INSTAPAPER_ACTIONS), tr(STR_INSTAPAPER_DOWNLOAD_ALL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
