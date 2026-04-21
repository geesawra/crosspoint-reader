#include "ActionsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ActionsActivity::onEnter() {
  Activity::onEnter();
  buildActions();
  selectedIndex = 0;
  state = MENU;
  requestUpdate();
}

void ActionsActivity::onExit() { Activity::onExit(); }

void ActionsActivity::buildActions() {
  actions.clear();
  if (provider) {
    actions = provider->availableActions(folder, article);
  }
}

const char* ActionsActivity::labelFor(Provider::Action a) const {
  switch (a) {
    case Provider::Action::Star:
      return tr(STR_INSTAPAPER_STAR);
    case Provider::Action::Unstar:
      return tr(STR_INSTAPAPER_UNSTAR);
    case Provider::Action::Archive:
      return tr(STR_INSTAPAPER_ARCHIVE_ACTION);
    case Provider::Action::Unarchive:
      return tr(STR_INSTAPAPER_UNARCHIVE);
    case Provider::Action::Delete:
      return tr(STR_INSTAPAPER_DELETE);
  }
  return "";
}

void ActionsActivity::runSelectedAction() {
  if (!provider) {
    finish();
    return;
  }

  // Same guard as the other network activities — starting a signed POST
  // without lwIP up faults inside ESP-IDF.
  if (WiFi.status() != WL_CONNECTED) {
    {
      RenderLock lock(*this);
      state = WIFI_SELECTION;
    }
    requestUpdate();
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& r) { onWifiSelectionComplete(!r.isCancelled); });
    return;
  }

  {
    RenderLock lock(*this);
    state = RUNNING;
  }
  requestUpdateAndWait();

  const Provider::Action action = actions[selectedIndex];
  const auto r = provider->performAction(article.id, action);

  // Best-effort local cache cleanup on destructive actions.
  if (r == Provider::Result::OK && action == Provider::Action::Delete) {
    const std::string epubPath = provider->epubPathFor(article.id);
    if (Storage.exists(epubPath.c_str())) {
      Storage.remove(epubPath.c_str());
    }
  }

  {
    RenderLock lock(*this);
    if (r == Provider::Result::OK) {
      state = DONE;
      message = labelFor(action);
    } else {
      state = FAILED;
      message = provider->errorString(r);
    }
  }
  requestUpdate(true);
}

void ActionsActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    RenderLock lock(*this);
    state = MENU;
    requestUpdate();
    return;
  }
  runSelectedAction();
}

void ActionsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // ActivityResult defaults to isCancelled=false — we have to mark cancel
    // explicitly, or the parent's callback treats Back as success and runs
    // performFetch(), which crashes lwIP when we're offline.
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    setResult(std::move(cancelled));
    finish();
    return;
  }

  if (state == DONE || state == FAILED) {
    // Any press dismisses the result screen. Treat DONE as "changed" so the
    // caller refreshes; FAILED as "no change".
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (state == DONE) {
        setResult(PercentResult{1});  // reusing a simple result — non-zero = changed
      } else {
        ActivityResult cancelled;
        cancelled.isCancelled = true;
        setResult(std::move(cancelled));
      }
      finish();
    }
    return;
  }

  if (state != MENU) return;

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(actions.size()));
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(actions.size()));
    requestUpdate();
  });
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    runSelectedAction();
  }
}

void ActionsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, article.title);

  if (state == RUNNING || state == WIFI_SELECTION) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, labelFor(actions[selectedIndex]), true,
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == DONE) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 10, message.c_str(), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, tr(STR_INSTAPAPER_AUTH_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INSTAPAPER_AUTH_HINT));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25, message.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(actions.size()), selectedIndex,
      [this](int index) { return std::string(labelFor(actions[index])); }, nullptr, nullptr);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
