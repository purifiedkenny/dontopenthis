#include <Pch.h>
#include <intrin.h>
#include "Core.h"
#include "Entity/Cache.h"

static constexpr uintptr_t kStateKey = 0x38;
static constexpr uintptr_t kStateKeyAlt = 0x08;
static constexpr uintptr_t kOwningWorld = 0xC0;
static constexpr uint32_t  kSlots = 7;
static constexpr uint64_t  kMixKey = 0x2545F4914F6CDD1DULL;

static const uintptr_t kStateCandidates[] = {
    0xD173910, 0xCEC5640, 0xCFD1D00, 0xCEC9500
};

static __forceinline uint64_t RotR64(uint64_t v, unsigned n) { return (v >> n) | (v << (64 - n)); }
static __forceinline uint64_t RotL64(uint64_t v, unsigned n) { return (v << n) | (v >> (64 - n)); }

static __forceinline uintptr_t DecryptXorKeys(uint32_t key, const uintptr_t* state)
{
    uint64_t mixed = kMixKey * static_cast<uint64_t>(key ^ ((key ^ (key >> 15)) >> 12) ^ (key << 25));
    uint32_t index = static_cast<uint32_t>(mixed % kSlots);
    uint32_t high = static_cast<uint32_t>(mixed >> 32);
    uint64_t v = state[index];

    uint64_t sum = static_cast<uint64_t>(index) + high;
    unsigned mod = static_cast<unsigned>(sum % 63);
    unsigned div = static_cast<unsigned>(sum / 63);
    unsigned low = static_cast<unsigned>(sum % 256);
    unsigned shiftR = mod + 1;
    unsigned shiftL = 63 * div - low + 63;

    switch (index)
    {
    case 0:
        v = ~(v - (high - 1));
        break;
    case 1:
        v ^= static_cast<uint32_t>(index + high) ^ static_cast<uint32_t>(high + 2 * index);
        break;
    case 2:
    case 3:
    {
        uint64_t a = ((v >> 1) ^ ((v >> 1) ^ (v << 1)) & 0xAAAAAAAAAAAAAAAAULL) >> 2;
        uint64_t b = a ^ (a ^ (4ULL * ((v >> 1) ^ ((v >> 1) ^ (v << 1)) & 0xAAAAAAAAAAAAAAAAULL))) & 0xCCCCCCCCCCCCCCCCULL;
        uint64_t c = b >> 4;
        uint64_t t = ((c ^ (c ^ (b << 4)) & 0xF0F0F0F0F0F0F0F0ULL) >> 8) ^
            (((c ^ (c ^ (b << 4)) & 0xF0F0F0F0F0F0F0F0ULL) >> 8) ^
                ((c ^ (c ^ (b << 4)) & 0xF0F0F0F0F0F0F0F0ULL) << 8)) & 0xFF00FF00FF00FF00ULL;
        uint64_t sw = (t << 32) | (t >> 32);
        v = (index == 2) ? RotR64(sw, shiftR) : RotL64(sw, shiftR);
        break;
    }
    case 4:
        v += static_cast<uint64_t>(high + 2 * index) + static_cast<uint64_t>(index + high);
        break;
    case 5:
        v = (~v << shiftL) | (~v >> shiftR);
        break;
    case 6:
    {
        uint64_t a = ((v >> 1) ^ ((v >> 1) ^ (v << 1)) & 0xAAAAAAAAAAAAAAAAULL) >> 2;
        uint64_t b = a ^ (a ^ (4ULL * ((v >> 1) ^ ((v >> 1) ^ (v << 1)) & 0xAAAAAAAAAAAAAAAAULL))) & 0xCCCCCCCCCCCCCCCCULL;
        uint64_t c = (b >> 4) ^ ((b >> 4) ^ (b << 4)) & 0xF0F0F0F0F0F0F0F0ULL;
        uint64_t t = (c >> 8) ^ ((c >> 8) ^ (c << 8)) & 0xFF00FF00FF00FF00ULL;
        uint64_t sw = (t << 32) | (t >> 32);
        v = sw - static_cast<uint32_t>(index + high);
        break;
    }
    }
    return v ^ key;
}

