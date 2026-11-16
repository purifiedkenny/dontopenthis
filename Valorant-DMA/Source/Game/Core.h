#pragma once
#include "structs.hpp"

class Core
{
public:
    void InitGame(bool debug = true);
    bool ScanForGWorld();
};

inline Core InitCore;