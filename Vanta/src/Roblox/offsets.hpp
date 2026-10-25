// Z4 | Roblox macOS Intel
// Version: 0.744.0.7441080
// Build: version-546c6da637854de3
#pragma once
#include <cstdint>
#include <array>

#ifndef REBASE
#define REBASE(x) (x)
#endif

namespace Offsets
{
    inline constexpr const char* architecture = "x86_64";
    inline constexpr const char* luau = "0.744.0.7441080";
    inline constexpr const char* build = "version-546c6da637854de3";

    namespace Memory {
        inline const uintptr_t luaM_freearray = REBASE(0x2b2f9aa);
        inline const uintptr_t luaM_freegco = REBASE(0x2b2fa9b);
        inline const uintptr_t luaM_newgco = REBASE(0x2b2f4b8);
        inline const uintptr_t luaM_toobig = REBASE(0x2b2f1f0);
        inline const uintptr_t luaM_visitgco = REBASE(0x2b30108);
        inline const uintptr_t newblock = REBASE(0x2b2f215);
        inline const uintptr_t newclasspage = REBASE(0x2b302d1);
        inline const uintptr_t newpage = REBASE(0x2b2f608);
    }

    namespace Luau {
        inline const uintptr_t LuaVMLoad = REBASE(0x2b3bc62);
        inline const uintptr_t OpcodeLookupTable = REBASE(0x6673cf0);
        inline const uintptr_t loadsafe = REBASE(0x2b3bd86);
        inline const uintptr_t luaA_pushvalue = REBASE(0x2b16ff5);
        inline const uintptr_t luaA_toobject = REBASE(0x2b16f9c);
        inline const uintptr_t luaC_step = REBASE(0x2b287ce);
        inline const uintptr_t luaD_rawrunprotected = REBASE(0x2b26ba0);
        inline const uintptr_t luaD_throw = REBASE(0x2b26c41);
        inline const uintptr_t luaF_newLclosure = REBASE(0x2b28169);
        inline const uintptr_t luaF_newUpval = REBASE(0x2b28363);
        inline const uintptr_t luaF_newproto = REBASE(0x2b28094);
        inline const uintptr_t luaG_runerror = REBASE(0x2b25bf0);
        inline const uintptr_t luaH_clone = REBASE(0x2b381ba);
        inline const uintptr_t luaH_dummynode = REBASE(0x6673cb0);
        inline const uintptr_t luaH_new = REBASE(0x2b36b10);
        inline const uintptr_t luaL_argerrorL = REBASE(0x2b1bca8);
        inline const uintptr_t luaL_checklstring = REBASE(0x2b1c0d3);
        inline const uintptr_t luaL_checktype = REBASE(0x2b1c2b8);
        inline const uintptr_t luaL_errorL = REBASE(0x2b1bd75);
        inline const uintptr_t luaL_findtable = REBASE(0x2b1c7ef);
        inline const uintptr_t luaL_getmetafield = REBASE(0x2b1c5d8);
        inline const uintptr_t luaL_register = REBASE(0x2b1c6bb);
        inline const uintptr_t luaL_tagerror = REBASE(0x2b1c252);
        inline const uintptr_t luaL_typeerrorL = REBASE(0x2b1be2a);
        inline const uintptr_t luaO_chunkid = REBASE(0x2b30eaf);
        inline const uintptr_t luaO_nilobject = REBASE(0x6673b50);
        inline const uintptr_t luaO_pushfstring = REBASE(0x2b30e15);
        inline const uintptr_t luaO_pushvfstring = REBASE(0x2b30d61);
        inline const uintptr_t luaO_str2d = REBASE(0x2b30bc0);
        inline const uintptr_t luaT_init = REBASE(0x2b3a6bc);
        inline const uintptr_t luaT_objtypename = REBASE(0x2b3a8d7);
        inline const uintptr_t luaV_equal = REBASE(0x2b3e6db);
        inline const uintptr_t luaV_gettable = REBASE(0x2b3dc8a);
        inline const uintptr_t luaV_lessthan = REBASE(0x2b3e586);
        inline const uintptr_t luaV_settable = REBASE(0x2b3e0ec);
        inline const uintptr_t lua_checkstack = REBASE(0x2b17053);
        inline const uintptr_t lua_eception = REBASE(0x2b26c7e);
        inline const uintptr_t lua_encodepointer = REBASE(0x2b1b0cb);
        inline const uintptr_t lua_error = REBASE(0x2b170e1);
        inline const uintptr_t lua_gettop = REBASE(0x2b17404);
        inline const uintptr_t lua_namecallatom = REBASE(0x2b18253);
        inline const uintptr_t lua_newstate = REBASE(0x2b31bbd);
        inline const uintptr_t lua_pushfstringL = REBASE(0x2b18b14);
        inline const uintptr_t lua_pushvfstring = REBASE(0x2b18ab9);
        inline const uintptr_t lua_rawcheckstack = REBASE(0x2b276b9);
        inline const uintptr_t lua_resetthread = REBASE(0x2b319c7);
        inline const uintptr_t lua_type = REBASE(0x2b178a0);
        inline const uintptr_t lua_yield = REBASE(0x2b27af2);
        inline const uintptr_t luaopen_base = REBASE(0x2b1d2dc);
        inline const uintptr_t luaopen_bit32 = REBASE(0x2b1e0c7);
        inline const uintptr_t luaopen_buffer = REBASE(0x2b1e7b5);
        inline const uintptr_t luaopen_class = REBASE(0x2b23da2);
        inline const uintptr_t luaopen_coroutine = REBASE(0x2b23e82);
        inline const uintptr_t luaopen_debug = REBASE(0x2b2521d);
        inline const uintptr_t luaopen_integer = REBASE(0x2b2d080);
        inline const uintptr_t luaopen_math = REBASE(0x2b2e009);
        inline const uintptr_t luaopen_os = REBASE(0x2b30f9b);
        inline const uintptr_t luaopen_string = REBASE(0x2b3243c);
        inline const uintptr_t luaopen_table = REBASE(0x2b38d80);
        inline const uintptr_t luaopen_utf8 = REBASE(0x2b3a9e5);
        inline const uintptr_t luaopen_vector = REBASE(0x2b3b225);
        inline const uintptr_t luau_execute = REBASE(0x2b3fc9e);
        inline const uintptr_t pseudo2addr = REBASE(0x2b1bbad);
    }

    namespace LuaLib {
        inline const uintptr_t luaB_assert = REBASE(0x2b1d6be);
        inline const uintptr_t luaB_error = REBASE(0x2b1d733);
        inline const uintptr_t luaB_gcinfo = REBASE(0x2b1d7a8);
        inline const uintptr_t luaB_getfenv = REBASE(0x2b1d7d3);
        inline const uintptr_t luaB_getmetatable = REBASE(0x2b1d82b);
        inline const uintptr_t luaB_newproxy = REBASE(0x2b1d879);
        inline const uintptr_t luaB_next = REBASE(0x2b1d225);
        inline const uintptr_t luaB_pcall = REBASE(0x2b1d513);
        inline const uintptr_t luaB_pcallrun = REBASE(0x2b1d546);
        inline const uintptr_t luaB_print = REBASE(0x2b1d906);
        inline const uintptr_t luaB_rawequal = REBASE(0x2b1d9c9);
        inline const uintptr_t luaB_rawget = REBASE(0x2b1da11);
        inline const uintptr_t luaB_rawlen = REBASE(0x2b1dab4);
        inline const uintptr_t luaB_rawset = REBASE(0x2b1da5c);
        inline const uintptr_t luaB_select = REBASE(0x2b1db15);
        inline const uintptr_t luaB_setfenv = REBASE(0x2b1dbc0);
        inline const uintptr_t luaB_setmetatable = REBASE(0x2b1dcaa);
        inline const uintptr_t luaB_tonumber = REBASE(0x2b1dd65);
        inline const uintptr_t luaB_tostring = REBASE(0x2b1de9d);
        inline const uintptr_t luaB_type = REBASE(0x2b1decb);
        inline const uintptr_t luaB_typeof = REBASE(0x2b1df0c);
    }

    namespace Misc {
        inline const uintptr_t Print = REBASE(0x2db3af1);
        inline const uintptr_t luaF_freeproto = REBASE(0x2b284cb);
        inline const uintptr_t luaF_newCclosure = REBASE(0x2b282ee);
        inline const uintptr_t luaL_tostring = REBASE(0x2b1cfe4);
        inline const uintptr_t luaS_free = REBASE(0x2b32388);
        inline const uintptr_t luaS_newlstr = REBASE(0x2b32236);
        inline const uintptr_t lua_clock = REBASE(0x2b31662);
        inline const uintptr_t lua_pushstring = REBASE(0x2b18a81);
        inline const uintptr_t lua_setfield = REBASE(0x2b197a4);
    }

    namespace Roblox {
        inline const uintptr_t ConnectionDisconnect = REBASE(0x151b92e);
        inline const uintptr_t GetGlobalState = REBASE(0x441cf18);
        inline const uintptr_t GetIdentityContext = REBASE(0x15b3d7e);
        inline const uintptr_t IdentityCapabilitiesTable = REBASE(0x64b04d8);
        inline const uintptr_t IdentityToCaps = REBASE(0x178b1b0);
        inline const uintptr_t IsLegalSendEvent = REBASE(0x56f9642);
        inline const uintptr_t PrintIdentity = REBASE(0x153a89c);
        inline const uintptr_t PushInstance = REBASE(0x150b632);
        inline const uintptr_t ScriptContextRequire = REBASE(0x16008ca);
        inline const uintptr_t ScriptContextResume = REBASE(0x15c901a);
        inline const uintptr_t ScriptContextResumeInternal = REBASE(0x15c994a);
    }

    namespace FFlag {
        inline const uintptr_t BooleanType = REBASE(0x75f0080);
        inline const uintptr_t FLogDataBank = REBASE(0x7de8a80);
        inline const uintptr_t GetFFlag = REBASE(0x3019a8e);
        inline const uintptr_t GetFFlagByName = REBASE(0x302aabc);
        inline const uintptr_t GetFFlagLocked = REBASE(0x301db5e);
        inline const uintptr_t RegisterBoolean = REBASE(0x302a31b);
        inline const uintptr_t SetFFlag = REBASE(0x30190bc);
    }

    namespace FlagStorage {
        inline const uintptr_t AnimGraphServerAuthorityNodeIndices = REBASE(0x7b5fbc0);
        inline const uintptr_t DebugMacDisableUpdates = REBASE(0x78157a8);
        inline const uintptr_t EnableLoadModule = REBASE(0x7826768);
        inline const uintptr_t RenderAdvSkyGroup = REBASE(0x7b8bb28);
    }

    namespace Scheduler {
        inline const uintptr_t TaskSchedulerConstructor = REBASE(0x2fb19d2);
        inline const uintptr_t TaskSchedulerPTR = REBASE(0x7de0e90);
        inline const uintptr_t TaskSchedulerTargetFPS = REBASE(0x7b081a0);
    }

    namespace Reflection {
        inline const uintptr_t GetProperty = REBASE(0x5223f58);
        inline const uintptr_t GetPropertyData = REBASE(0x17824b8);
        inline const uintptr_t GetPropertyEntry = REBASE(0x5223f18);
    }

