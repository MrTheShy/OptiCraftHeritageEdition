#ifdef PS2_PLATFORM

#include "ps2/system/Ps2EarlyCrash.h"

#include <cstdlib>
#include <exception>

#include "platform/Diagnostics.h"
#include "platform/Log.h"
#include "platform/WorldLoadTrace.h"

#if MC_LOG_LEVEL > 0
#include <ee_debug.h>
#include <sio.h>
#endif

// The PS2 counterpart of src/wii/system/WiiEarlyCrash.cpp, and the same shape.
//
// Minecraft::run() catches MinecraftException, bad_alloc, std::exception and
// (...), so an exception raised inside the game loop is already reported. What
// had no handler on this console is everything that never reaches a catch:
// thrown before the loop is entered, thrown while unwinding, or thrown out of a
// noexcept context. All of those go to std::terminate, whose default is
// abort(), and abort() on the EE stops with the last framebuffer still on
// screen and nothing in debug.log. That is indistinguishable from a hang, which
// is exactly the failure this port has been chasing.
//
// The Wii side routes through CrashHandler::Crash(); that is a PC/Wii path, so
// this one writes the same facts to the log instead, which is the sink that
// survives when the renderer no longer does.
namespace
{
void terminateHandler()
{
	// Reinstall the default first: if anything below throws, the second
	// terminate must not re-enter this handler and recurse.
	std::set_terminate(std::abort);

	const char* what = nullptr;
	if (std::current_exception())
	{
		try
		{
			std::rethrow_exception(std::current_exception());
		}
		catch (const std::exception& exception)
		{
			what = exception.what();
		}
		catch (...)
		{
			what = "(non-standard exception)";
		}
	}

	MC_LOG_ERROR("crash", "[PS2] terminate: %s\n",
	             what != nullptr ? what : "(no active exception)");

	const char* stage = WorldLoadTrace::currentStage();
	if (stage[0] != '\0')
		MC_LOG_ERROR("crash", "[PS2] terminate during world load: %s\n", stage);

	platformMemoryCheckpoint("terminate");

	// Only the lines an actual allocation failure already captured. Capturing
	// here would stamp "OUT OF MEMORY" on every terminate, including the ones
	// that have nothing to do with the heap.
	for (int i = 0; i < 5; ++i)
	{
		const char* line = platformOomDiagnosticLine(i);
		if (line != nullptr && line[0] != '\0')
			MC_LOG_ERROR("crash", "[PS2] last alloc failure: %s\n", line);
	}

	std::abort();
}
}

#if MC_LOG_LEVEL > 0
// CPU exceptions (bad loads/stores, bus errors) never reach a C++ handler: the
// EE just stops. This one writes the faulting PC, the caller (ra), the bad
// address and the top of the stack to the EE serial console, which PCSX2 keeps
// in emulog.txt (EE console logging on). Map the PCs with build/<dir>/OptiCraft.map.
// Only raw register writes here: the heap, newlib and the log may be what broke.
namespace
{
void sioHex(const char *label, u32 value)
{
	static const char digits[] = "0123456789abcdef";
	sio_putsn(label);
	char out[9];
	for (int i = 0; i < 8; ++i)
		out[i] = digits[(value >> (28 - 4 * i)) & 0xf];
	out[8] = '\0';
	sio_putsn(out);
}

int cpuExceptionHandler(EE_RegFrame *frame)
{
	sio_putsn("\n[CRASH]");
	sioHex(" cause=", frame->cause);
	sioHex(" epc=", frame->epc);
	sioHex(" ra=", frame->ra[0]);
	sioHex(" badvaddr=", frame->badvaddr);
	sioHex(" sp=", frame->sp[0]);
	sio_puts("");
	// Return addresses saved further up the stack: candidates for the call chain.
	const u32 sp = frame->sp[0];
	if (sp >= 0x00100000u && sp < 0x02000000u - 256u)
	{
		sio_putsn("[CRASH] stack");
		const u32 *words = reinterpret_cast<const u32 *>(sp);
		for (int i = 0; i < 64; ++i)
			if (words[i] >= 0x00100000u && words[i] < 0x00800000u) // EE user code starts at 1 MB
				sioHex(" ", words[i]);
		sio_puts("");
	}
	for (;;)
		;
	return 0;
}
}
#endif

namespace Ps2EarlyCrash
{
void install()
{
	std::set_terminate(terminateHandler);
#if MC_LOG_LEVEL > 0
	ee_dbg_install(1);
	for (int cause = 1; cause <= 7; ++cause) // TLB mod/load/store, address error, bus error
		ee_dbg_set_level1_handler(cause, cpuExceptionHandler);
#endif
}
}

#endif // PS2_PLATFORM
