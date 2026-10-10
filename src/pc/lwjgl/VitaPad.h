#pragma once

#ifdef VITA_PLATFORM

#include "SDL_events.h"

namespace lwjgl
{
namespace VitaPad
{

void poll();
bool keyDown(int sdlKey);
bool mouseButtonDown(int sdlButton);
void setGrabbed(bool grabbed);
bool grabbed();
void leftStick(float &x, float &y);
bool sneaking();
bool touchMouseEvent(const SDL_Event &e);

}
}

#endif
