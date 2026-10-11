#pragma once

#include <atomic>
#include <string>
#include <thread>

class ImageBuffer;
class ThreadDownloadImageData;

// net.minecraft.src.ThreadDownloadImage
class ThreadDownloadImage
{
public:
	ThreadDownloadImage(ThreadDownloadImageData *threaddownloadimagedata, const std::string &s, ImageBuffer *imagebuffer);
	~ThreadDownloadImage();

	void start();
	void run();
	// True once run() has returned: the destructor will not wait.
	bool finished() const { return done.load(); }

private:
	std::string location;
	ImageBuffer *buffer;
	ThreadDownloadImageData *imageData;
	std::thread worker;
	std::atomic<bool> done{false};
};
