#pragma once
#include "structs.hpp"

class Core
{
public:
    void InitGame(bool debug = true);
    bool ScanForGWorld();
    uintptr_t ResolveGWorld();
};

inline Core InitCore;