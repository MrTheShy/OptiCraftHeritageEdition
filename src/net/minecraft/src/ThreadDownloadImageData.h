#pragma once

#include <atomic>
#include <string>

class BufferedImage;
class ImageBuffer;
class ThreadDownloadImage;

// net.minecraft.src.ThreadDownloadImageData
class ThreadDownloadImageData
{
public:
	ThreadDownloadImageData(const std::string &s, ImageBuffer *imagebuffer);
	~ThreadDownloadImageData();

	std::atomic<BufferedImage *> image;
	int referenceCount;
	int textureName;
	bool textureSetupComplete;
	// Deleting the data waits for its download; this says whether it would.
	bool downloadFinished() const;

private:
	ThreadDownloadImage *downloadThread;
};
