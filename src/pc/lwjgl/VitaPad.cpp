#include "lwjgl/VitaPad.h"

#ifdef VITA_PLATFORM

#include <cmath>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

#include "SDL.h"

#include "lwjgl/Display.h"
#include "lwjgl/GLContext.h"
#include "lwjgl/Keyboard.h"
#include "lwjgl/Mouse.h"
#include "lwjgl/VitaIme.h"

#include "client/Minecraft.h"
#include "net/minecraft/src/EntityPlayerSP.h"
#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/InventoryPlayer.h"
#include "net/minecraft/src/ScaledResolution.h"
#include "net/minecraft/src/legacy/LegacyHudLayout.h"

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
	InputCount
};

constexpr float kMoveDeadZone = 0.2f;
constexpr int kChatCornerWidth = 140;
constexpr int kChatCornerHeight = 90;
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
bool g_sneaking = false;
bool g_touching = false;
int g_stickX = 128;
int g_stickY = 128;

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
			case InputLeft: return wheel(1);
			case InputRight: return wheel(-1);
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
		case InputLTrigger: return key(SDLK_PAGEUP);
		case InputRTrigger: return key(SDLK_PAGEDOWN);
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

void tapKey(int code)
{
	sendKey(code, true);
	sendKey(code, false);
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
	{
		g_cursorKnown = false;
		return;
	}

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

void selectHotbarAt(int px, int py)
{
	Minecraft *mc = Minecraft::getMinecraft();
	if (mc == nullptr || mc->gameSettings == nullptr || mc->thePlayer == nullptr || mc->thePlayer->inventory == nullptr)
		return;

	ScaledResolution resolution(mc->gameSettings, mc->displayWidth, mc->displayHeight);
	const int width = resolution.getScaledWidth();
	const int height = resolution.getScaledHeight();
	const int x = px * width / mc->displayWidth;
	const int y = py * height / mc->displayHeight;
	const int bottom = height - (mc->gameSettings->legacyUI ? legacyHudBottomInset(mc->isSplitScreenActive()) : 0);
	const int left = width / 2 - 91;
	if (x < left || x >= left + 182 || y < bottom - 22 || y >= bottom)
		return;

	int slot = (x - left - 1) / 20;
	if (slot < 0)
		slot = 0;
	if (slot > 8)
		slot = 8;
	mc->thePlayer->inventory->currentItem = slot;
}

void pollTouch()
{
	SceTouchData touch = {};
	if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 0)
		return;

	const bool touching = touch.reportNum > 0;
	if (touching && !g_touching && g_grabbed)
	{
		const int x = touch.report[0].x * Display::getWidth() / 1920;
		const int y = touch.report[0].y * Display::getHeight() / 1088;
		if (x >= Display::getWidth() - kChatCornerWidth && y < kChatCornerHeight)
			tapKey(SDLK_t);
		else
			selectHotbarAt(x, y);
	}
	g_touching = touching;
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
	g_stickX = pad.lx;
	g_stickY = pad.ly;

	if (VitaIme::active())
	{
		VitaIme::update();
		for (int i = 0; i < InputCount; ++i)
		{
			if (g_active[i].type != ActionType::None)
			{
				end(g_active[i]);
				g_active[i] = Action();
			}
			g_previous[i] = pressed[i];
		}
		g_stickX = 128;
		g_stickY = 128;
		return;
	}

	pollTouch();

	for (int i = 0; i < InputCount; ++i)
	{
		if (i == InputDown && g_grabbed && g_active[i].type == ActionType::None)
		{
			if (pressed[i] && !g_previous[i])
			{
				g_sneaking = !g_sneaking;
				sendKey(SDLK_LSHIFT, g_sneaking);
			}
			g_previous[i] = pressed[i];
			continue;
		}

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
	if (sdlKey == SDLK_LSHIFT && g_sneaking && g_grabbed)
		return true;
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
	if (grabbed && !g_grabbed && g_sneaking)
		sendKey(SDLK_LSHIFT, true);
	g_grabbed = grabbed;
	g_lookRemainderX = 0.0f;
	g_lookRemainderY = 0.0f;
}

bool grabbed()
{
	return g_grabbed;
}

bool touchMouseEvent(const SDL_Event &e)
{
	if (!g_grabbed)
		return false;
	switch (e.type)
	{
		case SDL_MOUSEMOTION: return e.motion.which == SDL_TOUCH_MOUSEID;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: return e.button.which == SDL_TOUCH_MOUSEID;
		case SDL_MOUSEWHEEL: return e.wheel.which == SDL_TOUCH_MOUSEID;
		default: return false;
	}
}

bool sneaking()
{
	return g_sneaking;
}

void leftStick(float &x, float &y)
{
	x = 0.0f;
	y = 0.0f;
	if (!g_grabbed)
		return;

	const float sx = static_cast<float>(g_stickX - 128) / 127.0f;
	const float sy = static_cast<float>(g_stickY - 128) / 127.0f;
	const float length = std::sqrt(sx * sx + sy * sy);
	if (length <= kMoveDeadZone)
		return;

	float scale = (length - kMoveDeadZone) / (1.0f - kMoveDeadZone);
	if (scale > 1.0f)
		scale = 1.0f;
	x = sx / length * scale;
	y = sy / length * scale;
}

}
}

#endif
