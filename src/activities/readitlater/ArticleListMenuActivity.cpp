#include "ArticleListMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

ArticleListMenuActivity::ArticleListMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ArticleListMenu", renderer, mappedInput), menuItems(buildMenuItems()) {}

std::vector<ArticleListMenuActivity::MenuItem> ArticleListMenuActivity::buildMenuItems() {
  return {{Action::DOWNLOAD_ALL, StrId::STR_INSTAPAPER_DOWNLOAD_ALL},
          {Action::REFRESH, StrId::STR_INSTAPAPER_REFRESH}};
}

const char* ArticleListMenuActivity::labelFor(Action a) {
  switch (a) {
    case Action::DOWNLOAD_ALL:
      return tr(STR_INSTAPAPER_DOWNLOAD_ALL);
    case Action::REFRESH:
      return tr(STR_INSTAPAPER_REFRESH);
  }
  return "";
}

void ArticleListMenuActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void ArticleListMenuActivity::onExit() { Activity::onExit(); }

void ArticleListMenuActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    setResult(std::move(cancelled));
    finish();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    setResult(PercentResult{selectedIndex});
    finish();
    return;
  }
}

void ArticleListMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_INSTAPAPER_ACTIONS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(menuItems.size()), selectedIndex,
      [this](int index) { return std::string(labelFor(menuItems[index].action)); }, nullptr, nullptr);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