    namespace Instance {
        inline const uintptr_t InstanceFindFirstAncestor = REBASE(0x11feeb0);
        inline const uintptr_t InstanceFindFirstAncestorWhichIsA = REBASE(0x11fefc4);
        inline const uintptr_t InstanceFindFirstChild = REBASE(0x11ff366);
        inline const uintptr_t InstanceFindFirstChildOfClass = REBASE(0x2e62b5a);
        inline const uintptr_t InstanceGetAttribute = REBASE(0x2e6b492);
        inline const uintptr_t InstanceGetAttributes = REBASE(0x1207c3c);
        inline const uintptr_t InstanceGetChildren = REBASE(0x11ff130);
        inline const uintptr_t InstanceGetDescendantsLua = REBASE(0x1207c54);
        inline const uintptr_t InstanceIndex = REBASE(0x1509446);
        inline const uintptr_t InstanceIsALua = REBASE(0x5225db4);
        inline const uintptr_t InstanceNew = REBASE(0x150634e);
    }

    namespace Remote {
        inline const uintptr_t FireAllClients = REBASE(0x46b32d6);
        inline const uintptr_t FireClient = REBASE(0x46b2fc2);
        inline const uintptr_t FireServer = REBASE(0x46b2c64);
        inline const uintptr_t InvokeClient = REBASE(0x477fa02);
        inline const uintptr_t InvokeServer = REBASE(0x477ebe6);
    }

    namespace WorldRoot {
        inline const uintptr_t BlockCast = REBASE(0x11be1e6);
        inline const uintptr_t ShapeCast = REBASE(0x11bf422);
        inline const uintptr_t SphereCast = REBASE(0x11bde24);
        inline const uintptr_t WorldRootRaycast = REBASE(0x11bdac6);
    }

    namespace Raycast {
        inline constexpr uintptr_t FunctionPointerOffset = 0x88;
        inline const uintptr_t RaycastBoundDesc = REBASE(0x7d70900);
        inline const uintptr_t RaycastFn = REBASE(0x11bdac6);
    }

    namespace Rendering {
        inline const uintptr_t CubeTextures = REBASE(0x2b9f7ca);
        inline const uintptr_t DrawSkyEnv = REBASE(0x48e6e88);
        inline const uintptr_t FastClusterEntityVTable = REBASE(0x76d4040);
    }

    namespace Input {
        inline const uintptr_t FireLeftMouseClick = REBASE(0x4e7c238);
        inline const uintptr_t FireMouseHoverEnter = REBASE(0x4e7cfe0);
        inline const uintptr_t FireMouseHoverLeave = REBASE(0x4e7d0c6);
        inline const uintptr_t FireProximityPrompt = REBASE(0x44640bc);
        inline const uintptr_t FireRightMouseClick = REBASE(0x4e7c39a);
        inline const uintptr_t FireTouchInterest = REBASE(0x130d064);
    }

    namespace Raknet {
        inline const uintptr_t HandleConnectionState = REBASE(0x56e1ef4);
        inline const uintptr_t ProcessNetworkPacket = REBASE(0x2e385e0);
        inline const uintptr_t RakNetReliabilityLayerSend = REBASE(0x2e5335e);
        inline const uintptr_t RakNetSend = REBASE(0x2e2d9ba);
        inline const uintptr_t RakPeerProcessNetworkPacket = REBASE(0x2e3f664);
        inline const uintptr_t RakPeerReceive = REBASE(0x2e2e56e);
        inline const uintptr_t RakPeerVirtualTable = REBASE(0x75e6cd0);
        inline const uintptr_t ReportNetworkError = REBASE(0x56e1aa8);
        inline const uintptr_t Send = REBASE(0x2e3f664);
        inline const uintptr_t SendPacketsToSelf = REBASE(0x2e2ca26);
        inline const uintptr_t UpdateNetworkLoop = REBASE(0x2e2cc9a);
    }

    namespace RbxTransport {
        inline const uintptr_t AppClose = REBASE(0x248f764);
        inline const uintptr_t AppDeletingDestructor = REBASE(0x248f2a8);
        inline const uintptr_t AppDestructor = REBASE(0x248f29e);
        inline const uintptr_t AppReceive = REBASE(0x24909de);
        inline const uintptr_t AppReceiveMaxBytes = REBASE(0x2491178);
        inline const uintptr_t AppSend = REBASE(0x2490236);
        inline const uintptr_t AppTypeInfo = REBASE(0x759ebe8);
        inline const uintptr_t AppVTable = REBASE(0x759eaf0);
        inline const uintptr_t ClientChannelConfig = REBASE(0x4d25eae);
        inline const uintptr_t ClientDeletingDestructor = REBASE(0x4d26f8a);
        inline const uintptr_t ClientDestructor = REBASE(0x4d26f32);
        inline const uintptr_t ClientOpenChannels = REBASE(0x4d262d2);
        inline const uintptr_t ClientReceive = REBASE(0x4d2619c);
        inline const uintptr_t ClientSend = REBASE(0x4d25cf4);
        inline const uintptr_t ClientSendShared = REBASE(0x4d25ed8);
        inline const uintptr_t ClientTypeInfo = REBASE(0x76ddd10);
        inline const uintptr_t ClientVTable = REBASE(0x76ddc28);
        inline const uintptr_t FailureFallbackToRbxTransport = REBASE(0x7bae378);
        inline const uintptr_t GenerateRetryToken = REBASE(0x258af3f);
        inline const uintptr_t RetryTokenDecrypt = REBASE(0x2589200);
        inline const uintptr_t RetryTokenEncrypt = REBASE(0x25890bc);
        inline const uintptr_t StreamAppend = REBASE(0x177724a);
        inline const uintptr_t StreamBufferDestroy = REBASE(0x587825c);
        inline const uintptr_t StreamCreate = REBASE(0x1776ede);
        inline const uintptr_t UseRbxTransportClient = REBASE(0x7b983d8);
        inline const uintptr_t VerifyRetryToken = REBASE(0x258b2bd);
    }

    namespace Task {
        inline const uintptr_t cancel = REBASE(0x15426fe);
        inline const uintptr_t defer = REBASE(0x1541dee);
        inline const uintptr_t delay = REBASE(0x15426fe);
        inline const uintptr_t desynchronize = REBASE(0x1541cf0);
        inline const uintptr_t spawn = REBASE(0x1542de9);
        inline const uintptr_t synchronize = REBASE(0x1541bf2);
        inline const uintptr_t task_lib_init = REBASE(0x15419d4);
        inline const uintptr_t wait = REBASE(0x15425a6);
    }

    namespace MathLib {
        inline const uintptr_t lua_math_abs = REBASE(0x2b2e17e);
        inline const uintptr_t lua_math_acos = REBASE(0x2b2e1ac);
        inline const uintptr_t lua_math_asin = REBASE(0x2b2e1d8);
        inline const uintptr_t lua_math_atan = REBASE(0x2b2e24a);
        inline const uintptr_t lua_math_atan2 = REBASE(0x2b2e204);
        inline const uintptr_t lua_math_ceil = REBASE(0x2b2e276);
        inline const uintptr_t lua_math_clamp = REBASE(0x2b2eeb3);
        inline const uintptr_t lua_math_cos = REBASE(0x2b2e2cf);
        inline const uintptr_t lua_math_cosh = REBASE(0x2b2e2a3);
        inline const uintptr_t lua_math_deg = REBASE(0x2b2e2fb);
        inline const uintptr_t lua_math_exp = REBASE(0x2b2e32a);
        inline const uintptr_t lua_math_floor = REBASE(0x2b2e356);
        inline const uintptr_t lua_math_fmod = REBASE(0x2b2e383);
        inline const uintptr_t lua_math_frexp = REBASE(0x2b2e3c9);
        inline const uintptr_t lua_math_isfinite = REBASE(0x2b2f169);
        inline const uintptr_t lua_math_isinf = REBASE(0x2b2f125);
        inline const uintptr_t lua_math_isnan = REBASE(0x2b2f0f4);
        inline const uintptr_t lua_math_ldexp = REBASE(0x2b2e40e);
        inline const uintptr_t lua_math_lerp = REBASE(0x2b2f077);
        inline const uintptr_t lua_math_log = REBASE(0x2b2e47f);
        inline const uintptr_t lua_math_log10 = REBASE(0x2b2e453);
        inline const uintptr_t lua_math_map = REBASE(0x2b2efd9);
        inline const uintptr_t lua_math_max = REBASE(0x2b2e532);
        inline const uintptr_t lua_math_min = REBASE(0x2b2e5b1);
        inline const uintptr_t lua_math_modf = REBASE(0x2b2e630);
        inline const uintptr_t lua_math_noise = REBASE(0x2b2ea38);
        inline const uintptr_t lua_math_pow = REBASE(0x2b2e681);
        inline const uintptr_t lua_math_rad = REBASE(0x2b2e6c7);
        inline const uintptr_t lua_math_random = REBASE(0x2b2e6f6);
        inline const uintptr_t lua_math_randomseed = REBASE(0x2b2e919);
        inline const uintptr_t lua_math_round = REBASE(0x2b2ef91);
        inline const uintptr_t lua_math_sign = REBASE(0x2b2ef38);
        inline const uintptr_t lua_math_sin = REBASE(0x2b2e989);
        inline const uintptr_t lua_math_sinh = REBASE(0x2b2e95d);
        inline const uintptr_t lua_math_sqrt = REBASE(0x2b2e9b5);
        inline const uintptr_t lua_math_tan = REBASE(0x2b2ea0c);
        inline const uintptr_t lua_math_tanh = REBASE(0x2b2e9e0);
    }

    namespace OsLib {
        inline const uintptr_t lua_os_clock = REBASE(0x2b30fb9);
        inline const uintptr_t lua_os_date = REBASE(0x2b30fdb);
        inline const uintptr_t lua_os_difftime = REBASE(0x2b3133d);
        inline const uintptr_t lua_os_time = REBASE(0x2b31386);
    }

    namespace StringLib {
        inline const uintptr_t lua_string_byte = REBASE(0x2b324d9);
        inline const uintptr_t lua_string_char = REBASE(0x2b325f3);
        inline const uintptr_t lua_string_find = REBASE(0x2b326c7);
        inline const uintptr_t lua_string_format = REBASE(0x2b326d6);
        inline const uintptr_t lua_string_gmatch = REBASE(0x2b32ee0);
        inline const uintptr_t lua_string_gsub = REBASE(0x2b32f40);
        inline const uintptr_t lua_string_len = REBASE(0x2b33433);
        inline const uintptr_t lua_string_lower = REBASE(0x2b3346b);
        inline const uintptr_t lua_string_match = REBASE(0x2b33516);
        inline const uintptr_t lua_string_pack = REBASE(0x2b33be7);
        inline const uintptr_t lua_string_packsize = REBASE(0x2b3416c);
        inline const uintptr_t lua_string_rep = REBASE(0x2b33522);
        inline const uintptr_t lua_string_reverse = REBASE(0x2b3367a);
        inline const uintptr_t lua_string_split = REBASE(0x2b3388e);
        inline const uintptr_t lua_string_sub = REBASE(0x2b33726);
        inline const uintptr_t lua_string_unpack = REBASE(0x2b3425f);
        inline const uintptr_t lua_string_upper = REBASE(0x2b337e3);
    }

