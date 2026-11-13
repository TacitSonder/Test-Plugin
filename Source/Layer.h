#pragma once

#include "LayerConfig.h"
#include "LayerParams.h"
#include "SampleLibrary.h"
#include "SamplerEngine.h"

// One layer: a bank of samples, its speed / level parameters, and the engine that plays it.
// Every note (MIDI or the Play button) triggers all layers; their outputs are summed.
struct Layer
{
    explicit Layer (perf::Monitor& pm) : library (pm), params (pm), engine (library, params, pm) {}

    SampleLibrary library;
    LayerParams params;
    SamplerEngine engine;      // declared last: it refers to the two above
};