static bool IsHeap(uint64_t p, uint64_t base, uint64_t size)
{
    return p >= 0x10000 && p <= 0x7FFFFFFFFFFF
        && (p < base || p >= base + size) && !(p & 7);
}

static bool WorldOk(uintptr_t w, uintptr_t base, uintptr_t size)
{
    if (!IsHeap(w, base, size)) return false;
    const auto lvl = mem.Read<uintptr_t>(w + offsets::PersistentLevel);
    if (!IsHeap(lvl, base, size) || lvl == w) return false;
    if (mem.Read<uintptr_t>(lvl + kOwningWorld) == w) return true;
    const auto n = mem.Read<int>(lvl + offsets::ActorArray + 0x8);
    return n > 0 && n < 10000;
}

uintptr_t Core::ResolveGWorld()
{
    const auto base = GameCache::BaseAddress;
    const auto size = mem.GetBaseSize("Valorant-Win64-Shipping.exe");
    if (!base || !size) { LOG("[gw] no base/size\n"); return 0; }

    static uintptr_t s_logged = 0;

    auto accept = [&](uintptr_t c) -> uintptr_t
        {
            if (!IsHeap(c, base, size)) return 0;
            if (WorldOk(c, base, size)) return c;
            const auto inner = mem.Read<uintptr_t>(c);
            if (inner && inner != c && WorldOk(inner, base, size)) return inner;
            return 0;
        };

    auto report = [&](uintptr_t w, const char* how) -> uintptr_t
        {
            if (w != s_logged)
            {
                s_logged = w;
                LOG("[gw] %s -> 0x%p\n", how, (void*)w);
            }
            return w;
        };

    if (auto w = accept(mem.Read<uintptr_t>(base + offsets::GWorld)))
        return report(w, "plain slot");

    static bool dumped = false;
    if (!dumped)
    {
        dumped = true;
        for (uintptr_t off : kStateCandidates)
        {
            LOG("[gw] dump 0x%llX:", (unsigned long long)off);
            for (int i = 0; i < 8; ++i)
                LOG(" %016llX", (unsigned long long)mem.Read<uint64_t>(base + off + i * 8));
            LOG("\n");
        }
    }

    for (uintptr_t off : kStateCandidates)
    {
        const uint32_t k38 = mem.Read<uint32_t>(base + off + kStateKey);
        const uint32_t k0 = mem.Read<uint32_t>(base + off);
        const uint32_t key = k38 ? k38 : k0;
        if (!key) continue;

        uintptr_t state[kSlots]{};
        mem.Read(base + (k38 ? off : off + kStateKeyAlt), state, sizeof(state));

        const uintptr_t dec = DecryptXorKeys(key, state);
        if (!dec) continue;

        if (auto w = accept(dec))
            return report(w, "state");
        if (auto w = accept(mem.Read<uintptr_t>(dec)))
            return report(w, "state deref");
    }

    return 0;
}

