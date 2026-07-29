#include "ProviderFolderActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "ArticleListActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ProviderFolderActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  if (provider) {
    provider->listFolders(folders);
  }

  requestUpdate();
}

void ProviderFolderActivity::onExit() { Activity::onExit(); }

void ProviderFolderActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(folders.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(folders.size()));
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (provider && selectedIndex >= 0 && selectedIndex < static_cast<int>(folders.size())) {
      activityManager.pushActivity(
          std::make_unique<ArticleListActivity>(renderer, mappedInput, provider, folders[selectedIndex]));
    }
  }
}

void ProviderFolderActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 provider ? provider->name() : "");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(folders.size()), selectedIndex,
      [this](int index) { return std::string(folders[index].label); }, nullptr,
      [](int index) { return UIIcon::ReadItLater; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
