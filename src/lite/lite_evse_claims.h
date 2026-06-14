#pragma once
#include <stdint.h>

// Claim target state (mirrors upstream EvseState semantics). None = "no opinion"
// (a claim that doesn't touch state); Active = charging allowed; Disabled = stop.
enum class EvseState : uint8_t { None = 0, Active = 1, Disabled = 2 };

// Control-client identifiers — values verbatim from upstream evse_man.h so lifted
// modules use identical names.
typedef uint32_t EvseClient;
static const EvseClient EvseClient_OpenEVSE_Manual      = 0x00010001;
static const EvseClient EvseClient_OpenEVSE_Divert      = 0x00010002;
static const EvseClient EvseClient_OpenEVSE_Boost       = 0x00010003;
static const EvseClient EvseClient_OpenEVSE_Schedule    = 0x00010004;
static const EvseClient EvseClient_OpenEVSE_Limit       = 0x00010006;
static const EvseClient EvseClient_OpenEVSE_Error       = 0x00010007;
static const EvseClient EvseClient_OpenEVSE_Ohm         = 0x00010008;
static const EvseClient EvseClient_OpenEVSE_OCPP        = 0x00010009;
static const EvseClient EvseClient_OpenEVSE_RFID        = 0x0001000A;
static const EvseClient EvseClient_OpenEVSE_MQTT        = 0x0001000B;
static const EvseClient EvseClient_OpenEVSE_Shaper      = 0x0001000C;
static const EvseClient EvseClient_OpenEVSE_TempThrottle = 0x0001000D;
static const EvseClient EvseClient_NULL                 = UINT32_MAX;

// Priority levels — verbatim from upstream. Higher wins a field.
static const int EvseManager_Priority_Default = 10;
static const int EvseManager_Priority_Divert  = 50;
static const int EvseManager_Priority_Timer   = 100;
static const int EvseManager_Priority_Boost   = 200;
static const int EvseManager_Priority_API     = 500;
static const int EvseManager_Priority_MQTT    = 500;
static const int EvseManager_Priority_Ohm     = 500;
static const int EvseManager_Priority_Manual  = 1000;
static const int EvseManager_Priority_RFID    = 1030;
static const int EvseManager_Priority_OCPP    = 1050;
static const int EvseManager_Priority_Limit   = 1100;
static const int EvseManager_Priority_Safety  = 5000;
static const int EvseManager_Priority_Error   = 10000;