    namespace TableLib {
        inline const uintptr_t lua_table_clear = REBASE(0x2b3996f);
        inline const uintptr_t lua_table_clone = REBASE(0x2b39a88);
        inline const uintptr_t lua_table_concat = REBASE(0x2b38f07);
        inline const uintptr_t lua_table_create = REBASE(0x2b39719);
        inline const uintptr_t lua_table_find = REBASE(0x2b397be);
        inline const uintptr_t lua_table_foreach = REBASE(0x2b39046);
        inline const uintptr_t lua_table_foreachi = REBASE(0x2b39101);
        inline const uintptr_t lua_table_freeze = REBASE(0x2b399aa);
        inline const uintptr_t lua_table_getn = REBASE(0x2b391cd);
        inline const uintptr_t lua_table_insert = REBASE(0x2b392c7);
        inline const uintptr_t lua_table_isfrozen = REBASE(0x2b39a4d);
        inline const uintptr_t lua_table_maxn = REBASE(0x2b39208);
        inline const uintptr_t lua_table_move = REBASE(0x2b39561);
        inline const uintptr_t lua_table_pack = REBASE(0x2b394cd);
        inline const uintptr_t lua_table_remove = REBASE(0x2b39385);
        inline const uintptr_t lua_table_sort = REBASE(0x2b39427);
        inline const uintptr_t lua_table_unpack = REBASE(0x2b38dd5);
    }

    namespace IntegerLib {
        inline const uintptr_t lua_integer_add = REBASE(0x2b2d1a9);
        inline const uintptr_t lua_integer_arshift = REBASE(0x2b2db25);
        inline const uintptr_t lua_integer_band = REBASE(0x2b2d70d);
        inline const uintptr_t lua_integer_bnot = REBASE(0x2b2d7df);
        inline const uintptr_t lua_integer_bor = REBASE(0x2b2d77a);
        inline const uintptr_t lua_integer_bswap = REBASE(0x2b2df09);
        inline const uintptr_t lua_integer_btest = REBASE(0x2b2de26);
        inline const uintptr_t lua_integer_bxor = REBASE(0x2b2d80c);
        inline const uintptr_t lua_integer_clamp = REBASE(0x2b2d685);
        inline const uintptr_t lua_integer_countlz = REBASE(0x2b2dece);
        inline const uintptr_t lua_integer_countrz = REBASE(0x2b2de97);
        inline const uintptr_t lua_integer_create = REBASE(0x2b2d0f2);
        inline const uintptr_t lua_integer_div = REBASE(0x2b2d25e);
        inline const uintptr_t lua_integer_extract = REBASE(0x2b2dc30);
        inline const uintptr_t lua_integer_fromstring = REBASE(0x2b2df36);
        inline const uintptr_t lua_integer_ge = REBASE(0x2b2d9ac);
        inline const uintptr_t lua_integer_gt = REBASE(0x2b2d96d);
        inline const uintptr_t lua_integer_idiv = REBASE(0x2b2d462);
        inline const uintptr_t lua_integer_le = REBASE(0x2b2d8b0);
        inline const uintptr_t lua_integer_lrotate = REBASE(0x2b2db8e);
        inline const uintptr_t lua_integer_lshift = REBASE(0x2b2da69);
        inline const uintptr_t lua_integer_lt = REBASE(0x2b2d871);
        inline const uintptr_t lua_integer_max = REBASE(0x2b2d368);
        inline const uintptr_t lua_integer_min = REBASE(0x2b2d2f6);
        inline const uintptr_t lua_integer_mod = REBASE(0x2b2d5eb);
        inline const uintptr_t lua_integer_mul = REBASE(0x2b2d221);
        inline const uintptr_t lua_integer_neg = REBASE(0x2b2d17c);
        inline const uintptr_t lua_integer_rem = REBASE(0x2b2d3da);
        inline const uintptr_t lua_integer_replace = REBASE(0x2b2dd14);
        inline const uintptr_t lua_integer_rrotate = REBASE(0x2b2dbdf);
        inline const uintptr_t lua_integer_rshift = REBASE(0x2b2dac7);
        inline const uintptr_t lua_integer_sub = REBASE(0x2b2d1e5);
        inline const uintptr_t lua_integer_tonumber = REBASE(0x2b2d150);
        inline const uintptr_t lua_integer_udiv = REBASE(0x2b2d509);
        inline const uintptr_t lua_integer_uge = REBASE(0x2b2da2a);
        inline const uintptr_t lua_integer_ugt = REBASE(0x2b2d9eb);
        inline const uintptr_t lua_integer_ule = REBASE(0x2b2d92e);
        inline const uintptr_t lua_integer_ult = REBASE(0x2b2d8ef);
        inline const uintptr_t lua_integer_urem = REBASE(0x2b2d57a);
    }

    namespace Bit32Lib {
        inline const uintptr_t lua_bit32_arshift = REBASE(0x2b1e0e5);
        inline const uintptr_t lua_bit32_band = REBASE(0x2b1e16f);
        inline const uintptr_t lua_bit32_bnot = REBASE(0x2b1e193);
        inline const uintptr_t lua_bit32_bor = REBASE(0x2b1e1be);
        inline const uintptr_t lua_bit32_btest = REBASE(0x2b1e288);
        inline const uintptr_t lua_bit32_bxor = REBASE(0x2b1e223);
        inline const uintptr_t lua_bit32_byteswap = REBASE(0x2b1e54c);
        inline const uintptr_t lua_bit32_countlz = REBASE(0x2b1e4cf);
        inline const uintptr_t lua_bit32_countrz = REBASE(0x2b1e512);
        inline const uintptr_t lua_bit32_extract = REBASE(0x2b1e2b2);
        inline const uintptr_t lua_bit32_lrotate = REBASE(0x2b1e30e);
        inline const uintptr_t lua_bit32_lshift = REBASE(0x2b1e352);
        inline const uintptr_t lua_bit32_replace = REBASE(0x2b1e3ad);
        inline const uintptr_t lua_bit32_rrotate = REBASE(0x2b1e42e);
        inline const uintptr_t lua_bit32_rshift = REBASE(0x2b1e475);
    }

    namespace BufferLib {
        inline const uintptr_t lua_buffer_copy = REBASE(0x2b1f353);
        inline const uintptr_t lua_buffer_create = REBASE(0x2b1e7e8);
        inline const uintptr_t lua_buffer_fill = REBASE(0x2b1f45c);
        inline const uintptr_t lua_buffer_fromstring = REBASE(0x2b1e838);
        inline const uintptr_t lua_buffer_len = REBASE(0x2b1f30f);
        inline const uintptr_t lua_buffer_readbits = REBASE(0x2b1f528);
        inline const uintptr_t lua_buffer_readf32 = REBASE(0x2b1ebe4);
        inline const uintptr_t lua_buffer_readf64 = REBASE(0x2b1ec67);
        inline const uintptr_t lua_buffer_readi16 = REBASE(0x2b1e9cf);
        inline const uintptr_t lua_buffer_readi32 = REBASE(0x2b1eadb);
        inline const uintptr_t lua_buffer_readi8 = REBASE(0x2b1e8cb);
        inline const uintptr_t lua_buffer_readstring = REBASE(0x2b1f15c);
        inline const uintptr_t lua_buffer_readu16 = REBASE(0x2b1ea55);
        inline const uintptr_t lua_buffer_readu32 = REBASE(0x2b1eb5e);
        inline const uintptr_t lua_buffer_readu8 = REBASE(0x2b1e94d);
        inline const uintptr_t lua_buffer_tostring = REBASE(0x2b1e889);
        inline const uintptr_t lua_buffer_writebits = REBASE(0x2b1f64d);
        inline const uintptr_t lua_buffer_writef32 = REBASE(0x2b1f038);
        inline const uintptr_t lua_buffer_writef64 = REBASE(0x2b1f0cc);
        inline const uintptr_t lua_buffer_writei16 = REBASE(0x2b1edfe);
        inline const uintptr_t lua_buffer_writei32 = REBASE(0x2b1ef1c);
        inline const uintptr_t lua_buffer_writei8 = REBASE(0x2b1ecea);
        inline const uintptr_t lua_buffer_writestring = REBASE(0x2b1f217);
        inline const uintptr_t lua_buffer_writeu16 = REBASE(0x2b1ee8d);
        inline const uintptr_t lua_buffer_writeu32 = REBASE(0x2b1efaa);
        inline const uintptr_t lua_buffer_writeu8 = REBASE(0x2b1ed74);
    }

    namespace VectorLib {
        inline const uintptr_t lua_vector_abs = REBASE(0x2b3b6e0);
        inline const uintptr_t lua_vector_angle = REBASE(0x2b3b546);
        inline const uintptr_t lua_vector_ceil = REBASE(0x2b3b6a5);
        inline const uintptr_t lua_vector_clamp = REBASE(0x2b3b7c1);
        inline const uintptr_t lua_vector_create = REBASE(0x2b3b32c);
        inline const uintptr_t lua_vector_cross = REBASE(0x2b3b467);
        inline const uintptr_t lua_vector_dot = REBASE(0x2b3b4e6);
        inline const uintptr_t lua_vector_floor = REBASE(0x2b3b66a);
        inline const uintptr_t lua_vector_lerp = REBASE(0x2b3b9ca);
        inline const uintptr_t lua_vector_magnitude = REBASE(0x2b3b3ad);
        inline const uintptr_t lua_vector_max = REBASE(0x2b3b8a6);
        inline const uintptr_t lua_vector_min = REBASE(0x2b3b938);
        inline const uintptr_t lua_vector_normalize = REBASE(0x2b3b3fb);
        inline const uintptr_t lua_vector_sign = REBASE(0x2b3b725);
    }

    namespace Utf8Lib {
        inline const uintptr_t lua_utf8_char = REBASE(0x2b3ad3b);
        inline const uintptr_t lua_utf8_codepoint = REBASE(0x2b3abcd);
        inline const uintptr_t lua_utf8_codes = REBASE(0x2b3af87);
        inline const uintptr_t lua_utf8_len = REBASE(0x2b3ae32);
        inline const uintptr_t lua_utf8_offset = REBASE(0x2b3aa35);
    }

    namespace DebugLib {
        inline const uintptr_t lua_debug_info = REBASE(0x2b2523b);
        inline const uintptr_t lua_debug_traceback = REBASE(0x2b2551c);
    }

    namespace CoroutineLib {
        inline const uintptr_t lua_coroutine_close = REBASE(0x2b24cfa);
        inline const uintptr_t lua_coroutine_create = REBASE(0x2b2445c);
        inline const uintptr_t lua_coroutine_finally = REBASE(0x2b24596);
        inline const uintptr_t lua_coroutine_isyieldable = REBASE(0x2b24572);
        inline const uintptr_t lua_coroutine_resume = REBASE(0x2b23f32);
        inline const uintptr_t lua_coroutine_running = REBASE(0x2b24498);
        inline const uintptr_t lua_coroutine_status = REBASE(0x2b244be);
        inline const uintptr_t lua_coroutine_wrap = REBASE(0x2b24525);
        inline const uintptr_t lua_coroutine_yield = REBASE(0x2b2455c);
    }

    namespace ClassLib {
        inline const uintptr_t lua_class_classof = REBASE(0x2b23e2b);
        inline const uintptr_t lua_class_isinstance = REBASE(0x2b23dc0);
    }

    namespace LuauAliases {
        inline const uintptr_t luaB_pcallcont = REBASE(0x2b1d546);
        inline const uintptr_t luaB_pcally = REBASE(0x2b1d513);
        inline const uintptr_t luaL_tolstring = REBASE(0x2b1cfe4);
        inline const uintptr_t luaM_free_ = REBASE(0x2b2f9aa);
        inline const uintptr_t luaM_freegco_ = REBASE(0x2b2fa9b);
        inline const uintptr_t luaM_newgco_ = REBASE(0x2b2f4b8);
        inline const uintptr_t luaO_nilobject_ = REBASE(0x6673b50);
        inline const uintptr_t luaV_equalval = REBASE(0x2b3e6db);
        inline const uintptr_t luau_load = REBASE(0x2b3bc62);
    }

