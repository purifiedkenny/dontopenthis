#include <Pch.h>
#include <WinDef.h>
#include <cstdlib>
#include "Game/Core.h"
#include "Game/Entity/Cache.h"

bool debug = true; // global debug bool - set false once stable

/*
* Made by -- Jouh
* UnknownCheats -- Jouh
*/

int main()
{
    SetConsoleTitleA("Valorant Free DMA - Jouh");
    settings::loadSettings("config.txt");

    // ---- DMA init: retry instead of one-shot ----
    bool dmaReady = false;
    for (int attempt = 1; attempt <= 10; attempt++) {
        if (mem.Init("Valorant-Win64-Shipping.exe", true, debug)) {
            dmaReady = true;
            break;
        }
        LOG("DMA init attempt %d/10 failed\n", attempt);
        Sleep(1500);
    }
    if (!dmaReady) {
        LOG("DMA never initialized - card plugged in? Valorant running?\n");
        system("pause");
        return 1;
    }

    // ---- base address: retry, then hard fail ----
    for (int attempt = 0; attempt < 10 && !GameCache::BaseAddress; attempt++) {
        GameCache::BaseAddress = mem.GetBaseDaddy("Valorant-Win64-Shipping.exe");
        if (!GameCache::BaseAddress) Sleep(1000);
    }
    if (!GameCache::BaseAddress) {
        LOG("Failed to get base address\n");
        system("pause");
        return 1;
    }
    LOG("BaseAddress: 0x%p\n", (void*)GameCache::BaseAddress);

    if (!init()) {
        LOG("Failed to init GUI\n");
        system("pause");
        return 1;
    }

    if (!mem.GetKeyboard()->InitKeyboard()) {
        LOG("Failed to init keyboard - hotkeys/aimkey may not work\n");
    }

    bool kmBoxSuccess = kmBox::init(settings::kmbox::baudrate);
    bool kmNetSuccess = kmNet_init(settings::kmbox::ip, settings::kmbox::port, settings::kmbox::uuid) == 0;

    if (kmNetSuccess) {
        settings::kmbox::kmboxB = false;
        settings::kmbox::kmboxNet = true;
        std::cout << "Found KmBox .NET" << std::endl;
    }

    if (kmBoxSuccess) {
        settings::kmbox::kmboxB = true;
        settings::kmbox::kmboxNet = false;
        std::cout << "Found kmBox B+ " << std::endl;
    }

    std::thread(&Core::InitGame, InitCore, debug).detach();

    render_loop();
    return 0;
}