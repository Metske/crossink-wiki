#pragma once

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

class UiListActivity : public Activity, protected UiAppHost<24, 4> {
 public:
  using UiScreen = typename UiAppHost<24, 4>::Screen;

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 protected:
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_USER = 2;

  UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                 bool wantsTouchLongPress = false);

  virtual int listCount() const = 0;
  virtual void buildScreen(UiScreen& screen) = 0;
  virtual void activateIndex(int index) = 0;
  virtual void onRowLongPress(int index) {}
  virtual freeink::ui::ListNav& activeNav() { return nav; }
  virtual void onRowAction(const freeink::ui::ActionEvent& event);
  virtual void navigateButtons();
  virtual bool handleCustomInput() { return false; }
  virtual bool handleButtons();
  virtual void onBackButton() { finish(); }
  virtual const char* headerTitle() const { return nullptr; }
  virtual void drawChrome();
  virtual void drawFooter();

  void syncListViewport(UiScreen& screen, freeink::ui::ListProps& props, bool hasSubtitle = false);
  void moveSelectionTo(int index);

  freeink::ui::ListNav nav;
  bool chromeNeedsListLayout = false;
  ButtonNavigator buttonNavigator;

 private:
  static void screenTrampoline(UiScreen& screen, void* user);
  static void rowActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  bool routeListTouch();

  const bool wantsTouchLongPress;
};