    namespace LuauApi {
        inline const uintptr_t lua_absindex = REBASE(0x2b173e1);
        inline const uintptr_t lua_addfinalizer = REBASE(0x2b1a2d7);
        inline const uintptr_t lua_call = REBASE(0x2b19cc7);
        inline const uintptr_t lua_callbacks = REBASE(0x2b1ba6b);
        inline const uintptr_t lua_callyieldable = REBASE(0x2b19f30);
        inline const uintptr_t lua_cleartable = REBASE(0x2b1b91f);
        inline const uintptr_t lua_clonefunction = REBASE(0x2b1b79b);
        inline const uintptr_t lua_clonetable = REBASE(0x2b1b987);
        inline const uintptr_t lua_close = REBASE(0x2b31f30);
        inline const uintptr_t lua_concat = REBASE(0x2b1a8d5);
        inline const uintptr_t lua_costatus = REBASE(0x2b1a07c);
        inline const uintptr_t lua_cpcall = REBASE(0x2b19e7e);
        inline const uintptr_t lua_createtable = REBASE(0x2b1937c);
        inline const uintptr_t lua_equal = REBASE(0x2b17b92);
        inline const uintptr_t lua_gc = REBASE(0x2b1a467);
        inline const uintptr_t lua_getcoverage = REBASE(0x2b2645d);
        inline const uintptr_t lua_getfenv = REBASE(0x2b1964a);
        inline const uintptr_t lua_getfield = REBASE(0x2b18f2f);
        inline const uintptr_t lua_getinfo = REBASE(0x2b2584c);
        inline const uintptr_t lua_getlocal = REBASE(0x2b256df);
        inline const uintptr_t lua_getmetatable = REBASE(0x2b19535);
        inline const uintptr_t lua_getreadonly = REBASE(0x2b19490);
        inline const uintptr_t lua_gettable = REBASE(0x2b18e9d);
        inline const uintptr_t lua_getthreaddata = REBASE(0x2b1a453);
        inline const uintptr_t lua_getupvalue = REBASE(0x2b1ac94);
        inline const uintptr_t lua_getuserdatametatable = REBASE(0x2b1b514);
        inline const uintptr_t lua_getweakref = REBASE(0x2b1b464);
        inline const uintptr_t lua_insert = REBASE(0x2b1764e);
        inline const uintptr_t lua_isLfunction = REBASE(0x2b17978);
        inline const uintptr_t lua_iscfunction = REBASE(0x2b1791c);
        inline const uintptr_t lua_isnumber = REBASE(0x2b179d4);
        inline const uintptr_t lua_isstring = REBASE(0x2b17a4a);
        inline const uintptr_t lua_isuserdata = REBASE(0x2b17a66);
        inline const uintptr_t lua_isyieldable = REBASE(0x2b27b76);
        inline const uintptr_t lua_mainthread = REBASE(0x2b173d0);
        inline const uintptr_t lua_newbuffer = REBASE(0x2b1abf5);
        inline const uintptr_t lua_newthread = REBASE(0x2b1731d);
        inline const uintptr_t lua_newuserdatadtor = REBASE(0x2b1ab2e);
        inline const uintptr_t lua_newuserdatatagged = REBASE(0x2b1a9bb);
        inline const uintptr_t lua_next = REBASE(0x2b1a692);
        inline const uintptr_t lua_objlen = REBASE(0x2b18300);
        inline const uintptr_t lua_pcall = REBASE(0x2b19d5a);
        inline const uintptr_t lua_pcallyieldable = REBASE(0x2b19f7a);
        inline const uintptr_t lua_pushboolean = REBASE(0x2b18d41);
        inline const uintptr_t lua_pushcclosurek = REBASE(0x2b18bff);
        inline const uintptr_t lua_pushinteger = REBASE(0x2b18814);
        inline const uintptr_t lua_pushinteger64 = REBASE(0x2b1887b);
        inline const uintptr_t lua_pushlightuserdatatagged = REBASE(0x2b18da9);
        inline const uintptr_t lua_pushlstring = REBASE(0x2b189d4);
        inline const uintptr_t lua_pushnil = REBASE(0x2b1874b);
        inline const uintptr_t lua_pushnumber = REBASE(0x2b187aa);
        inline const uintptr_t lua_pushthread = REBASE(0x2b18e1a);
        inline const uintptr_t lua_pushunsigned = REBASE(0x2b188dc);
        inline const uintptr_t lua_pushvalue = REBASE(0x2b177e0);
        inline const uintptr_t lua_pushvector = REBASE(0x2b18946);
        inline const uintptr_t lua_rawequal = REBASE(0x2b17ac0);
        inline const uintptr_t lua_rawget = REBASE(0x2b1912a);
        inline const uintptr_t lua_rawgetfield = REBASE(0x2b19032);
        inline const uintptr_t lua_rawgeti = REBASE(0x2b191c1);
        inline const uintptr_t lua_rawgetptagged = REBASE(0x2b1929c);
        inline const uintptr_t lua_rawiter = REBASE(0x2b1a773);
        inline const uintptr_t lua_rawset = REBASE(0x2b1991f);
        inline const uintptr_t lua_rawsetfield = REBASE(0x2b19844);
        inline const uintptr_t lua_rawseti = REBASE(0x2b199d5);
        inline const uintptr_t lua_rawsetptagged = REBASE(0x2b19a8d);
        inline const uintptr_t lua_ref = REBASE(0x2b1b12c);
        inline const uintptr_t lua_remove = REBASE(0x2b175cf);
        inline const uintptr_t lua_replace = REBASE(0x2b176e9);
        inline const uintptr_t lua_resume = REBASE(0x2b2762f);
        inline const uintptr_t lua_resumeerror = REBASE(0x2b2791b);
        inline const uintptr_t lua_setembeddergc = REBASE(0x2b1b321);
        inline const uintptr_t lua_setfenv = REBASE(0x2b19c27);
        inline const uintptr_t lua_setlocal = REBASE(0x2b2578e);
        inline const uintptr_t lua_setmetatable = REBASE(0x2b19b55);
        inline const uintptr_t lua_setreadonly = REBASE(0x2b19429);
        inline const uintptr_t lua_setsafeenv = REBASE(0x2b194dd);
        inline const uintptr_t lua_settable = REBASE(0x2b1972f);
        inline const uintptr_t lua_setthreaddata = REBASE(0x2b1a45d);
        inline const uintptr_t lua_settop = REBASE(0x2b17416);
        inline const uintptr_t lua_setupvalue = REBASE(0x2b1adfe);
        inline const uintptr_t lua_status = REBASE(0x2b1a072);
        inline const uintptr_t lua_toboolean = REBASE(0x2b17edd);
        inline const uintptr_t lua_tobuffer = REBASE(0x2b18677);
        inline const uintptr_t lua_tocfunction = REBASE(0x2b18394);
        inline const uintptr_t lua_tointeger64 = REBASE(0x2b17f40);
        inline const uintptr_t lua_tointegerx = REBASE(0x2b17dcd);
        inline const uintptr_t lua_tolightuserdata = REBASE(0x2b183f1);
        inline const uintptr_t lua_tolightuserdatatagged = REBASE(0x2b18444);
        inline const uintptr_t lua_tolstring = REBASE(0x2b17fb3);
        inline const uintptr_t lua_tonumberx = REBASE(0x2b17d44);
        inline const uintptr_t lua_topointer = REBASE(0x2b186e3);
        inline const uintptr_t lua_tothread = REBASE(0x2b18624);
        inline const uintptr_t lua_tounsignedx = REBASE(0x2b17e50);
        inline const uintptr_t lua_touserdata = REBASE(0x2b184a5);
        inline const uintptr_t lua_tovector = REBASE(0x2b182ad);
        inline const uintptr_t lua_typename = REBASE(0x2b178fa);
        inline const uintptr_t lua_unref = REBASE(0x2b1b22f);
        inline const uintptr_t lua_weakref = REBASE(0x2b1b332);
        inline const uintptr_t lua_xmove = REBASE(0x2b171ab);
        inline const uintptr_t lua_xpush = REBASE(0x2b17257);
    }

    namespace LuauAux {
        inline const uintptr_t luaL_addlstring = REBASE(0x2b1cb8c);
        inline const uintptr_t luaL_addvalue = REBASE(0x2b1cdce);
        inline const uintptr_t luaL_addvalueany = REBASE(0x2b1ce49);
        inline const uintptr_t luaL_buffinit = REBASE(0x2b1cb68);
        inline const uintptr_t luaL_buffinitsize = REBASE(0x2b1cc90);
        inline const uintptr_t luaL_callmeta = REBASE(0x2b1c647);
        inline const uintptr_t luaL_checkany = REBASE(0x2b1c2eb);
        inline const uintptr_t luaL_checkboolean = REBASE(0x2b1c3a9);
        inline const uintptr_t luaL_checkbuffer = REBASE(0x2b1c228);
        inline const uintptr_t luaL_checkinteger = REBASE(0x2b1c414);
        inline const uintptr_t luaL_checkinteger64 = REBASE(0x2b1c44f);
        inline const uintptr_t luaL_checknumber = REBASE(0x2b1c330);
        inline const uintptr_t luaL_checkstack = REBASE(0x2b1c272);
        inline const uintptr_t luaL_checkudata = REBASE(0x2b1c16c);
        inline const uintptr_t luaL_checkunsigned = REBASE(0x2b1c4f9);
        inline const uintptr_t luaL_checkvector = REBASE(0x2b1c571);
        inline const uintptr_t luaL_newmetatable = REBASE(0x2b1c0fd);
        inline const uintptr_t luaL_openlibs = REBASE(0x2b2cdb7);
        inline const uintptr_t luaL_optinteger64 = REBASE(0x2b1c4bc);
        inline const uintptr_t luaL_optlstring = REBASE(0x2b1c078);
        inline const uintptr_t luaL_optnumber = REBASE(0x2b1c36b);
        inline const uintptr_t luaL_optvector = REBASE(0x2b1c59b);
        inline const uintptr_t luaL_prepbuffsize = REBASE(0x2b1cbda);
        inline const uintptr_t luaL_pushresult = REBASE(0x2b1cbfa);
        inline const uintptr_t luaL_pushresultsize = REBASE(0x2b1d218);
        inline const uintptr_t luaL_sandbox = REBASE(0x2b2ce75);
        inline const uintptr_t luaL_sandboxthread = REBASE(0x2b2cf8a);
        inline const uintptr_t luaL_traceback = REBASE(0x2b1c931);
        inline const uintptr_t luaL_typename = REBASE(0x2b1c8ff);
        inline const uintptr_t luaL_where = REBASE(0x2b1bf1e);
    }

    namespace LuauBuiltins {
        inline const uintptr_t luauF_readfp_double = REBASE(0x2b215b2);
        inline const uintptr_t luauF_readfp_float = REBASE(0x2b21509);
        inline const uintptr_t luauF_readinteger_int16_t = REBASE(0x2b21305);
        inline const uintptr_t luauF_readinteger_int32_t = REBASE(0x2b21409);
        inline const uintptr_t luauF_readinteger_int8_t = REBASE(0x2b21213);
        inline const uintptr_t luauF_readinteger_uint16_t = REBASE(0x2b2135e);
        inline const uintptr_t luauF_readinteger_uint32_t = REBASE(0x2b2145f);
        inline const uintptr_t luauF_readinteger_uint8_t = REBASE(0x2b21266);
        inline const uintptr_t luauF_writefp_double = REBASE(0x2b21608);
        inline const uintptr_t luauF_writefp_float = REBASE(0x2b2155f);
        inline const uintptr_t luauF_writeinteger_uint16_t = REBASE(0x2b213b7);
        inline const uintptr_t luauF_writeinteger_uint32_t = REBASE(0x2b214b8);
        inline const uintptr_t luauF_writeinteger_uint8_t = REBASE(0x2b212b9);
    }

