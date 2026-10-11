#include "platform/Log.h"
#include "ThreadDownloadImage.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <vector>

#include "ImageBuffer.h"
#include "ThreadDownloadImageData.h"
#include "java/JavaNetwork.h"
#include "java/BufferedImage.h"

ThreadDownloadImage::ThreadDownloadImage(ThreadDownloadImageData *threaddownloadimagedata, const std::string &s, ImageBuffer *imagebuffer)
	: location(s)
	, buffer(imagebuffer)
	, imageData(threaddownloadimagedata)
{
}

ThreadDownloadImage::~ThreadDownloadImage()
{
	if (worker.joinable())
		worker.join();
	delete buffer;
	buffer = nullptr;
}

void ThreadDownloadImage::start()
{
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM)
	// The Wii backend currently exposes multiplayer TCP only. readUrl() returns
	// false immediately, so execute it inline instead of invoking std::thread,
	// which is an ENOSYS stub in devkitPPC. PS2: same readUrl stub, and
	// std::thread goes through pthreads-embedded, never initialised here
	// (pte_osInit): a thread per spawned player crashed in pte_osThreadCreate.
	run();
#else
	try
	{
		worker = std::thread(&ThreadDownloadImage::run, this);
	}
	catch (const std::system_error &)
	{
		done = true; // no thread left (many skins at once): keep the default skin
	}
#endif
}

namespace
{
// Skins and capes are small PNGs. Reading the header first keeps a server from
// making the client decode a huge image.
constexpr std::size_t kMaxSkinBytes = 64u * 1024u;
constexpr unsigned kMaxSkinSide = 256;

bool smallPng(const std::vector<unsigned char> &data)
{
	static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	if (data.size() < 24 || !std::equal(signature, signature + 8, data.begin()))
		return false;
	auto readBE32 = [&](std::size_t at)
	{
		return (unsigned)data[at] << 24 | (unsigned)data[at + 1] << 16 | (unsigned)data[at + 2] << 8 | data[at + 3];
	};
	const unsigned width = readBE32(16);
	const unsigned height = readBE32(20);
	return width > 0 && height > 0 && width <= kMaxSkinSide && height <= kMaxSkinSide;
}
}

void ThreadDownloadImage::run()
{
	try
	{
		std::vector<unsigned char> data;
		if (!JavaNetwork::readUrl(location, data, kMaxSkinBytes) || !smallPng(data))
		{
			done = true;
			return;
		}
		std::string bytes(reinterpret_cast<const char *>(data.data()), data.size());
		std::istringstream input(bytes, std::ios::binary);
		BufferedImage *downloaded = new BufferedImage(BufferedImage::ImageIO_read(input));
		BufferedImage *parsed = buffer == nullptr ? downloaded : buffer->parseUserSkin(downloaded);
		if (parsed != downloaded)
			delete downloaded;

		// Publish the fully-built image atomically. If a retry ever replaces an
		// existing image, release the previous allocation instead of leaking it.
		BufferedImage *old = imageData->image.exchange(parsed);
		if (old != parsed)
			delete old;
	}
	catch (std::exception &exception)
	{
		MC_LOG_ERROR("game", "%s\n", exception.what());
	}
	done = true;
}
