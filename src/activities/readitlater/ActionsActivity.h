#pragma once
#include <ReadItLaterArticle.h>

#include <string>
#include <vector>

#include "Provider.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Modal action menu for a single Read-it-Later article.
 *
 * Launched by ArticleListActivity when the user presses Left on
 * a highlighted article. Menu entries are built dynamically via
 * Provider::availableActions().
 *
 * On Confirm, performs the corresponding API call synchronously and
 * finishes with a result that signals whether the parent list must refresh.
 */
class ActionsActivity final : public Activity {
 public:
  ActionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Provider* provider,
                  ReadItLaterArticle article, Provider::FolderInfo folder)
      : Activity("Actions", renderer, mappedInput), provider(provider), article(article), folder(folder) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == RUNNING; }

 private:
  enum State : uint8_t {
    MENU,
    WIFI_SELECTION,
    RUNNING,
    DONE,
    FAILED,
  };

  Provider* provider = nullptr;
  ReadItLaterArticle article;
  Provider::FolderInfo folder;
  State state = MENU;
  int selectedIndex = 0;
  std::vector<Provider::Action> actions;
  std::string message;
  ButtonNavigator buttonNavigator;

  void buildActions();
  void runSelectedAction();
  void onWifiSelectionComplete(bool success);
  const char* labelFor(Provider::Action a) const;
};