    namespace LuauData {
        inline const uintptr_t luaT_eventname = REBASE(0x75cabd0);
        inline const uintptr_t luaT_typenames = REBASE(0x75cab60);
        inline const uintptr_t luauF_table = REBASE(0x75c9880);
    }

    namespace LuauHelpers {
        inline const uintptr_t luau_lapi_aux_upvalue = REBASE(0x2b1ad72);
        inline const uintptr_t luau_lapi_f_Ccall = REBASE(0x2b19eb1);
        inline const uintptr_t luau_lapi_f_call = REBASE(0x2b19e6b);
        inline const uintptr_t luau_lapi_runfinalizercont = REBASE(0x2b1a2c3);
        inline const uintptr_t luau_lapi_runfinalizery = REBASE(0x2b1a19d);
        inline const uintptr_t luau_laux_currfuncname = REBASE(0x2b1bd14);
        inline const uintptr_t luau_laux_extendstrbuf = REBASE(0x2b1ccd5);
        inline const uintptr_t luau_lbaselib_auxopen = REBASE(0x2b1d433);
        inline const uintptr_t luau_lbaselib_getfunc = REBASE(0x2b1df43);
        inline const uintptr_t luau_lbitlib_andaux = REBASE(0x2b1e577);
        inline const uintptr_t luau_lbitlib_fieldargs = REBASE(0x2b1e5d5);
        inline const uintptr_t luau_lcorolib_auxresumecont = REBASE(0x2b24c69);
        inline const uintptr_t luau_lcorolib_coclosecont = REBASE(0x2b24419);
        inline const uintptr_t luau_lcorolib_coclosey = REBASE(0x2b242b8);
        inline const uintptr_t luau_lcorolib_coresumecont = REBASE(0x2b23fd2);
        inline const uintptr_t luau_lcorolib_coresumefinish = REBASE(0x2b251c4);
        inline const uintptr_t luau_ldblib_getthread = REBASE(0x2b255b1);
        inline const uintptr_t luau_ldebug_pusherror = REBASE(0x2b25f89);
        inline const uintptr_t luau_ldo_restore_stack_limit = REBASE(0x2b27d4d);
        inline const uintptr_t luau_ldo_resume = REBASE(0x2b27751);
        inline const uintptr_t luau_ldo_resume_finish = REBASE(0x2b2780b);
        inline const uintptr_t luau_ldo_resume_handle = REBASE(0x2b279b2);
        inline const uintptr_t luau_ldo_resume_start = REBASE(0x2b276b9);
        inline const uintptr_t luau_lgc_freeobj = REBASE(0x2b29b42);
        inline const uintptr_t luau_lgc_reallymarkobject = REBASE(0x2b29862);
        inline const uintptr_t luau_loslib_getfield = REBASE(0x2b315da);
        inline const uintptr_t luau_lstate_close_state = REBASE(0x2b31ec0);
        inline const uintptr_t luau_lstate_f_luaopen = REBASE(0x2b31d8d);
        inline const uintptr_t luau_lstrlib_add_value = REBASE(0x2b353f9);
        inline const uintptr_t luau_lstrlib_classend = REBASE(0x2b34e46);
        inline const uintptr_t luau_lstrlib_gmatch_aux = REBASE(0x2b352e8);
        inline const uintptr_t luau_lstrlib_match = REBASE(0x2b34912);
        inline const uintptr_t luau_lstrlib_push_captures = REBASE(0x2b34d6f);
        inline const uintptr_t luau_lstrlib_push_onecapture = REBASE(0x2b35254);
        inline const uintptr_t luau_lstrlib_str_find_aux = REBASE(0x2b34660);
        inline const uintptr_t luau_lstrlib_unpackint = REBASE(0x2b35e21);
        inline const uintptr_t luau_ltable_resize = REBASE(0x2b36501);
        inline const uintptr_t luau_ltable_setarrayvector = REBASE(0x2b36bad);
        inline const uintptr_t luau_ltablib_addfield = REBASE(0x2b39b0f);
        inline const uintptr_t luau_ltablib_moveelements = REBASE(0x2b39bbf);
        inline const uintptr_t luau_ltablib_sort_rec = REBASE(0x2b3a019);
        inline const uintptr_t luau_lutf8lib_buffutfchar = REBASE(0x2b3b07e);
        inline const uintptr_t luau_lutf8lib_iter_aux = REBASE(0x2b3b151);
        inline const uintptr_t luau_lutf8lib_utf8_decode = REBASE(0x2b3afd5);
        inline const uintptr_t luau_lvmutils_callTMres = REBASE(0x2b3de8a);
        inline const uintptr_t luau_lvmutils_get_compTM = REBASE(0x2b3e858);
    }

