#pragma once
#include <I18n.h>

#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Modal menu for article-list-level actions (download all, refresh).
 *
 * Launched by ArticleListActivity when the user presses Right.
 * Renders a short selection list and returns the chosen action.
 */
class ArticleListMenuActivity final : public Activity {
 public:
  enum class Action : uint8_t {
    DOWNLOAD_ALL,
    REFRESH,
  };

  explicit ArticleListMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    Action action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems();

  const std::vector<MenuItem> menuItems;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  static const char* labelFor(Action a);
};