auto Core::ScanForGWorld() -> bool
{
    const auto base = GameCache::BaseAddress;
    const auto size = mem.GetBaseSize("Valorant-Win64-Shipping.exe");
    if (!base || !size) { LOG("[gw] no base/size\n"); return false; }

    IMAGE_DOS_HEADER dos{};
    if (!mem.Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) { LOG("[gw] bad dos\n"); return false; }
    IMAGE_NT_HEADERS64 nt{};
    if (!mem.Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) { LOG("[gw] bad nt\n"); return false; }
    const auto secTable = base + dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;

    const size_t CHUNK = 4 * 1024 * 1024;
    std::vector<uint8_t> buf(CHUNK + 8);
    int hits = 0;

    for (unsigned s = 0; s < nt.FileHeader.NumberOfSections; s++)
    {
        IMAGE_SECTION_HEADER sh{};
        if (!mem.Read(secTable + s * sizeof(IMAGE_SECTION_HEADER), &sh, sizeof(sh))) continue;
        const DWORD vSize = sh.Misc.VirtualSize;
        const bool exec = (sh.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (exec || !vSize) continue;

        char name[9] = { 0 };
        memcpy(name, sh.Name, 8);
        LOG("[gw] section %s va=0x%lX size=0x%lX\n", name,
            (unsigned long)sh.VirtualAddress, (unsigned long)vSize);

        for (size_t off = 0; off < vSize; off += CHUNK)
        {
            size_t toRead = std::min<size_t>(CHUNK + 8, (size_t)vSize - off);
            if (!mem.Read(base + sh.VirtualAddress + off, buf.data(), toRead)) continue;

            for (size_t i = 0; i + 8 <= toRead; i += 8)
            {
                auto candidate = *reinterpret_cast<uint64_t*>(buf.data() + i);
                if (!IsHeap(candidate, base, size)) continue;

                uintptr_t world = 0;
                if (WorldOk(candidate, base, size)) world = candidate;
                else
                {
                    const auto inner = mem.Read<uintptr_t>(candidate);
                    if (inner && inner != candidate && WorldOk(inner, base, size)) world = inner;
                }
                if (!world) continue;

                LOG("[gw] HIT rva=0x%llX world=0x%p\n",
                    (unsigned long long)(sh.VirtualAddress + off + i), (void*)world);
                offsets::GWorld = sh.VirtualAddress + off + i;
                LOG("[gw] ADOPTED GWorld rva=0x%llX\n", (unsigned long long)offsets::GWorld);
                return true;
            }
            if (++hits > 60) return false;
        }
    }

    LOG("[gw] no hit\n");
    return false;
}

auto Core::InitGame(bool debug) -> void
{
    ULONGLONG lastBaseCheck = 0;
    ULONGLONG lastLog = 0;

    while (true)
    {
        const auto now = GetTickCount64();

        if (now - lastBaseCheck > 1000)
        {
            lastBaseCheck = now;
            auto base = mem.GetBaseDaddy("Valorant-Win64-Shipping.exe");
            if (base && base != GameCache::BaseAddress)
            {
                GameCache::BaseAddress = base;
                LOG("BaseAddress refreshed: 0x%p\n", (void*)GameCache::BaseAddress);
            }
        }
        if (!GameCache::BaseAddress) { Sleep(250); continue; }

        const bool logNow = debug && (now - lastLog >= 250);
        if (logNow) lastLog = now;

        const auto candidate = ResolveGWorld();
        if (!candidate)
        {
            if (logNow) LOG("GWorld unresolved\n");
            Sleep(8);
            continue;
        }

        const auto plevel = mem.Read<uintptr_t>(candidate + offsets::PersistentLevel);
        const auto actorCount = plevel ? mem.Read<int>(plevel + offsets::ActorArray + 0x8) : 0;

        if (logNow) LOG("GWorld=0x%p PersistentLevel=0x%p actorCount=%d\n",
            (void*)candidate, (void*)plevel, actorCount);

        GameCache::UWorld = candidate;
        GameCache::PersistentLevel = plevel;

        GameCache::GameState = mem.Read<uintptr_t>(GameCache::UWorld + offsets::GameState);
        GameCache::GameInstance = mem.Read<uintptr_t>(GameCache::UWorld + offsets::GameInstance);

        auto templocal = mem.Read<uintptr_t>(GameCache::GameInstance + offsets::LocalPlayers);
        GameCache::LocalPlayers = mem.Read<uintptr_t>(templocal);

        GameCache::PlayerController = mem.Read<uintptr_t>(GameCache::LocalPlayers + offsets::PlayerController);

        if (GameCache::PlayerController)
        {
            GameCache::LocalPawn = mem.Read<uintptr_t>(GameCache::PlayerController + offsets::AcknowledgedPawn);
            GameCache::PlayerCameraManager = mem.Read<uintptr_t>(GameCache::PlayerController + offsets::CameraManager);
        }

        GameCache::PlayerCount = mem.Read<int>(GameCache::PersistentLevel + offsets::ActorArray + 0x8);

        if (logNow) LOG("Pawn: 0x%p CameraManager: 0x%p Count: %d\n",
            (void*)GameCache::LocalPawn, (void*)GameCache::PlayerCameraManager, GameCache::PlayerCount);

        Sleep(8);
    }
}