    namespace LuauInternals {
        inline const uintptr_t luaA_pushclass = REBASE(0x2b170ef);
        inline const uintptr_t luaB_inext = REBASE(0x2b1d278);
        inline const uintptr_t luaB_ipairs = REBASE(0x2b1d485);
        inline const uintptr_t luaB_newbuffer = REBASE(0x2b1e691);
        inline const uintptr_t luaB_pairs = REBASE(0x2b1d4cd);
        inline const uintptr_t luaB_xpcallcont = REBASE(0x2b1d64b);
        inline const uintptr_t luaB_xpcally = REBASE(0x2b1d5dc);
        inline const uintptr_t luaC_allocationrate = REBASE(0x2b29a0c);
        inline const uintptr_t luaC_barrierf = REBASE(0x2b29838);
        inline const uintptr_t luaC_barriertable = REBASE(0x2b2998c);
        inline const uintptr_t luaC_enumheap = REBASE(0x2b2b6c4);
        inline const uintptr_t luaD_call = REBASE(0x2b272d9);
        inline const uintptr_t luaD_growCI = REBASE(0x2b26f83);
        inline const uintptr_t luaD_growstack = REBASE(0x2b26f5d);
        inline const uintptr_t luaD_pcall = REBASE(0x2b27b9b);
        inline const uintptr_t luaD_preparefinalize = REBASE(0x2b27458);
        inline const uintptr_t luaD_preparefinalizestate = REBASE(0x2b273d8);
        inline const uintptr_t luaD_reallocCI = REBASE(0x2b26ee3);
        inline const uintptr_t luaD_reallocstack = REBASE(0x2b26c88);
        inline const uintptr_t luaD_runfinalizers = REBASE(0x2b274de);
        inline const uintptr_t luaD_seterrorobj = REBASE(0x2b27373);
        inline const uintptr_t luaE_newthread = REBASE(0x2b3171c);
        inline const uintptr_t luaF_close = REBASE(0x2b28434);
        inline const uintptr_t luaF_getlocal = REBASE(0x2b2866b);
        inline const uintptr_t luaG_aritherror = REBASE(0x2b25d43);
        inline const uintptr_t luaG_indexerror = REBASE(0x2b25e44);
        inline const uintptr_t luaG_missingmembererror = REBASE(0x2b25ec4);
        inline const uintptr_t luaG_ordererror = REBASE(0x2b25dce);
        inline const uintptr_t luaG_readonlyerror = REBASE(0x2b25f64);
        inline const uintptr_t luaG_typeerrorL = REBASE(0x2b25bb4);
        inline const uintptr_t luaH_clear = REBASE(0x2b382e9);
        inline const uintptr_t luaH_free = REBASE(0x2b36dfe);
        inline const uintptr_t luaH_get = REBASE(0x2b3703e);
        inline const uintptr_t luaH_getn = REBASE(0x2b38091);
        inline const uintptr_t luaH_getnum = REBASE(0x2b36e9a);
        inline const uintptr_t luaH_getreadonly = REBASE(0x2b386f6);
        inline const uintptr_t luaH_getstr = REBASE(0x2b36f6d);
        inline const uintptr_t luaH_newkey = REBASE(0x2b375d6);
        inline const uintptr_t luaH_set = REBASE(0x2b37584);
        inline const uintptr_t luaH_setnum = REBASE(0x2b37f68);
        inline const uintptr_t luaH_setreadonly = REBASE(0x2b3845b);
        inline const uintptr_t luaH_setstr = REBASE(0x2b37fd7);
        inline const uintptr_t luaM_realloc_ = REBASE(0x2b2fe54);
        inline const uintptr_t luaO_rawequalKey = REBASE(0x2b30b18);
        inline const uintptr_t luaO_rawequalObj = REBASE(0x2b30a74);
        inline const uintptr_t luaR_addclassmember = REBASE(0x2b233ad);
        inline const uintptr_t luaR_defaultcreateobject = REBASE(0x2b23824);
        inline const uintptr_t luaR_inheritclass = REBASE(0x2b22efc);
        inline const uintptr_t luaR_setupconstructor = REBASE(0x2b22d9a);
        inline const uintptr_t luaU_newudata = REBASE(0x2b3a8e6);
        inline const uintptr_t luaV_callTM = REBASE(0x2b3e2b4);
        inline const uintptr_t luaV_dolen = REBASE(0x2b3f745);
        inline const uintptr_t luaV_tonumber = REBASE(0x2b3dbb4);
        inline const uintptr_t luauF_abs = REBASE(0x2b1f8d9);
        inline const uintptr_t luauF_acos = REBASE(0x2b1f916);
        inline const uintptr_t luauF_arshift = REBASE(0x2b200f5);
        inline const uintptr_t luauF_asin = REBASE(0x2b1f95c);
        inline const uintptr_t luauF_assert = REBASE(0x2b1f8ac);
        inline const uintptr_t luauF_atan = REBASE(0x2b1f9f5);
        inline const uintptr_t luauF_atan2 = REBASE(0x2b1f9a2);
        inline const uintptr_t luauF_band = REBASE(0x2b20146);
        inline const uintptr_t luauF_bnot = REBASE(0x2b201c2);
        inline const uintptr_t luauF_bor = REBASE(0x2b20201);
        inline const uintptr_t luauF_btest = REBASE(0x2b202f9);
        inline const uintptr_t luauF_bufferreadlong = REBASE(0x2b22a1f);
        inline const uintptr_t luauF_bufferwritelong = REBASE(0x2b22a73);
        inline const uintptr_t luauF_bxor = REBASE(0x2b2027d);
        inline const uintptr_t luauF_byte = REBASE(0x2b20657);
        inline const uintptr_t luauF_byteswap = REBASE(0x2b211d4);
        inline const uintptr_t luauF_ceil = REBASE(0x2b1fa3b);
        inline const uintptr_t luauF_char = REBASE(0x2b2070d);
        inline const uintptr_t luauF_clamp = REBASE(0x2b208e7);
        inline const uintptr_t luauF_cos = REBASE(0x2b1faba);
        inline const uintptr_t luauF_cosh = REBASE(0x2b1fa74);
        inline const uintptr_t luauF_countlz = REBASE(0x2b20d4d);
        inline const uintptr_t luauF_countrz = REBASE(0x2b20d9a);
        inline const uintptr_t luauF_deg = REBASE(0x2b1fb00);
        inline const uintptr_t luauF_exp = REBASE(0x2b1fb3f);
        inline const uintptr_t luauF_extract = REBASE(0x2b20373);
        inline const uintptr_t luauF_extractk = REBASE(0x2b20ecb);
        inline const uintptr_t luauF_floor = REBASE(0x2b1fb85);
        inline const uintptr_t luauF_fmod = REBASE(0x2b1fbbe);
        inline const uintptr_t luauF_frexp = REBASE(0x2b1fc11);
        inline const uintptr_t luauF_getmetatable = REBASE(0x2b20f22);
        inline const uintptr_t luauF_integeradd = REBASE(0x2b21ecb);
        inline const uintptr_t luauF_integerarshift = REBASE(0x2b22740);
        inline const uintptr_t luauF_integerband = REBASE(0x2b22317);
        inline const uintptr_t luauF_integerbnot = REBASE(0x2b223d1);
        inline const uintptr_t luauF_integerbor = REBASE(0x2b22374);
        inline const uintptr_t luauF_integerbswap = REBASE(0x2b229e7);
        inline const uintptr_t luauF_integerbtest = REBASE(0x2b228f5);
        inline const uintptr_t luauF_integerbxor = REBASE(0x2b22409);
        inline const uintptr_t luauF_integerclamp = REBASE(0x2b222b9);
        inline const uintptr_t luauF_integercountlz = REBASE(0x2b2299e);
        inline const uintptr_t luauF_integercountrz = REBASE(0x2b22959);
        inline const uintptr_t luauF_integercreate = REBASE(0x2b21df6);
        inline const uintptr_t luauF_integerdiv = REBASE(0x2b21f8c);
        inline const uintptr_t luauF_integerextract = REBASE(0x2b2285e);
        inline const uintptr_t luauF_integerge = REBASE(0x2b225ba);
        inline const uintptr_t luauF_integergt = REBASE(0x2b22576);
        inline const uintptr_t luauF_integeridiv = REBASE(0x2b2213b);
        inline const uintptr_t luauF_integerle = REBASE(0x2b224aa);
        inline const uintptr_t luauF_integerlrotate = REBASE(0x2b227b0);
        inline const uintptr_t luauF_integerlshift = REBASE(0x2b22686);
        inline const uintptr_t luauF_integerlt = REBASE(0x2b22466);
        inline const uintptr_t luauF_integermax = REBASE(0x2b22063);
        inline const uintptr_t luauF_integermin = REBASE(0x2b21fec);
        inline const uintptr_t luauF_integermod = REBASE(0x2b22241);
        inline const uintptr_t luauF_integermul = REBASE(0x2b21f4b);
        inline const uintptr_t luauF_integerneg = REBASE(0x2b21e94);
        inline const uintptr_t luauF_integerrem = REBASE(0x2b220da);
        inline const uintptr_t luauF_integerrrotate = REBASE(0x2b22807);
        inline const uintptr_t luauF_integerrshift = REBASE(0x2b226e3);
        inline const uintptr_t luauF_integersub = REBASE(0x2b21f0b);
        inline const uintptr_t luauF_integertonumber = REBASE(0x2b21e5c);
        inline const uintptr_t luauF_integerudiv = REBASE(0x2b221ad);
        inline const uintptr_t luauF_integeruge = REBASE(0x2b22642);
        inline const uintptr_t luauF_integerugt = REBASE(0x2b225fe);
        inline const uintptr_t luauF_integerule = REBASE(0x2b22532);
        inline const uintptr_t luauF_integerult = REBASE(0x2b224ee);
        inline const uintptr_t luauF_integerurem = REBASE(0x2b221f7);
        inline const uintptr_t luauF_isfinite = REBASE(0x2b21da6);
        inline const uintptr_t luauF_isinf = REBASE(0x2b21d56);
        inline const uintptr_t luauF_isnan = REBASE(0x2b21d18);
        inline const uintptr_t luauF_ldexp = REBASE(0x2b1fc76);
        inline const uintptr_t luauF_len = REBASE(0x2b207d6);
        inline const uintptr_t luauF_lerp = REBASE(0x2b21be1);
        inline const uintptr_t luauF_log = REBASE(0x2b1fd0f);
        inline const uintptr_t luauF_log10 = REBASE(0x2b1fcc9);
        inline const uintptr_t luauF_lrotate = REBASE(0x2b2041a);
        inline const uintptr_t luauF_lshift = REBASE(0x2b20466);
        inline const uintptr_t luauF_max = REBASE(0x2b1fdb8);
        inline const uintptr_t luauF_min = REBASE(0x2b1fe35);
        inline const uintptr_t luauF_modf = REBASE(0x2b1feb2);
        inline const uintptr_t luauF_pow = REBASE(0x2b1ff14);
        inline const uintptr_t luauF_rad = REBASE(0x2b1ff67);
        inline const uintptr_t luauF_rawequal = REBASE(0x2b20b1b);
        inline const uintptr_t luauF_rawget = REBASE(0x2b20ad7);
        inline const uintptr_t luauF_rawlen = REBASE(0x2b20e6e);
        inline const uintptr_t luauF_rawset = REBASE(0x2b20a03);
        inline const uintptr_t luauF_replace = REBASE(0x2b204b7);
        inline const uintptr_t luauF_round = REBASE(0x2b209ab);
        inline const uintptr_t luauF_rrotate = REBASE(0x2b2057e);
        inline const uintptr_t luauF_rshift = REBASE(0x2b205ca);
        inline const uintptr_t luauF_select = REBASE(0x2b20de4);
        inline const uintptr_t luauF_setmetatable = REBASE(0x2b20fca);
        inline const uintptr_t luauF_sign = REBASE(0x2b20942);
        inline const uintptr_t luauF_sin = REBASE(0x2b1ffec);
        inline const uintptr_t luauF_sinh = REBASE(0x2b1ffa6);
        inline const uintptr_t luauF_sqrt = REBASE(0x2b20032);
        inline const uintptr_t luauF_sub = REBASE(0x2b2084f);
        inline const uintptr_t luauF_tan = REBASE(0x2b200af);
        inline const uintptr_t luauF_tanh = REBASE(0x2b20069);
        inline const uintptr_t luauF_tinsert = REBASE(0x2b20b5c);
        inline const uintptr_t luauF_tonumber = REBASE(0x2b21038);
        inline const uintptr_t luauF_tostring = REBASE(0x2b210a9);
        inline const uintptr_t luauF_tunpack = REBASE(0x2b20bde);
        inline const uintptr_t luauF_type = REBASE(0x2b2061b);
        inline const uintptr_t luauF_typeof = REBASE(0x2b20811);
        inline const uintptr_t luauF_vector = REBASE(0x2b20ce7);
        inline const uintptr_t luauF_vectorabs = REBASE(0x2b218e2);
        inline const uintptr_t luauF_vectorceil = REBASE(0x2b2188b);
        inline const uintptr_t luauF_vectorclamp = REBASE(0x2b219f7);
        inline const uintptr_t luauF_vectorcross = REBASE(0x2b2173f);
        inline const uintptr_t luauF_vectordot = REBASE(0x2b217ce);
        inline const uintptr_t luauF_vectorfloor = REBASE(0x2b21834);
        inline const uintptr_t luauF_vectorlerp = REBASE(0x2b21c4e);
        inline const uintptr_t luauF_vectormagnitude = REBASE(0x2b2165b);
        inline const uintptr_t luauF_vectormax = REBASE(0x2b21b44);
        inline const uintptr_t luauF_vectormin = REBASE(0x2b21aa7);
        inline const uintptr_t luauF_vectornormalize = REBASE(0x2b216b8);
        inline const uintptr_t luauF_vectorsign = REBASE(0x2b2193d);
        inline const uintptr_t luau_precall = REBASE(0x2b4ac85);
    }

    namespace ObjectFields {
        namespace AirProperties {
            inline constexpr uintptr_t AirDensity = 0x10;
            inline constexpr uintptr_t GlobalWind = 0x34;
        }
        namespace AnimationTrack {
            inline constexpr uintptr_t Animation = 0xa0;
            inline constexpr uintptr_t IsPlaying = 0xcc;
            inline constexpr uintptr_t Speed = 0xbc;
            inline constexpr uintptr_t TimePosition = 0xc0;
        }
        namespace Animator {
            inline constexpr uintptr_t ActiveAnimations = 0x8d0;
        }
        namespace Atmosphere {
            inline constexpr uintptr_t Color = 0xa0;
            inline constexpr uintptr_t Decay = 0xac;
            inline constexpr uintptr_t Density = 0xb8;
            inline constexpr uintptr_t Glare = 0xbc;
            inline constexpr uintptr_t Haze = 0xc0;
            inline constexpr uintptr_t Offset = 0xc4;
        }
        namespace Attachment {
            inline constexpr uintptr_t Position = 0xac;
        }
        namespace BasePart {
            inline constexpr uintptr_t CastShadow = 0x118;
            inline constexpr uintptr_t Locked = 0x119;
            inline constexpr uintptr_t Massless = 0x11a;
            inline constexpr uintptr_t Primitive = 0x168;
            inline constexpr uintptr_t Reflectance = 0xec;
            inline constexpr uintptr_t Shape = 0x190;
            inline constexpr uintptr_t Transparency = 0x110;
        }
        namespace Beam {
            inline constexpr uintptr_t Attachment0 = 0x140;
            inline constexpr uintptr_t Attachment1 = 0x150;
            inline constexpr uintptr_t Brightness = 0x160;
            inline constexpr uintptr_t CurveSize0 = 0x164;
            inline constexpr uintptr_t CurveSize1 = 0x168;
            inline constexpr uintptr_t LightEmission = 0x16c;
            inline constexpr uintptr_t LightInfluence = 0x170;
            inline constexpr uintptr_t Texture = 0x118;
            inline constexpr uintptr_t TextureLength = 0x17c;
            inline constexpr uintptr_t TextureSpeed = 0x184;
            inline constexpr uintptr_t Width0 = 0x188;
            inline constexpr uintptr_t Width1 = 0x18c;
            inline constexpr uintptr_t ZOffset = 0x190;
        }
        namespace BloomEffect {
            inline constexpr uintptr_t Enabled = 0x95;
            inline constexpr uintptr_t Intensity = 0x98;
            inline constexpr uintptr_t Size = 0x9c;
            inline constexpr uintptr_t Threshold = 0xa0;
        }
        namespace BlurEffect {
            inline constexpr uintptr_t Enabled = 0x95;
            inline constexpr uintptr_t Size = 0x98;
        }
        namespace ByteCode {
            inline constexpr uintptr_t Pointer = 0x20;
            inline constexpr uintptr_t Size = 0x18;
            inline constexpr uintptr_t IsLong = 0x10;
            inline constexpr uint64_t IsLong_Mask = 0x1;
            inline constexpr uintptr_t InlineData = 0x11;
            inline constexpr uintptr_t InlineSize = 0x10;
        }
        namespace CachedItem {
            inline constexpr uintptr_t FileMeshData = 0x20;
        }
        namespace Camera {
            inline constexpr uintptr_t CameraSubject = 0xa8;
            inline constexpr uintptr_t CameraType = 0x118;
            inline constexpr uintptr_t FieldOfView = 0x120;
        }
        namespace CharacterMesh {
            inline constexpr uintptr_t BodyPart = 0x118;
        }
        namespace ClickDetector {
            inline constexpr uintptr_t MaxActivationDistance = 0xc8;
            inline constexpr uintptr_t MouseIcon = 0xa0;
        }
        namespace Clothing {
            inline constexpr uintptr_t Color3 = 0xf8;
        }
        namespace ColorCorrectionEffect {
            inline constexpr uintptr_t Brightness = 0xa4;
            inline constexpr uintptr_t Contrast = 0xa8;
            inline constexpr uintptr_t Enabled = 0x95;
            inline constexpr uintptr_t TintColor = 0x98;
        }
        namespace ColorGradingEffect {
            inline constexpr uintptr_t Enabled = 0x95;
            inline constexpr uintptr_t TonemapperPreset = 0x98;
        }
        namespace DataModel {
             
