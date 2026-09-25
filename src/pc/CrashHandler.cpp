#include "CrashHandler.h"

#include "SDL_messagebox.h"

#ifdef VITA_PLATFORM
#include <cstdio>
#endif

namespace CrashHandler
{

void Crash(const std::string &message, const std::string &stackTrace)
{
	std::string text = message + "\n\n" + stackTrace;
#ifdef VITA_PLATFORM
	if (FILE *f = std::fopen("ux0:data/opticraft/crash.txt", "a"))
	{
		std::fputs(text.c_str(), f);
		std::fputs("\n\n", f);
		std::fclose(f);
	}
#endif
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Minecraft has crashed!", text.c_str(), nullptr);
}

}
