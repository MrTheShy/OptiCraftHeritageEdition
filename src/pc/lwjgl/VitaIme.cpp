#include "lwjgl/VitaIme.h"

#ifdef VITA_PLATFORM

#include <cstring>
#include <string>

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>

#include "SDL.h"

#include "lwjgl/Keyboard.h"

#include "net/minecraft/src/ChatAllowedCharacters.h"
#include "net/minecraft/src/GuiTextField.h"

namespace lwjgl
{
namespace VitaIme
{

namespace
{

GuiTextField *g_field = nullptr;
bool g_running = false;
bool g_submit = false;
SceWChar16 g_title[1] = {};
SceWChar16 g_initial[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1] = {};
SceWChar16 g_input[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1] = {};

void toUtf16(const std::string &in, SceWChar16 *out, int max)
{
	int n = 0;
	std::size_t i = 0;
	while (i < in.size() && n < max)
	{
		const unsigned char c = static_cast<unsigned char>(in[i]);
		unsigned int cp = c;
		int extra = 0;
		if (c >= 0xF0)
		{
			cp = c & 0x07;
			extra = 3;
		}
		else if (c >= 0xE0)
		{
			cp = c & 0x0F;
			extra = 2;
		}
		else if (c >= 0xC0)
		{
			cp = c & 0x1F;
			extra = 1;
		}
		++i;
		for (int k = 0; k < extra && i < in.size(); ++k, ++i)
			cp = (cp << 6) | (static_cast<unsigned char>(in[i]) & 0x3F);

		if (cp >= 0x10000)
		{
			if (n + 2 > max)
				break;
			cp -= 0x10000;
			out[n++] = static_cast<SceWChar16>(0xD800 + (cp >> 10));
			out[n++] = static_cast<SceWChar16>(0xDC00 + (cp & 0x3FF));
		}
		else
		{
			out[n++] = static_cast<SceWChar16>(cp);
		}
	}
	out[n] = 0;
}

void tapKey(SDL_Keycode code)
{
	SDL_Event e = {};
	e.key.keysym.sym = code;
	e.key.keysym.scancode = SDL_GetScancodeFromKey(code);
	e.type = SDL_KEYDOWN;
	e.key.state = SDL_PRESSED;
	Keyboard::detail::pushEvent(e);
	e.type = SDL_KEYUP;
	e.key.state = SDL_RELEASED;
	Keyboard::detail::pushEvent(e);
}

std::string toUtf8(const SceWChar16 *in)
{
	std::string out;
	for (int i = 0; in[i] != 0; ++i)
	{
		unsigned int cp = in[i];
		if (cp >= 0xD800 && cp < 0xDC00 && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000)
		{
			cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
			++i;
		}

		if (cp < 0x80)
		{
			out += static_cast<char>(cp);
		}
		else if (cp < 0x800)
		{
			out += static_cast<char>(0xC0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			out += static_cast<char>(0xE0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else
		{
			out += static_cast<char>(0xF0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}
	return out;
}

}

void open(GuiTextField *field, bool submit)
{
	if (g_running || field == nullptr)
		return;

	int maxLength = field->getMaxStringLength();
	if (maxLength < 1)
		maxLength = 1;
	if (maxLength > SCE_IME_DIALOG_MAX_TEXT_LENGTH)
		maxLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;

	toUtf16(field->getText(), g_initial, maxLength);
	std::memset(g_input, 0, sizeof(g_input));

	SceImeDialogParam param;
	sceImeDialogParamInit(&param);
	param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
	param.languagesForced = SCE_FALSE;
	param.type = SCE_IME_TYPE_DEFAULT;
	param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
	param.maxTextLength = static_cast<SceUInt32>(maxLength);
	param.title = g_title;
	param.initialText = g_initial;
	param.inputTextBuffer = g_input;

	if (sceImeDialogInit(&param) < 0)
		return;

	g_field = field;
	g_submit = submit;
	g_running = true;
}

void forget(GuiTextField *field)
{
	if (g_field == field)
		g_field = nullptr;
}

void update()
{
	if (!g_running || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
		return;

	SceImeDialogResult result;
	std::memset(&result, 0, sizeof(result));
	sceImeDialogGetResult(&result);
	const bool entered = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
	if (entered && g_field != nullptr)
		g_field->setText(ChatAllowedCharacters::filterAllowedCharacters(toUtf8(g_input)));

	sceImeDialogTerm();
	if (g_submit && g_field != nullptr)
		tapKey(entered ? SDLK_RETURN : SDLK_ESCAPE);
	g_field = nullptr;
	g_running = false;
}

bool active()
{
	return g_running;
}

}
}

#endif