            inline constexpr uintptr_t InstanceOffset = 0x1f0;
            inline constexpr uintptr_t CreatorId = 0x350;
            inline constexpr uintptr_t GameId = 0x358;
            inline constexpr uintptr_t GameLoaded = 0x5d0;
            inline constexpr uintptr_t JobId = 0x2f8;
            inline constexpr uintptr_t PlaceId = 0x360;
            inline constexpr uintptr_t PlaceVersion = 0x37c;
            inline constexpr uintptr_t Workspace = 0x328;
        }
        namespace DepthOfFieldEffect {
            inline constexpr uintptr_t Enabled = 0x95;
            inline constexpr uintptr_t FarIntensity = 0x98;
            inline constexpr uintptr_t FocusDistance = 0x9c;
            inline constexpr uintptr_t InFocusRadius = 0xa0;
            inline constexpr uintptr_t NearIntensity = 0xa4;
        }
        namespace DragDetector {
            inline constexpr uintptr_t ActivatedCursorIcon = 0x180;
            inline constexpr uintptr_t CursorIcon = 0xa0;
            inline constexpr uintptr_t MaxActivationDistance = 0xc8;
            inline constexpr uintptr_t MaxDragAngle = 0x270;
            inline constexpr uintptr_t MaxDragTranslation = 0x234;
            inline constexpr uintptr_t MaxForce = 0x274;
            inline constexpr uintptr_t MaxTorque = 0x278;
            inline constexpr uintptr_t MinDragAngle = 0x27c;
            inline constexpr uintptr_t MinDragTranslation = 0x240;
            inline constexpr uintptr_t ReferenceInstance = 0x1b8;
            inline constexpr uintptr_t Responsiveness = 0x288;
        }
        namespace GuiObject {
            inline constexpr uintptr_t BackgroundColor3 = 0x520;
            inline constexpr uintptr_t BackgroundTransparency = 0x544;
            inline constexpr uintptr_t BorderColor3 = 0x52c;
            inline constexpr uintptr_t LayoutOrder = 0x55c;
            inline constexpr uintptr_t Position = 0x4f0;
            inline constexpr uintptr_t Rotation = 0x574;
            inline constexpr uintptr_t ScreenGui_Enabled = 0x4a4;
            inline constexpr uintptr_t Size = 0x510;
            inline constexpr uintptr_t Visible = 0x58d;
            inline constexpr uintptr_t ZIndex = 0x584;
        }
        namespace Humanoid {
            inline constexpr uintptr_t AutoJumpEnabled = 0x1b8;
            inline constexpr uintptr_t AutoRotate = 0x1b9;
            inline constexpr uintptr_t AutomaticScalingEnabled = 0x1ba;
            inline constexpr uintptr_t BreakJointsOnDeath = 0x1bb;
            inline constexpr uintptr_t CameraOffset = 0x100;
            inline constexpr uintptr_t DisplayDistanceType = 0x164;
            inline constexpr uintptr_t DisplayName = 0xa0;
            inline constexpr uintptr_t EvaluateStateMachine = 0x1bc;
            inline constexpr uintptr_t Health = 0x174;
            inline constexpr uintptr_t HealthDisplayDistance = 0x16c;
            inline constexpr uintptr_t HealthDisplayType = 0x170;
            inline constexpr uintptr_t HipHeight = 0x178;
            inline constexpr uintptr_t Jump = 0x1bf;
            inline constexpr uintptr_t JumpHeight = 0x184;
            inline constexpr uintptr_t JumpPower = 0x188;
            inline constexpr uintptr_t MaxHealth = 0x18c;
            inline constexpr uintptr_t MaxSlopeAngle = 0x190;
            inline constexpr uintptr_t MoveDirection = 0x124;
            inline constexpr uintptr_t MoveToPoint = 0x148;
            inline constexpr uintptr_t NameDisplayDistance = 0x194;
            inline constexpr uintptr_t NameOcclusion = 0x198;
            inline constexpr uintptr_t PlatformStand = 0x1c1;
            inline constexpr uintptr_t RequiresNeck = 0x1c2;
            inline constexpr uintptr_t RigType = 0x1a4;
            inline constexpr uintptr_t SeatPart = 0xe0;
            inline constexpr uintptr_t Sit = 0x1c3;
            inline constexpr uintptr_t TargetPoint = 0x130;
            inline constexpr uintptr_t UseJumpPower = 0x1c5;
            inline constexpr uintptr_t Walkspeed = 0x1b4;
        }
        namespace Instance {
            inline constexpr std::array<uintptr_t, 2> ChildrenEnd_Path = {0x70, 0x8};
            inline constexpr std::array<uintptr_t, 2> ChildrenStart_Path = {0x70, 0x0};
            inline constexpr uintptr_t Name = 0x68;
            inline constexpr uintptr_t Parent = 0x60;
        }
        namespace LRUNode {
            inline constexpr uintptr_t AssetID = 0x10;
            inline constexpr uintptr_t CachedItem = 0x30;
            inline constexpr uintptr_t Next = 0x8;
        }
        namespace Lighting {
            inline constexpr uintptr_t Ambient = 0xb8;
            inline constexpr uintptr_t Brightness = 0x100;
            inline constexpr uintptr_t ColorShift_Bottom = 0xc4;
            inline constexpr uintptr_t ColorShift_Top = 0xd0;
            inline constexpr uintptr_t EnvironmentDiffuseScale = 0x104;
            inline constexpr uintptr_t EnvironmentSpecularScale = 0x108;
            inline constexpr uintptr_t ExposureCompensation = 0x10c;
            inline constexpr uintptr_t FogColor = 0xdc;
            inline constexpr uintptr_t FogEnd = 0x114;
            inline constexpr uintptr_t FogStart = 0x118;
            inline constexpr uintptr_t GeographicLatitude = 0x11c;
            inline constexpr uintptr_t GlobalShadows = 0x12c;
            inline constexpr uintptr_t OutdoorAmbient = 0xe8;
        }
        namespace LocalScript {
            inline constexpr uintptr_t ByteCode = 0x168;
            inline constexpr uintptr_t GUID = 0xb8;
            inline constexpr uintptr_t Hash = 0x178;
            inline constexpr uintptr_t Source = 0x160;
            inline constexpr uintptr_t Disabled = 0x10c;
            inline constexpr uintptr_t RunContext = 0x108;
            inline constexpr uintptr_t LinkedSource = 0xe8;
        }
        namespace MaterialColors {
            inline constexpr uintptr_t Asphalt = 0x30;
            inline constexpr uintptr_t Basalt = 0x27;
            inline constexpr uintptr_t Brick = 0xf;
            inline constexpr uintptr_t Cobblestone = 0x33;
            inline constexpr uintptr_t Concrete = 0xc;
            inline constexpr uintptr_t CrackedLava = 0x2d;
            inline constexpr uintptr_t Glacier = 0x1b;
            inline constexpr uintptr_t Grass = 0x6;
            inline constexpr uintptr_t Ground = 0x2a;
            inline constexpr uintptr_t Ice = 0x36;
            inline constexpr uintptr_t LeafyGrass = 0x39;
            inline constexpr uintptr_t Limestone = 0x3f;
            inline constexpr uintptr_t Mud = 0x24;
            inline constexpr uintptr_t Pavement = 0x42;
            inline constexpr uintptr_t Rock = 0x18;
            inline constexpr uintptr_t Salt = 0x3c;
            inline constexpr uintptr_t Sand = 0x12;
            inline constexpr uintptr_t Sandstone = 0x21;
            inline constexpr uintptr_t Slate = 0x9;
            inline constexpr uintptr_t Snow = 0x1e;
            inline constexpr uintptr_t WoodPlanks = 0x15;
        }
        namespace MemEnforcedLRUCache {
            inline constexpr uintptr_t Head = 0x10;
        }
        namespace MeshContentProvider {
            inline constexpr uintptr_t LRUHolder = 0xc0;
        }
        namespace MeshPart {
            inline constexpr uintptr_t MeshId = 0x2d0;
            inline constexpr uintptr_t Texture = 0x2f8;
        }
        namespace Misc {
            inline constexpr uintptr_t AnimationId = 0x98;
        }
        namespace Model {
            inline constexpr uintptr_t PrimaryPart = 0x200;
            inline constexpr uintptr_t Scale = 0x124;
        }
        namespace ModuleScript {
            inline constexpr uintptr_t ByteCode = 0x110;
            inline constexpr uintptr_t GUID = 0xb8;
            inline constexpr uintptr_t Hash = 0x120;
            inline constexpr uintptr_t Source = 0x108;
            inline constexpr uintptr_t LinkedSource = 0xe8;
            inline constexpr uintptr_t CachedRemoteSource = 0xa0;
            inline constexpr uintptr_t CachedRemoteSourceLoadState = 0xd0;
        }
        namespace ParticleEmitter {
            inline constexpr uintptr_t Acceleration = 0x1c0;
            inline constexpr uintptr_t Brightness = 0x1fc;
            inline constexpr uintptr_t Drag = 0x200;
            inline constexpr uintptr_t Lifetime = 0x1d4;
            inline constexpr uintptr_t LightEmission = 0x218;
            inline constexpr uintptr_t LightInfluence = 0x21c;
            inline constexpr uintptr_t Rate = 0x228;
            inline constexpr uintptr_t RotSpeed = 0x1dc;
            inline constexpr uintptr_t Rotation = 0x1e4;
            inline constexpr uintptr_t Speed = 0x1ec;
            inline constexpr uintptr_t SpreadAngle = 0x1f4;
            inline constexpr uintptr_t Texture = 0x198;
            inline constexpr uintptr_t TimeScale = 0x23c;
            inline constexpr uintptr_t VelocityInheritance = 0x240;
            inline constexpr uintptr_t ZOffset = 0x244;
        }
        namespace Player {
            inline constexpr uintptr_t AccountAge = 0x30c;
            inline constexpr uintptr_t CameraMode = 0x320;
            inline constexpr uintptr_t DisplayName = 0x108;
            inline constexpr uintptr_t HealthDisplayDistance = 0x344;
            inline constexpr uintptr_t LocalPlayer = 0x108;
            inline constexpr uintptr_t MaxZoomDistance = 0x318;
            inline constexpr uintptr_t MinZoomDistance = 0x31c;
            inline constexpr uintptr_t ModelInstance = 0x248;
            inline constexpr uintptr_t Mouse = 0x10f0;
            inline constexpr uintptr_t NameDisplayDistance = 0x354;
            inline constexpr uintptr_t TeamColor = 0x360;
        }
        namespace PlayerMouse {
            inline constexpr uintptr_t Icon = 0xa0;
        }
        namespace Primitive {
            inline constexpr uintptr_t AssemblyAngularVelocity = 0xec;
            inline constexpr uintptr_t Flags = 0x1be;
            inline constexpr uintptr_t Material = 0x24e;
            inline constexpr uintptr_t Position = 0xd4;
            inline constexpr uintptr_t Size = 0x1c4;
        }
        namespace PrimitiveFlags {
            inline constexpr uintptr_t Anchored = 0x2;
            inline constexpr uint64_t Anchored_Mask = 0x2;
            inline constexpr uintptr_t CanCollide = 0x8;
            inline constexpr uint64_t CanCollide_Mask = 0x8;
            inline constexpr uintptr_t CanQuery = 0x20;
            inline constexpr uint64_t CanQuery_Mask = 0x20;
            inline constexpr uintptr_t CanTouch = 0x10;
            inline constexpr uint64_t CanTouch_Mask = 0x10;
        }
        namespace ProximityPrompt {
            inline constexpr uintptr_t ActionText = 0x98;
            inline constexpr uintptr_t Enabled = 0x10e;
            inline constexpr uintptr_t GamepadKeyCode = 0xf4;
            inline constexpr uintptr_t HoldDuration = 0xf8;
            inline constexpr uintptr_t KeyCode = 0xfc;
            inline constexpr uintptr_t MaxActivationDistance = 0x100;
            inline constexpr uintptr_t ObjectText = 0xb0;
            inline constexpr uintptr_t RequiresLineOfSight = 0x10f;
        }
        namespace RenderJob {
            inline constexpr uintptr_t RealDataModel = 0x188;
        }
        namespace RenderView {
            inline constexpr uintptr_t VisualEngine = 0x18;
        }
        namespace Script {
            inline constexpr uintptr_t ByteCode = 0x168;
            inline constexpr uintptr_t GUID = 0xb8;
            inline constexpr uintptr_t Hash = 0x178;
            inline constexpr uintptr_t Source = 0x160;
            inline constexpr uintptr_t Disabled = 0x10c;
            inline constexpr uintptr_t RunContext = 0x108;
            inline constexpr uintptr_t LinkedSource = 0xe8;
        }
        namespace Seat {
            inline constexpr uintptr_t Occupant = 0x1f0;
        }
        namespace Sky {
            inline constexpr uintptr_t MoonAngularSize = 0x1ec;
            inline constexpr uintptr_t MoonTextureId = 0xa0;
            inline constexpr uintptr_t SkyboxBk = 0xc8;
            inline constexpr uintptr_t SkyboxDn = 0xf0;
            inline constexpr uintptr_t SkyboxFt = 0x118;
            inline constexpr uintptr_t SkyboxLf = 0x140;
            inline constexpr uintptr_t SkyboxOrientation = 0x1e0;
            inline constexpr uintptr_t SkyboxRt = 0x168;
            inline constexpr uintptr_t SkyboxUp = 0x190;
            inline constexpr uintptr_t StarCount = 0x1f0;
            inline constexpr uintptr_t SunAngularSize = 0x1f4;
            inline constexpr uintptr_t SunTextureId = 0x1b8;
        }
        namespace Sound {
            inline constexpr uintptr_t IsPlaying = 0x120;
            inline constexpr uintptr_t Looped = 0x11d;
            inline constexpr uintptr_t PlaybackSpeed = 0xfc;
            inline constexpr uintptr_t RollOffMaxDistance = 0x100;
            inline constexpr uintptr_t RollOffMinDistance = 0x104;
            inline constexpr uintptr_t SoundGroup = 0xc8;
            inline constexpr uintptr_t SoundId = 0xa0;
            inline constexpr uintptr_t Volume = 0x110;
        }
        namespace SpawnLocation {
            inline constexpr uintptr_t AllowTeamChangeOnTouch = 0x1c8;
            inline constexpr uintptr_t Enabled = 0x1c9;
            inline constexpr uintptr_t ForcefieldDuration = 0x1c0;
            inline constexpr uintptr_t Neutral = 0x1ca;
            inline constexpr uintptr_t TeamColor = 0x1c4;
        }
        namespace SpecialMesh {
            inline constexpr uintptr_t MeshId = 0xd0;
            inline constexpr uintptr_t Scale = 0xac;
        }
        namespace SunRaysEffect {
            inline constexpr uintptr_t Enabled = 0x95;
            inline constexpr uintptr_t Intensity = 0x98;
            inline constexpr uintptr_t Spread = 0x9c;
        }
        namespace SurfaceAppearance {
            inline constexpr uintptr_t AlphaMode = 0x1a8;
            inline constexpr uintptr_t Color = 0x190;
            inline constexpr uintptr_t ColorMap = 0xa0;
            inline constexpr uintptr_t EmissiveMaskContent = 0xc8;
            inline constexpr uintptr_t EmissiveStrength = 0x1ac;
            inline constexpr uintptr_t EmissiveTint = 0x19c;
            inline constexpr uintptr_t MetalnessMap = 0xf0;
            inline constexpr uintptr_t NormalMap = 0x118;
            inline constexpr uintptr_t RoughnessMap = 0x140;
        }
        namespace TaskScheduler {
            inline constexpr uintptr_t JobEnd = 0xd8;
            inline constexpr uintptr_t JobName = 0x18;
            inline constexpr uintptr_t JobStart = 0xd0;
            inline const uintptr_t Pointer = REBASE(0x7de0e90);
        }
        namespace Team {
            inline constexpr uintptr_t BrickColor = 0xa0;
        }
        namespace Terrain {
            inline constexpr uintptr_t GrassLength = 0x1c8;
            inline constexpr uintptr_t MaterialColors = 0x490;
            inline constexpr uintptr_t WaterColor = 0x1b8;
            inline constexpr uintptr_t WaterReflectance = 0x1d0;
            inline constexpr uintptr_t WaterTransparency = 0x1d4;
            inline constexpr uintptr_t WaterWaveSize = 0x1d8;
            inline constexpr uintptr_t WaterWaveSpeed = 0x1dc;
        }
        namespace Tool {
            inline constexpr uintptr_t CanBeDropped = 0x430;
            inline constexpr uintptr_t Enabled = 0x431;
            inline constexpr uintptr_t Grip = 0x400;
            inline constexpr uintptr_t ManualActivationOnly = 0x432;
            inline constexpr uintptr_t RequiresHandle = 0x433;
            inline constexpr uintptr_t TextureId = 0x2f8;
            inline constexpr uintptr_t Tooltip = 0x3e8;
        }
        namespace UnionOperation {
            inline constexpr uintptr_t AssetId = 0x2d0;
        }
        namespace VehicleSeat {
            inline constexpr uintptr_t MaxSpeed = 0x200;
            inline constexpr uintptr_t SteerFloat = 0x204;
            inline constexpr uintptr_t ThrottleFloat = 0x208;
            inline constexpr uintptr_t Torque = 0x20c;
            inline constexpr uintptr_t TurnSpeed = 0x210;
        }
        namespace VisualEngine {
            inline constexpr uintptr_t RenderView = 0xc70;
        }
        namespace Weld {
            inline constexpr uintptr_t Part0 = 0x100;
            inline constexpr uintptr_t Part1 = 0x110;
        }
        namespace WeldConstraint {
            inline constexpr uintptr_t Part0 = 0xa0;
            inline constexpr uintptr_t Part1 = 0xb0;
        }
        namespace Workspace {
            inline constexpr uintptr_t CurrentCamera = 0x458;
            inline constexpr uintptr_t DistributedGameTime = 0x478;
            inline constexpr uintptr_t World = 0x3b8;
        }
        namespace World {
            inline constexpr uintptr_t AirProperties = 0x230;
        }
        namespace LuaSourceContainer {
            inline constexpr uintptr_t GUID = 0xb8;
            inline constexpr uintptr_t CachedRemoteSource = 0xa0;
            inline constexpr uintptr_t CachedRemoteSourceLoadState = 0xd0;
            inline constexpr std::array<uintptr_t, 2> SandboxedSource_Path = {0xd8, 0x8};
        }
        namespace BaseScript {
            inline constexpr uintptr_t Disabled = 0x10c;
            inline constexpr uintptr_t RunContext = 0x108;
            inline constexpr uintptr_t LinkedSource = 0xe8;
        }
        namespace ProtectedString {
            inline constexpr uintptr_t Source = 0x0;
            inline constexpr uintptr_t ByteCode = 0x8;
        }
        namespace SharedString {
            inline constexpr std::array<uintptr_t, 2> Contents_Path = {0x0, 0x10};
        }
        namespace DataModelPointer {
            inline constexpr uintptr_t Scheduler = 0x7de0e90;
            inline constexpr uintptr_t JobVTable = 0x74a6920;
            inline constexpr uintptr_t DataModelVTable = 0x770c390;
            inline constexpr uintptr_t JobDataModel = 0x188;
            inline constexpr uintptr_t JobStride = 0x10;
        }
        namespace TextLabel {
            inline constexpr uintptr_t Text = 0xe60;
            inline constexpr uintptr_t TextColor3 = 0xe88;
            inline constexpr uintptr_t RichText = 0xec5;
        }
        namespace TextButton {
            inline constexpr uintptr_t Text = 0x10e8;
            inline constexpr uintptr_t TextColor3 = 0x1110;
            inline constexpr uintptr_t RichText = 0x114d;
        }
        namespace TextBox {
            inline constexpr uintptr_t TextColor3 = 0xeac;
            inline constexpr uintptr_t RichText = 0xefe;
        }
        namespace ImageLabel {
            inline constexpr uintptr_t Image = 0xa70;
        }
        namespace ImageButton {
            inline constexpr uintptr_t Image = 0xd20;
        }
        namespace ShirtGraphic {
            inline constexpr uintptr_t Graphic = 0xa8;
        }
        namespace BillboardGui {
            inline constexpr uintptr_t Adornee = 0x6a8;
        }
        namespace SurfaceGui {
            inline constexpr uintptr_t Adornee = 0x6a8;
        }
        namespace Highlight {
            inline constexpr uintptr_t Adornee = 0xa0;
        }
        namespace SelectionBox {
            inline constexpr uintptr_t Adornee = 0xd8;
        }
        namespace BoxHandleAdornment {
            inline constexpr uintptr_t Adornee = 0xd8;
        }
        namespace ViewportFrame {
            inline constexpr uintptr_t CurrentCamera = 0x9c8;
            inline constexpr uintptr_t Size = 0x510;
        }
        namespace BinaryStringValue {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace BoolValue {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace BrickColorValue {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace CFrameValue {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace Color3Value {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace DoubleConstrainedValue {
            inline constexpr uintptr_t Value = 0xb0;
        }
        namespace IntConstrainedValue {
            inline constexpr uintptr_t Value = 0xb0;
        }
        namespace IntValue {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace NumberValue {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace ObjectValue {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace RayValue {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace StringValue {
            inline constexpr uintptr_t Value = 0xa0;
        }
        namespace Vector3Value {
            inline constexpr uintptr_t Value = 0xa0;
        }

