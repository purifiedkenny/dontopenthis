#include <Pch.h>
#include "Core.h"
#include "Entity/Cache.h"

auto Core::ScanForGWorld() -> bool
{
    const auto base = GameCache::BaseAddress;
    const auto size = mem.GetBaseSize("Valorant-Win64-Shipping.exe");
    if (!base || !size) { LOG("[gw] no base/size\n"); return false; }

    auto isHeap = [&](uint64_t p) {
        return p >= 0x10000 && p <= 0x7FFFFFFFFFFF && (p < base || p >= base + size) && !(p & 0x7);
        };

    IMAGE_DOS_HEADER dos{};
    if (!mem.Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) { LOG("[gw] bad dos\n"); return false; }
    IMAGE_NT_HEADERS64 nt{};
    if (!mem.Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) { LOG("[gw] bad nt\n"); return false; }
    const auto secTable = base + dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;

    const size_t CHUNK = 4 * 1024 * 1024;
    std::vector<uint8_t> buf(CHUNK + 8);
    int hits = 0;

    for (unsigned s = 0; s < nt.FileHeader.NumberOfSections; s++) {
        IMAGE_SECTION_HEADER sh{};
        if (!mem.Read(secTable + s * sizeof(IMAGE_SECTION_HEADER), &sh, sizeof(sh))) continue;
        const DWORD vSize = sh.Misc.VirtualSize;
        const bool exec = (sh.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (exec || !vSize) continue;

        char name[9] = { 0 };
        memcpy(name, sh.Name, 8);
        LOG("[gw] section %s va=0x%lX size=0x%lX\n", name,
            (unsigned long)sh.VirtualAddress, (unsigned long)vSize);

        for (size_t off = 0; off < vSize; off += CHUNK) {
            size_t toRead = std::min<size_t>(CHUNK + 8, (size_t)vSize - off);
            if (!mem.Read(base + sh.VirtualAddress + off, buf.data(), toRead)) continue;

            for (size_t i = 0; i + 8 <= toRead; i += 8) {
                auto candidate = *reinterpret_cast<uint64_t*>(buf.data() + i);
                if (!isHeap(candidate)) continue;

                auto plevel = mem.Read<uintptr_t>(candidate + offsets::PersistentLevel);
                if (!isHeap(plevel)) continue;

                auto actorCount = mem.Read<int>(plevel + offsets::ActorArray + 0x8);
                if (actorCount <= 0 || actorCount > 200) continue;

                auto gstate = mem.Read<uintptr_t>(candidate + offsets::GameState);
                bool gstateOk = isHeap(gstate);

                LOG("[gw] HIT rva=0x%llX candidate=0x%p plevel=0x%p actorCount=%d gstateOk=%d\n",
                    (unsigned long long)(sh.VirtualAddress + off + i),
                    (void*)candidate, (void*)plevel, actorCount, gstateOk);

                if (gstateOk) {
                    offsets::GWorld = sh.VirtualAddress + off + i;
                    LOG("[gw] ADOPTED GWorld rva=0x%llX\n", (unsigned long long)offsets::GWorld);
                    return true;
                }
                if (++hits > 60) return false;
            }
        }
    }

    LOG("[gw] no hit (hits=%d)\n", hits);
    return false;
}

auto Core::InitGame(bool debug) -> void
{
    ULONGLONG lastBaseCheck = 0;
    ULONGLONG lastLog = 0;
    bool scanned = false;

    while (true)
    {
        const auto now = GetTickCount64();

        // re-resolve base once a second: ASLR / game restart invalidates it
        if (now - lastBaseCheck > 1000) {
            lastBaseCheck = now;
            auto base = mem.GetBaseDaddy("Valorant-Win64-Shipping.exe");
            if (base && base != GameCache::BaseAddress) {
                GameCache::BaseAddress = base;
                LOG("BaseAddress refreshed: 0x%p\n", (void*)GameCache::BaseAddress);
            }
        }
        if (!GameCache::BaseAddress) { Sleep(250); continue; }

        // run the GWorld scan exactly once, after we have a base address
        if (!scanned) { ScanForGWorld(); scanned = true; }

        // throttle debug spam to ~4/sec
        const bool logNow = debug && (now - lastLog >= 250);
        if (logNow) lastLog = now;

        // GWorld can alternate between the real world and a stale/decoy instance.
        // Validate by checking PersistentLevel + a sane actor count.
        auto candidate = mem.Read<uintptr_t>(GameCache::BaseAddress + offsets::GWorld);
        if (candidate < 0x10000 || candidate > 0x7FFFFFFFFFFF) {
            if (logNow) LOG("GWorld read invalid: 0x%p\n", (void*)candidate);
            Sleep(8);
            continue;
        }

        auto plevel = mem.Read<uintptr_t>(candidate + offsets::PersistentLevel);
        auto actorCount = plevel ? mem.Read<int>(plevel + offsets::ActorArray + 0x8) : 0;
        bool validWorld = plevel >= 0x10000 && actorCount > 0 && actorCount < 10000;

        if (logNow) LOG("GWorld=0x%p PersistentLevel=0x%p actorCount=%d valid=%d\n",
            (void*)candidate, (void*)plevel, actorCount, validWorld);

        if (!validWorld) { Sleep(8); continue; }

        GameCache::UWorld = candidate;
        GameCache::PersistentLevel = plevel;
        if (logNow) LOG("UWorld: 0x%p\n", (void*)GameCache::UWorld);

        GameCache::GameState = mem.Read<uintptr_t>(GameCache::UWorld + offsets::GameState);
        if (logNow) LOG("GameState: 0x%p\n", (void*)GameCache::GameState);

        GameCache::GameInstance = mem.Read<uintptr_t>(GameCache::UWorld + offsets::GameInstance);
        if (logNow) LOG("GameInstance: 0x%p\n", (void*)GameCache::GameInstance);

        auto templocal = mem.Read<uintptr_t>(GameCache::GameInstance + offsets::LocalPlayers);
        GameCache::LocalPlayers = mem.Read<uintptr_t>(templocal);
        if (logNow) LOG("LocalPlayers: 0x%p\n", (void*)GameCache::LocalPlayers);

        GameCache::PlayerController = mem.Read<uintptr_t>(GameCache::LocalPlayers + offsets::PlayerController);
        if (logNow) LOG("PlayerController: 0x%p\n", (void*)GameCache::PlayerController);

        if (GameCache::PlayerController)
        {
            GameCache::LocalPawn = mem.Read<uintptr_t>(GameCache::PlayerController + offsets::AcknowledgedPawn);
            if (logNow) LOG("LocalPawn: 0x%p\n", (void*)GameCache::LocalPawn);

            GameCache::PlayerCameraManager = mem.Read<uintptr_t>(GameCache::PlayerController + offsets::CameraManager);
            if (logNow) LOG("PlayerCameraManager: 0x%p\n", (void*)GameCache::PlayerCameraManager);
        }

        GameCache::PlayerCount = mem.Read<uintptr_t>(GameCache::PersistentLevel + offsets::ActorArray + 0x8);
        if (logNow) LOG("PlayerCount: %d\n", GameCache::PlayerCount);

        Sleep(8);
    }
}