#include "lwjgl/VitaPad.h"

#ifdef VITA_PLATFORM

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "SDL.h"

#include "lwjgl/Display.h"
#include "lwjgl/GLContext.h"
#include "lwjgl/Keyboard.h"
#include "lwjgl/Mouse.h"

#include "client/Minecraft.h"
#include "net/minecraft/src/GameSettings.h"

namespace lwjgl
{
namespace VitaPad
{

namespace
{

enum class ActionType
{
	None,
	Key,
	MouseButton,
	Wheel
};

struct Action
{
	ActionType type = ActionType::None;
	int code = 0;
};

enum Input
{
	InputCross,
	InputCircle,
	InputSquare,
	InputTriangle,
	InputLTrigger,
	InputRTrigger,
	InputStart,
	InputSelect,
	InputUp,
	InputDown,
	InputLeft,
	InputRight,
	InputStickUp,
	InputStickDown,
	InputStickLeft,
	InputStickRight,
	InputCount
};

constexpr int kStickThreshold = 64;
constexpr int kLookDeadZone = 24;
constexpr float kLookSpeedPerSecond = 1000.0f;
constexpr float kMaxFrameSeconds = 0.1f;
constexpr float kCursorSpeedPerSecond = 700.0f;

bool g_initialized = false;
bool g_grabbed = false;
bool g_previous[InputCount] = {};
Action g_active[InputCount] = {};
float g_lookRemainderX = 0.0f;
float g_lookRemainderY = 0.0f;
SceUInt64 g_lastPollTime = 0;
bool g_pointerMode = false;
bool g_cursorKnown = false;
float g_cursorX = 0.0f;
float g_cursorY = 0.0f;

Action key(int code)
{
	Action action;
	action.type = ActionType::Key;
	action.code = code;
	return action;
}

Action mouseButton(int code)
{
	Action action;
	action.type = ActionType::MouseButton;
	action.code = code;
	return action;
}

Action wheel(int delta)
{
	Action action;
	action.type = ActionType::Wheel;
	action.code = delta;
	return action;
}

bool legacyCrafting()
{
	Minecraft *mc = Minecraft::getMinecraft();
	return mc != nullptr && mc->gameSettings != nullptr && mc->gameSettings->legacyCrafting;
}

Action actionFor(int input, bool inGame, bool pointerMode)
{
	if (inGame)
	{
		switch (input)
		{
			case InputCross: return key(SDLK_SPACE);
			case InputCircle: return key(SDLK_q);
			case InputSquare: return key(legacyCrafting() ? SDLK_c : SDLK_e);
			case InputTriangle: return key(SDLK_e);
			case InputLTrigger: return mouseButton(SDL_BUTTON_RIGHT);
			case InputRTrigger: return mouseButton(SDL_BUTTON_LEFT);
			case InputStart: return key(SDLK_ESCAPE);
			case InputSelect: return key(SDLK_F3);
			case InputUp: return key(SDLK_F5);
			case InputDown: return key(SDLK_LSHIFT);
			case InputLeft: return wheel(1);
			case InputRight: return wheel(-1);
			case InputStickUp: return key(SDLK_w);
			case InputStickDown: return key(SDLK_s);
			case InputStickLeft: return key(SDLK_a);
			case InputStickRight: return key(SDLK_d);
			default: return Action();
		}
	}

	switch (input)
	{
		case InputCross: return pointerMode ? mouseButton(SDL_BUTTON_LEFT) : key(SDLK_RETURN);
		case InputSquare: return pointerMode ? mouseButton(SDL_BUTTON_RIGHT) : Action();
		case InputCircle: return key(SDLK_ESCAPE);
		case InputTriangle: return key(SDLK_e);
		case InputStart: return key(SDLK_ESCAPE);
		case InputUp: return key(SDLK_UP);
		case InputDown: return key(SDLK_DOWN);
		case InputLeft: return key(SDLK_LEFT);
		case InputRight: return key(SDLK_RIGHT);
		default: return Action();
	}
}

void sendKey(int code, bool down)
{
	SDL_Event e = {};
	e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	e.key.repeat = 0;
	e.key.keysym.sym = static_cast<SDL_Keycode>(code);
	e.key.keysym.scancode = SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(code));
	Keyboard::detail::pushEvent(e);
}

void sendMouseButton(int code, bool down)
{
	SDL_Event e = {};
	e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	e.button.button = static_cast<Uint8>(code);
	e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	if (g_grabbed)
	{
		e.button.x = Display::getWidth() / 2;
		e.button.y = Display::getHeight() / 2;
	}
	else
	{
		SDL_GetMouseState(&e.button.x, &e.button.y);
	}
	Mouse::detail::pushEvent(e);
}

void sendWheel(int delta)
{
	SDL_Event e = {};
	e.type = SDL_MOUSEWHEEL;
	e.wheel.y = delta;
	Mouse::detail::pushEvent(e);
}

void sendLook(int dx, int dy)
{
	SDL_Event e = {};
	e.type = SDL_MOUSEMOTION;
	e.motion.x = Display::getWidth() / 2;
	e.motion.y = Display::getHeight() / 2;
	e.motion.xrel = dx;
	e.motion.yrel = dy;
	Mouse::detail::pushEvent(e);
}

void begin(const Action &action)
{
	switch (action.type)
	{
		case ActionType::Key: sendKey(action.code, true); break;
		case ActionType::MouseButton: sendMouseButton(action.code, true); break;
		case ActionType::Wheel: sendWheel(action.code); break;
		default: break;
	}
}

void end(const Action &action)
{
	switch (action.type)
	{
		case ActionType::Key: sendKey(action.code, false); break;
		case ActionType::MouseButton: sendMouseButton(action.code, false); break;
		default: break;
	}
}

float cursorAxis(int value, float seconds)
{
	const int centered = value - 128;
	const int magnitude = centered < 0 ? -centered : centered;
	if (magnitude < kLookDeadZone)
		return 0.0f;
	float amount = static_cast<float>(magnitude - kLookDeadZone) / static_cast<float>(128 - kLookDeadZone);
	if (amount > 1.0f)
		amount = 1.0f;
	amount = amount * amount * kCursorSpeedPerSecond * seconds;
	return centered < 0 ? -amount : amount;
}

void moveCursor(const SceCtrlData &pad, float seconds)
{
	const float dx = cursorAxis(pad.lx, seconds);
	const float dy = cursorAxis(pad.ly, seconds);
	if (dx == 0.0f && dy == 0.0f)
		return;

	if (!g_cursorKnown)
	{
		int x = 0;
		int y = 0;
		SDL_GetMouseState(&x, &y);
		g_cursorX = static_cast<float>(x);
		g_cursorY = static_cast<float>(y);
		g_cursorKnown = true;
	}

	const float maxX = static_cast<float>(Display::getWidth() - 1);
	const float maxY = static_cast<float>(Display::getHeight() - 1);
	g_cursorX += dx;
	g_cursorY += dy;
	if (g_cursorX < 0.0f)
		g_cursorX = 0.0f;
	if (g_cursorY < 0.0f)
		g_cursorY = 0.0f;
	if (g_cursorX > maxX)
		g_cursorX = maxX;
	if (g_cursorY > maxY)
		g_cursorY = maxY;

	g_pointerMode = true;
	SDL_WarpMouseInWindow(GLContext::detail::getWindow(), static_cast<int>(g_cursorX), static_cast<int>(g_cursorY));
}

float lookAxis(int value, float &remainder, float seconds)
{
	const int centered = value - 128;
	const int magnitude = centered < 0 ? -centered : centered;
	if (magnitude < kLookDeadZone)
	{
		remainder = 0.0f;
		return 0.0f;
	}
	float amount = static_cast<float>(magnitude - kLookDeadZone) / static_cast<float>(128 - kLookDeadZone);
	if (amount > 1.0f)
		amount = 1.0f;
	amount = amount * amount * kLookSpeedPerSecond * seconds;
	return centered < 0 ? -amount : amount;
}

int takeWhole(float value, float &remainder)
{
	remainder += value;
	const int whole = static_cast<int>(remainder);
	remainder -= static_cast<float>(whole);
	return whole;
}

}

void poll()
{
	if (!g_initialized)
	{
		sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
		g_initialized = true;
	}

	const SceUInt64 now = sceKernelGetProcessTimeWide();
	float seconds = g_lastPollTime == 0 ? 0.0f : static_cast<float>(now - g_lastPollTime) / 1000000.0f;
	g_lastPollTime = now;
	if (seconds > kMaxFrameSeconds)
		seconds = kMaxFrameSeconds;

	SceCtrlData pad = {};
	if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
		return;

	bool pressed[InputCount] = {};
	pressed[InputCross] = (pad.buttons & SCE_CTRL_CROSS) != 0;
	pressed[InputCircle] = (pad.buttons & SCE_CTRL_CIRCLE) != 0;
	pressed[InputSquare] = (pad.buttons & SCE_CTRL_SQUARE) != 0;
	pressed[InputTriangle] = (pad.buttons & SCE_CTRL_TRIANGLE) != 0;
	pressed[InputLTrigger] = (pad.buttons & SCE_CTRL_LTRIGGER) != 0;
	pressed[InputRTrigger] = (pad.buttons & SCE_CTRL_RTRIGGER) != 0;
	pressed[InputStart] = (pad.buttons & SCE_CTRL_START) != 0;
	pressed[InputSelect] = (pad.buttons & SCE_CTRL_SELECT) != 0;
	pressed[InputUp] = (pad.buttons & SCE_CTRL_UP) != 0;
	pressed[InputDown] = (pad.buttons & SCE_CTRL_DOWN) != 0;
	pressed[InputLeft] = (pad.buttons & SCE_CTRL_LEFT) != 0;
	pressed[InputRight] = (pad.buttons & SCE_CTRL_RIGHT) != 0;
	pressed[InputStickUp] = pad.ly < 128 - kStickThreshold;
	pressed[InputStickDown] = pad.ly > 128 + kStickThreshold;
	pressed[InputStickLeft] = pad.lx < 128 - kStickThreshold;
	pressed[InputStickRight] = pad.lx > 128 + kStickThreshold;

	for (int i = 0; i < InputCount; ++i)
	{
		if (pressed[i] && !g_previous[i])
		{
			if (!g_grabbed && i >= InputUp && i <= InputRight)
				g_pointerMode = false;
			g_active[i] = actionFor(i, g_grabbed, g_pointerMode);
			begin(g_active[i]);
		}
		else if (!pressed[i] && g_previous[i])
		{
			end(g_active[i]);
			g_active[i] = Action();
		}
		g_previous[i] = pressed[i];
	}

	if (!g_grabbed)
		moveCursor(pad, seconds);

	if (g_grabbed)
	{
		const int dx = takeWhole(lookAxis(pad.rx, g_lookRemainderX, seconds), g_lookRemainderX);
		const int dy = takeWhole(lookAxis(pad.ry, g_lookRemainderY, seconds), g_lookRemainderY);
		if (dx != 0 || dy != 0)
			sendLook(dx, dy);
	}
}

bool keyDown(int sdlKey)
{
	for (int i = 0; i < InputCount; ++i)
	{
		if (g_active[i].type == ActionType::Key && g_active[i].code == sdlKey)
			return true;
	}
	return false;
}

bool mouseButtonDown(int sdlButton)
{
	for (int i = 0; i < InputCount; ++i)
	{
		if (g_active[i].type == ActionType::MouseButton && g_active[i].code == sdlButton)
			return true;
	}
	return false;
}

void setGrabbed(bool grabbed)
{
	g_grabbed = grabbed;
	g_lookRemainderX = 0.0f;
	g_lookRemainderY = 0.0f;
}

bool grabbed()
{
	return g_grabbed;
}

}
}

#endif
