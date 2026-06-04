#include "ArticleListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Provider.h>
#include <StateCache.h>
#include <WiFi.h>
#include <time.h>

#include <cstdio>

#include "ActionsActivity.h"
#include "ArticleListMenuActivity.h"
#include "DownloadAllActivity.h"
#include "FetchActivity.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int LIST_LIMIT = 200;
}  // namespace

void ArticleListActivity::onEnter() {
  Activity::onEnter();

  pendingRefresh = false;
  errorMessage.clear();

  // Populate from SD cache first so the list appears instantly, even if the
  // follow-up live refresh is slow or fails entirely (offline re-entry).
  const bool hadCache = StateCache::load(provider->cacheDirName(), folder.id, articles, &lastSyncedAt);
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

void ArticleListActivity::onExit() { Activity::onExit(); }

void ArticleListActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    finish();
    return;
  }
  state = FETCHING;
  requestUpdate();
  performFetch();
}

void ArticleListActivity::performFetch(bool showProgress) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_DBG("RIL", "performFetch skipped: WiFi not connected");
    if (showProgress) {
      // User explicitly requested a refresh — offer WiFi selection.
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                             [this](const ActivityResult& r) { onWifiSelectionComplete(!r.isCancelled); });
      return;
    }
    RenderLock lock(*this);
    if (state == SHOWING_LIST && !articles.empty()) {
      offline = true;
    } else {
      state = FETCH_FAILED;
      errorMessage = "No WiFi";
      errorDetail.clear();
    }
    requestUpdate();
    return;
  }

  // If we already have cached data on screen, keep showing it while the
  // refresh runs rather than blanking to a "fetching" spinner — unless an
  // explicit refresh was requested by the user.
  const bool hadCachedList = (state == SHOWING_LIST && !articles.empty());
  if (!hadCachedList || showProgress) {
    RenderLock lock(*this);
    state = FETCHING;
  }
  requestUpdateAndWait();

  std::vector<ReadItLaterArticle> fresh;
  const auto r = provider->listArticles(folder, LIST_LIMIT, fresh);

  if (r.isOk()) {
    StateCache::save(provider->cacheDirName(), folder.id, fresh);
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
      errorMessage = Provider::errorString(r.code);
      errorDetail = r.message;
      state = SHOWING_LIST;
    } else {
      state = FETCH_FAILED;
      errorMessage = Provider::errorString(r.code);
      errorDetail = r.message;
    }
  }
  requestUpdate(true);
}

void ArticleListActivity::openSelected() {
  if (state != SHOWING_LIST || articles.empty()) return;
  const ReadItLaterArticle& a = articles[selectedIndex];
  activityManager.pushActivity(std::make_unique<FetchActivity>(renderer, mappedInput, provider, a));
}

void ArticleListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (pendingRefresh) {
    pendingRefresh = false;
    performFetch(true);
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
    const ReadItLaterArticle a = articles[selectedIndex];
    startActivityForResult(std::make_unique<ActionsActivity>(renderer, mappedInput, provider, a, folder),
                           [this, savedIndex](const ActivityResult& r) {
                             const bool changed = !r.isCancelled;
                             if (changed) {
                               pendingRefresh = true;
                             } else {
                               selectedIndex = savedIndex;
                               requestUpdate();
                             }
                           });
    return;
  }

  // Right: article-list-level actions menu (download all / refresh).
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    startActivityForResult(
        std::make_unique<ArticleListMenuActivity>(renderer, mappedInput), [this](const ActivityResult& r) {
          if (r.isCancelled) {
            requestUpdate();
            return;
          }
          const auto action = static_cast<ArticleListMenuActivity::Action>(std::get<PercentResult>(r.data).percent);
          switch (action) {
            case ArticleListMenuActivity::Action::DOWNLOAD_ALL:
              if (articles.empty()) {
                requestUpdate();
                break;
              }
              startActivityForResult(std::make_unique<DownloadAllActivity>(renderer, mappedInput, provider, articles),
                                     [this](const ActivityResult&) { requestUpdate(); });
              break;
            case ArticleListMenuActivity::Action::REFRESH:
              pendingRefresh = true;
              requestUpdate();
              break;
          }
        });
    return;
  }
}

void ArticleListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  char subtitleBuf[48];
  const char* subtitle = nullptr;
  if (offline && lastSyncedAt > 0) {
    const time_t age = ::time(nullptr) - lastSyncedAt;
    if (age < 120) {
      std::snprintf(subtitleBuf, sizeof(subtitleBuf), "Offline");
    } else if (age < 3600) {
      std::snprintf(subtitleBuf, sizeof(subtitleBuf), "Offline · %dm", static_cast<int>(age / 60));
    } else if (age < 86400) {
      std::snprintf(subtitleBuf, sizeof(subtitleBuf), "Offline · %dh", static_cast<int>(age / 3600));
    } else {
      std::snprintf(subtitleBuf, sizeof(subtitleBuf), "Offline · %dd", static_cast<int>(age / 86400));
    }
    subtitle = subtitleBuf;
  }

  const char* headerLabel =
      (folder.label && folder.label[0] != '\0') ? folder.label : (provider ? provider->name() : "");
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerLabel, subtitle);

  if (state == FETCHING || state == WIFI_SELECTION) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_REFRESHING), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FETCH_FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, errorMessage.c_str(), true,
                              EpdFontFamily::BOLD);
    if (!errorDetail.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 5, errorDetail.c_str());
    }
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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_INSTAPAPER_ACTIONS), tr(STR_OPTIONS));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
