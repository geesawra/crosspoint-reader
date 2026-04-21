#pragma once
#include <InstapaperArticle.h>
#include <InstapaperClient.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Modal action menu for a single Instapaper article.
 *
 * Launched by InstapaperArticleListActivity when the user presses Left on
 * a highlighted article. Menu entries are folder-aware:
 *   Unread   → Star, Archive, Delete
 *   Starred  → Unstar, Archive, Delete
 *   Archive  → Unarchive, Delete
 *
 * On Confirm, performs the corresponding API call synchronously and
 * finishes with a result that signals whether the parent list must refresh.
 */
class InstapaperActionsActivity final : public Activity {
 public:
  InstapaperActionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, InstapaperArticle article,
                            InstapaperFolder folder)
      : Activity("InstapaperActions", renderer, mappedInput), article(article), folder(folder) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == RUNNING; }

 private:
  enum Action : uint8_t {
    STAR,
    UNSTAR,
    ARCHIVE,
    UNARCHIVE,
    DELETE,
  };
  enum State : uint8_t {
    MENU,
    WIFI_SELECTION,
    RUNNING,
    DONE,
    FAILED,
  };

  InstapaperArticle article;
  InstapaperFolder folder;
  State state = MENU;
  int selectedIndex = 0;
  std::vector<Action> actions;
  std::string message;
  ButtonNavigator buttonNavigator;

  void buildActions();
  void runSelectedAction();
  void onWifiSelectionComplete(bool success);
  const char* labelFor(Action a) const;
};
