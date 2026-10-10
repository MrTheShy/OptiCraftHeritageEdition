#include "platform/Input.h"

#ifdef VITA_PLATFORM
#include "lwjgl/VitaPad.h"
#endif

PlatformTextInputSnapshot platformTextInputSnapshot(int port)
{
    (void)port;
    return {};
}

PlatformGamepadSnapshot platformGamepadSnapshot(int port)
{
#ifdef VITA_PLATFORM
    PlatformGamepadSnapshot snapshot;
    if (port == 0)
    {
        snapshot.connected = true;
        lwjgl::VitaPad::leftStick(snapshot.leftX, snapshot.leftY);
    }
    return snapshot;
#else
    (void)port;
    return {};
#endif
}

PlatformGamepadSnapshot platformRawGamepadSnapshot(int port)
{
    (void)port;
    return {};
}

int platformMenuPad()
{
    return 0;
}

bool platformMenuPointerActive()
{
    return true;
}

bool platformMenuCursorVisible()
{
    return false;
}

void platformSetMenuCursor(int x, int y)
{
    (void)x;
    (void)y;
}

const PlatformKeyboardHints& platformKeyboardHints()
{
    static const PlatformKeyboardHints hints;
    return hints;
}

const char* platformInputDebugLine()
{
    return "";
}