        namespace DataModel {
            template <class ReadPointer>
            uintptr_t Pointer(uintptr_t moduleBase, ReadPointer readPointer, uintptr_t maxJobs = 4096) {
                const auto scheduler = readPointer(moduleBase + DataModelPointer::Scheduler);
                if (!scheduler) return 0;
                const auto begin = readPointer(scheduler + TaskScheduler::JobStart);
                const auto end = readPointer(scheduler + TaskScheduler::JobEnd);
                if (!begin || end < begin || (end - begin) % DataModelPointer::JobStride) return 0;
                const auto count = (end - begin) / DataModelPointer::JobStride;
                if (count > maxJobs) return 0;
                uintptr_t result = 0;
                for (uintptr_t index = 0; index < count; ++index) {
                    const auto job = readPointer(begin + index * DataModelPointer::JobStride);
                    if (!job || readPointer(job) != moduleBase + DataModelPointer::JobVTable) continue;
                    const auto candidate = readPointer(job + DataModelPointer::JobDataModel);
                    if (!candidate || readPointer(candidate) != moduleBase + DataModelPointer::DataModelVTable) continue;
                    if (result && result != candidate) return 0;
                    result = candidate;
                }
                if (readPointer(scheduler + TaskScheduler::JobStart) != begin || readPointer(scheduler + TaskScheduler::JobEnd) != end) return 0;
                return result;
            }
        }
    }
}
