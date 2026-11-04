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

/** A corner of the screen that shows the connection's ping, input delay and rollbacks during netplay races. */
class NetplayStatsOverlay : public Ship::GuiWindow {
  public:
    NetplayStatsOverlay() : Ship::GuiWindow("", true, "Netplay Stats") {
    }
    /** Draws a small, click-through box, only while a race runs and the overlay is turned on. */
    void Draw() override;

  protected:
    void InitElement() override {};
    void DrawElement() override;
    void UpdateElement() override {};
};
} // namespace GameUI
