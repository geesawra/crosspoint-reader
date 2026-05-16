#include "ReadItLaterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Provider.h>

#include "MappedInputManager.h"
#include "ProviderFolderActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Include known providers for discovery
#include "Providers/Instapaper/InstapaperProvider.h"

void ReadItLaterActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  providers.clear();
  state = SCANNING;

  discoverProviders();

  if (providers.empty()) {
    state = NO_PROVIDERS;
  } else {
    state = SHOWING_LIST;
  }

  requestUpdate();
}

void ReadItLaterActivity::onExit() { Activity::onExit(); }

void ReadItLaterActivity::discoverProviders() {
  auto instapaper = std::make_unique<InstapaperProvider>();
  if (instapaper->loadCredentials()) {
    providers.push_back(std::move(instapaper));
  }
  // Future providers are added here
}

void ReadItLaterActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state != SHOWING_LIST) {
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(providers.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(providers.size()));
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    Provider* provider = providers[selectedIndex].get();
    activityManager.pushActivity(std::make_unique<ProviderFolderActivity>(renderer, mappedInput, provider));
  }
}

void ReadItLaterActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READ_IT_LATER));

  if (state == NO_PROVIDERS) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 40, tr(STR_RIL_NO_TOKENS), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 5, tr(STR_RIL_TOKENS_HINT));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(providers.size()), selectedIndex,
      [this](int index) { return std::string(providers[index]->name()); }, nullptr,
      [](int index) { return UIIcon::ReadItLater; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
