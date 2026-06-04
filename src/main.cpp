#include "DemoApp.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    woah::DemoApp app;
    if (!app.Initialize(instance, showCommand))
    {
        return 1;
    }

    return app.Run();
}
