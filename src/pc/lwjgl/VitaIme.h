#pragma once

#ifdef VITA_PLATFORM

class GuiTextField;

namespace lwjgl
{
namespace VitaIme
{

void open(GuiTextField *field, bool submit = false);
void forget(GuiTextField *field);
void update();
bool active();

}
}

#endif
