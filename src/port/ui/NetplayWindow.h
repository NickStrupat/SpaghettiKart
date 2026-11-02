#pragma once

#include <libultraship/libultraship.h>

namespace GameUI {
/** The netplay lobby: host or join a room, pick a character, and (as host) choose and start races. */
class NetplayWindow : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;

  protected:
    void InitElement() override {};
    void DrawElement() override;
    void UpdateElement() override {};
};
} // namespace GameUI
