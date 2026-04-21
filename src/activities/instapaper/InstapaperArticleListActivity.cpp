#include "InstapaperArticleListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <InstapaperEpubBuilder.h>
#include <InstapaperStateCache.h>
#include <Logging.h>
#include <WiFi.h>
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

  // Populate from SD cache first so the list appears instantly, even if the
  // follow-up live refresh is slow or fails entirely (offline re-entry).
  const bool hadCache = InstapaperStateCache::load(folder, articles, &lastSyncedAt);
  if (hadCache && !articles.empty()) {
    state = SHOWING_LIST;
    selectedIndex = 0;
    offline = true;
    requestUpdate();
  }

  if (WiFi.status() == WL_CONNECTED) {
    performFetch();
    return;
  }

  if (hadCache) {
    // Stay on cached data; don't force the user onto the WiFi picker.
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
  // If we already have cached data on screen, keep showing it while the
  // refresh runs rather than blanking to a "fetching" spinner.
  const bool hadCachedList = (state == SHOWING_LIST && !articles.empty());
  if (!hadCachedList) {
    RenderLock lock(*this);
    state = FETCHING;
  }
  requestUpdateAndWait();

  std::vector<InstapaperArticle> fresh;
  const auto r = InstapaperClient::listBookmarks(folder, LIST_LIMIT, fresh);

  if (r == InstapaperClient::OK) {
    InstapaperStateCache::save(folder, fresh);
    {
      RenderLock lock(*this);
      articles = std::move(fresh);
      offline = false;
      lastSyncedAt = ::time(nullptr);
      if (articles.empty()) {
        state = EMPTY_LIST;
      } else {
        state = SHOWING_LIST;
        if (selectedIndex >= static_cast<int>(articles.size())) {
          selectedIndex = 0;
        }
      }
    }
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock(*this);
    if (hadCachedList) {
      // Stay on cached data, just mark as offline.
      offline = true;
      errorMessage = InstapaperClient::errorString(r);
    } else {
      state = FETCH_FAILED;
      errorMessage = InstapaperClient::errorString(r);
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

  // Nav uses Up/Down only — NOT ButtonNavigator's Up/Left/Down/Right, because
  // Left and Right are bound to per-article actions and bulk-download here.
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(articles.size()));
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(articles.size()));
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    openSelected();
    return;
  }

  // Left: per-article actions menu (star/archive/delete).
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (articles.empty()) return;
    const int savedIndex = selectedIndex;
    const InstapaperArticle a = articles[selectedIndex];
    startActivityForResult(std::make_unique<InstapaperActionsActivity>(renderer, mappedInput, a, folder),
                           [this, savedIndex](const ActivityResult& r) {
                             const bool changed = !r.isCancelled;
                             if (changed) {
                               // Article may have moved folders — refetch, keep index within bounds.
                               performFetch();
                             } else {
                               // Cancelled: restore the cursor exactly and just redraw.
                               selectedIndex = savedIndex;
                               requestUpdate();
                             }
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

  char subtitleBuf[48];
  const char* subtitle = nullptr;
  if (offline && lastSyncedAt > 0) {
    const time_t age = ::time(nullptr) - lastSyncedAt;
    if (age < 120) {
      std::snprintf(subtitleBuf, sizeof(subtitleBuf), "Offline · updated just now");
    } else if (age < 3600) {
      std::snprintf(subtitleBuf, sizeof(subtitleBuf), "Offline · %dm ago", static_cast<int>(age / 60));
    } else if (age < 86400) {
      std::snprintf(subtitleBuf, sizeof(subtitleBuf), "Offline · %dh ago", static_cast<int>(age / 3600));
    } else {
      std::snprintf(subtitleBuf, sizeof(subtitleBuf), "Offline · %dd ago", static_cast<int>(age / 86400));
    }
    subtitle = subtitleBuf;
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderLabelKey(folder),
                 subtitle);

  if (state == FETCHING || state == WIFI_SELECTION) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_FETCHING), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FETCH_FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, tr(STR_INSTAPAPER_AUTH_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_AUTH_HINT));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25, errorMessage.c_str());
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
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_INSTAPAPER_ACTIONS), tr(STR_INSTAPAPER_GET_ALL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
