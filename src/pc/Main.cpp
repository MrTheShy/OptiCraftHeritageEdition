#define SDL_MAIN_HANDLED
#include "SDL.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "client/Minecraft.h"
#include "net/minecraft/src/GameResources.h"
#include "net/minecraft/src/Tessellator.h"

#include "external/SDLException.h"

#include "lwjgl/GLContext.h"
#include "pc/render/PcRenderBackend.h"

#ifdef VITA_PLATFORM
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

extern "C"
{
int _newlib_heap_size_user = 192 * 1024 * 1024;
}
#endif

namespace
{
std::string trimUsername(std::string value)
{
	if (value.size() >= 3 &&
		static_cast<unsigned char>(value[0]) == 0xef &&
		static_cast<unsigned char>(value[1]) == 0xbb &&
		static_cast<unsigned char>(value[2]) == 0xbf)
	{
		value.erase(0, 3);
	}

	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return "";
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

PcRenderBackendType loadRenderBackendPreference()
{
	const std::filesystem::path path =
		std::filesystem::path(GameResources::getExeDir()) / ".minecraft" / "options.txt";
	std::ifstream input(path);
	std::string line;
	while (input && std::getline(input, line))
	{
		if (line.rfind("renderBackend:", 0) != 0)
			continue;
		return pcRenderBackendFromString(line.substr(14));
	}
	return PcRenderBackendType::OpenGL;
}

int loadOptiFineAaLevel()
{
	const std::filesystem::path path =
		std::filesystem::path(GameResources::getExeDir()) / ".minecraft" / "options.txt";
	std::ifstream input(path);
	std::string line;
	while (input && std::getline(input, line))
	{
		if (line.rfind("ofAaLevel:", 0) != 0)
			continue;
		try
		{
			const int value = std::stoi(line.substr(10));
			switch (value)
			{
			case 2: case 4: case 8: case 16: return value;
			default: return 0;
			}
		}
		catch (...)
		{
			return 0;
		}
	}
	return 0;
}

std::string loadUsername()
{
	const std::filesystem::path path =
		std::filesystem::path(GameResources::getExeDir()) / "username.txt";
	std::ifstream input(path);
	std::string username;
	if (input && std::getline(input, username))
		username = trimUsername(username);

	if (!username.empty())
		return username;

	username = "Player";
	std::ofstream output(path, std::ios::trunc);
	if (output)
		output << username << '\n';
	return username;
}
}

#ifdef VITA_PLATFORM
// vita-elf-create fails with "segment 1 overlaps" when the code segment ends
// less than ~5 KB before a 64 KB boundary. This padding pushes the end past it.
// If the error comes back after code changes, resize the array
// (check the segment layout with `arm-vita-eabi-readelf -l`).
// cmake/vita.cmake links with --undefined=g_vitaSegmentPad so --gc-sections
// keeps it.
extern "C" const char g_vitaSegmentPad[28672] __attribute__((used, aligned(4))) = {1};
#endif

int main(int argc, char *argv[])
{
#ifdef VITA_PLATFORM
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
#endif
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_AUDIO) < 0)
		throw SDLException();
	pcRenderBackendSetRequested(loadRenderBackendPreference());
	lwjgl::GLContext::setRequestedSamples(loadOptiFineAaLevel());
	lwjgl::GLContext::instantiate();
#if PLATFORM_PC_LEGACY
	Tessellator::convertQuadsToTriangles = !pcRenderBackendIsDirect3D9();
#endif

	jstring username = loadUsername();
	if (argc >= 2 && std::strlen(argv[1]) > 0)
		username = argv[1];

	jstring auth = "-";
	if (argc >= 3 && std::strlen(argv[2]) > 1)
		auth = argv[2];

	Minecraft::start(&username, &auth);

	return 0;
}
