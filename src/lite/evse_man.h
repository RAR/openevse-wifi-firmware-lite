#pragma once
// Compatibility shim: lifted standard-fw control modules #include "evse_man.h"
// and use EvseManager*. In the lite build that resolves here, aliasing the
// lite control manager so those modules compile unmodified.
#include "lite_evse_claims.h"
#include "lite_evse_properties.h"
#include "lite_evse_manager.h"

using EvseManager = LiteEvseManager;
