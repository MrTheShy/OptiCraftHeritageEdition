#pragma once

#ifdef VITA_PLATFORM

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

}
}

#endif
