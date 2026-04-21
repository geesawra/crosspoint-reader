#include "InstapaperFolderActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <InstapaperArticle.h>
#include <InstapaperTokenStore.h>
#include <Logging.h>

#include "InstapaperArticleListActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEM_COUNT = 3;
}  // namespace

void InstapaperFolderActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;

  if (!INSTAPAPER_TOKENS.loadFromFile()) {
    state = NO_TOKENS;
  } else {
    state = READY;
  }

  requestUpdate();
}

void InstapaperFolderActivity::onExit() { Activity::onExit(); }

void InstapaperFolderActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state != READY) {
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const InstapaperFolder folders[MENU_ITEM_COUNT] = {InstapaperFolder::UNREAD, InstapaperFolder::STARRED,
                                                       InstapaperFolder::ARCHIVE};
    activityManager.pushActivity(
        std::make_unique<InstapaperArticleListActivity>(renderer, mappedInput, folders[selectedIndex]));
  }
}

void InstapaperFolderActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_INSTAPAPER));

  if (state == NO_TOKENS) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 40, tr(STR_INSTAPAPER_NO_TOKENS), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 5, tr(STR_INSTAPAPER_TOKENS_HINT));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, tr(STR_INSTAPAPER_TOKENS_PATH));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  static constexpr StrId folderLabels[MENU_ITEM_COUNT] = {StrId::STR_INSTAPAPER_UNREAD, StrId::STR_INSTAPAPER_STARRED,
                                                          StrId::STR_INSTAPAPER_ARCHIVE};
  static constexpr UIIcon folderIcons[MENU_ITEM_COUNT] = {UIIcon::Instapaper, UIIcon::Instapaper, UIIcon::Instapaper};

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEM_COUNT, selectedIndex,
      [](int index) { return std::string(I18N.get(folderLabels[index])); }, nullptr,
      [](int index) { return folderIcons[index]; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
