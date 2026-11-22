/*
    This project is licensed under CC BY-NC 4.0
    https://creativecommons.org/licenses/by-nc/4.0*

    You are free to:
    • Share: Copy and redistribute the material in any medium or format.
    • Adapt: Remix, transform, and build upon the material*

    Under the following terms:
    • Attribution: You must give appropriate credit, provide a link to the original source repository, and indicate if changes were made.
    • Non-Commercial: You may not use the material for commercial purposes*

    You are not allowed to:
    • Sell: This license forbids selling original or modified material for commercial purposes.
    • Sublicense: This license forbids sublicensing original or modified material.

    © Copyright by dekrypte0 (https://github.com/Dekrypte0) and contributors.
    See https://github.com/Dekrypte0/cs2-external/-/blob/main/LICENSE for full details.
*/

#include <iostream>

#include "updater/Updater.hpp"
#include "core/stealth/Stealth.hpp"
#include "core/engine/Engine.hpp"
#include "gui/renderer/Renderer.hpp"

#include <external/exception.hpp>

int main()
{
    c_exception_handler::setup();

    LogHelper::Init();

    if (!stealth::Init()) {
        LOGF(FATAL, "Stealth layer failed to initialize, syscall path unavailable");
        stealth::Shutdown();
        LogHelper::Destroy();
        return 1;
    }

    LOGF(INFO, "Compiled {}, welcome aboard!", __TIMESTAMP__);

    // Needs to be ran as ADMINISTRATOR. Above-normal only: a high-priority
    // overlay spinning next to the game steals frame time from it.
    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS))
        LOGF(WARNING, "Could not set application process priority to ABOVE_NORMAL");

    if (!Updater::Init() || !Updater::Process()) {
        LOGF(FATAL, "Updater failed to run, the application has not verified its status, execution its not recommended");
        LOGF(INFO, "Press any key to ignore and continue execution...");
        std::cin.get();
    }

    if (!Engine::Init()) {
        LOGF(FATAL, "Engine failed to initialize, cannot continue execution");
        goto exit;
    }

    if (!Renderer::Init()) {
        LOGF(FATAL, "Renderer failed to initialize, cannot continue execution");
        goto exit;
    }

    LOGF(INFO, "Everything setup and ready, just... make sure you are not in \"Full Screen\"!");

    // Locking
    Renderer::Thread();

exit:
    LOGF(INFO, "Thats it, im done, hope you had a great time!");
    stealth::Shutdown();
    LogHelper::Destroy();
    std::cin.get();
}