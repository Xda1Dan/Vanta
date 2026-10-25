// Z4 | Roblox macOS Intel
// Version: 0.744.0.7441080
// Build: version-546c6da637854de3
//
// The struct definitions live in namespace z4 so they can share a translation
// unit with the vendored luau/VM/include/lua.h, which also defines
// lua_Callbacks and spells LUA_T_COUNT as an enumerator. Same layouts, same
// static_asserts - read them as z4::Proto, z4::lua_State and so on. The three
// VM constants the layouts are parameterised on are prefixed Z4_ for the same
// reason (LUA_EXTRA_SIZE 1, LUA_T_COUNT 14, TM_N 21 - all unchanged on 0.744).
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <memory>
#include <vector>
#include <string>
#include "encs.hpp"

using enc::vmval1; using enc::vmval2; using enc::vmval3; using enc::vmval4;

#define CLOSURE_CONT_ENC vmval3
#define LSTATE_STACKSIZE_ENC vmval3
#define PROTO_ABSLINEINFO_ENC vmval2
#define PROTO_DEBUGINSN_ENC vmval4
#define PROTO_DEBUGNAME_ENC vmval1
#define PROTO_LINEINFO_ENC vmval3
#define PROTO_LOCVARS_ENC vmval1
#define PROTO_SOURCE_ENC vmval1
#define PROTO_TYPEINFO_ENC vmval3
#define PROTO_UPVALUES_ENC vmval3
#define TSTRING_HASH_ENC vmval1
#define UDATA_META_ENC vmval1

#ifndef Z4_LUA_EXTRA_SIZE
#define Z4_LUA_EXTRA_SIZE 1
#endif
#ifndef Z4_LUA_T_COUNT
#define Z4_LUA_T_COUNT 14
#endif
#ifndef Z4_TM_N
#define Z4_TM_N 21
#endif

#ifndef Z4_COMMONHEADER
#define Z4_COMMONHEADER \
    uint8_t marked; \
    uint8_t memcat; \
    uint8_t tt
#endif

struct Buffer;
struct CallInfo;
struct Closure;
struct DirectFieldResult;
struct FeedbackVectorSlot;
struct GCCycleMetrics;
struct GCMetrics;
struct GCObject;
struct GCStats;
struct GCheader;
struct LocVar;
struct LuaNode;
struct LuaTable;
struct LuauClass;
struct LuauObject;
struct LuauVector;
struct Proto;
struct RobloxExtraSpace;
struct Shared;
struct TKey;
struct TString;
struct TValue;
struct Udata;
struct UpVal;
struct UpValOpen;
struct UpValStorage;
struct global_State;
struct lua_Callbacks;
struct lua_Debug;
struct lua_ExecutionCallbacks;
struct lua_Page;
struct lua_State;
struct lua_UdataDirectAccessData;
struct stringtable;
using Instruction = uint32_t;
using lua_CFunction = int (*)(lua_State*);
using lua_Continuation = int (*)(lua_State*, int);

namespace z4 {

#pragma pack(push, 1)

struct Buffer {
    Z4_COMMONHEADER; /* (0) (0x0) */
    char gap_0[0x1];
    unsigned int len; /* (4) (0x4) */
    char data[1]; /* (8) (0x8) */
};
static_assert(sizeof(Buffer) == 9, "sizeof(Buffer) == 9");

struct CallInfo {
    TValue* base; /* (0) (0x0) */
    TValue* func; /* (8) (0x8) */
    Proto* p; /* (16) (0x10) */
    TValue* top; /* (24) (0x18) */
    const Instruction* savedpc; /* (32) (0x20) */
    int nresults; /* (40) (0x28) */
    unsigned int flags; /* (44) (0x2C) */

};
static_assert(sizeof(CallInfo) == 48, "sizeof(CallInfo) == 48");

struct Closure {
    Z4_COMMONHEADER; /* (0) (0x0) */
    unsigned char isC; /* (3) (0x3) */
    unsigned char preload; /* (4) (0x4) */
    unsigned char nupvalues; /* (5) (0x5) */
    unsigned char stacksize; /* (6) (0x6) */
    char gap_0[0x1];
    GCObject* gclist; /* (8) (0x8) */
    LuaTable* env; /* (16) (0x10) */
    union {
        struct {
            TString* debugname; /* (0) (0x0) */
            CLOSURE_CONT_ENC<lua_Continuation> cont; /* (8) (0x8) */
            lua_CFunction f; /* (16) (0x10) */
        } c;
        struct {
            Proto* p; /* (0) (0x0) */
            char gap_1[0x10];
        } l;
    };

};
static_assert(sizeof(Closure) == 48, "sizeof(Closure) == 48");

struct GCCycleMetrics {
    char gap_0[0x8];
    size_t heaptriggersizebytes; /* (8) (0x8) */
    double pausetime; /* (16) (0x10) */
    double starttimestamp; /* (24) (0x18) */
    double endtimestamp; /* (32) (0x20) */
    double marktime; /* (40) (0x28) */
    double markassisttime; /* (48) (0x30) */
    double markmaxexplicittime; /* (56) (0x38) */
    size_t markexplicitsteps; /* (64) (0x40) */
    size_t markwork; /* (72) (0x48) */
    char gap_1[0x10];
    double atomictime; /* (96) (0x60) */
    char gap_2[0x28];
    double sweeptime; /* (144) (0x90) */
    double sweepassisttime; /* (152) (0x98) */
    double sweepmaxexplicittime; /* (160) (0xA0) */
    size_t sweepexplicitsteps; /* (168) (0xA8) */
    size_t sweepwork; /* (176) (0xB0) */
    size_t assistwork; /* (184) (0xB8) */
    size_t explicitwork; /* (192) (0xC0) */
    size_t propagatework; /* (200) (0xC8) */
    size_t propagateagainwork; /* (208) (0xD0) */
    char gap_3[0x8];
};
static_assert(sizeof(GCCycleMetrics) == 224, "sizeof(GCCycleMetrics) == 224");

struct GCheader {
    Z4_COMMONHEADER; /* (0) (0x0) */
};
static_assert(sizeof(GCheader) == 3, "sizeof(GCheader) == 3");

struct GCObject {
    GCheader gch; /* (0) (0x0) */
};
static_assert(sizeof(GCObject) == 3, "sizeof(GCObject) == 3");

struct LocVar {
    TString* varname; /* (0) (0x0) */
    int startpc; /* (8) (0x8) */
    int endpc; /* (12) (0xC) */
    uint8_t reg; /* (16) (0x10) */
    char gap_0[0x7];
};
static_assert(sizeof(LocVar) == 24, "sizeof(LocVar) == 24");

struct LuaTable {
    Z4_COMMONHEADER; /* (0) (0x0) */
    uint8_t tmcache; /* (3) (0x3) */
    uint8_t safeenv; /* (4) (0x4) */
    uint8_t nodemask8; /* (5) (0x5) */
    uint8_t lsizenode; /* (6) (0x6) */
    uint8_t readonly; /* (7) (0x7) */
    int sizearray; /* (8) (0x8) */
    int lastfree; /* (12) (0xC) */
    LuaTable* metatable; /* (16) (0x10) */
    TValue* array; /* (24) (0x18) */
    LuaNode* node; /* (32) (0x20) */
    GCObject* gclist; /* (40) (0x28) */

};
static_assert(sizeof(LuaTable) == 48, "sizeof(LuaTable) == 48");

struct Proto {
    Z4_COMMONHEADER; /* (0) (0x0) */
    unsigned char nups; /* (3) (0x3) */
    unsigned char is_vararg; /* (4) (0x4) */
    unsigned char maxstacksize; /* (5) (0x5) */
    unsigned char numparams; /* (6) (0x6) */
    unsigned char flags; /* (7) (0x7) */
    PROTO_SOURCE_ENC<TString*> source; /* (8) (0x8) */
    PROTO_UPVALUES_ENC<TString**> upvalues; /* (16) (0x10) */
    GCObject* gclist; /* (24) (0x18) */
    PROTO_LOCVARS_ENC<struct LocVar*> locvars; /* (32) (0x20) */
    unsigned int* codeentry; /* (40) (0x28) */
    Proto** p; /* (48) (0x30) */
    char gap_0[0x8];
    PROTO_LINEINFO_ENC<unsigned char*> lineinfo; /* (64) (0x40) */
    TValue* k; /* (72) (0x48) */
    unsigned int* code; /* (80) (0x50) */
    PROTO_ABSLINEINFO_ENC<int*> abslineinfo; /* (88) (0x58) */
    PROTO_DEBUGNAME_ENC<TString*> debugname; /* (96) (0x60) */
    PROTO_DEBUGINSN_ENC<unsigned char*> debuginsn; /* (104) (0x68) */
    void* execdata; /* (112) (0x70) */
    uintptr_t exectarget; /* (120) (0x78) */
    PROTO_TYPEINFO_ENC<unsigned char*> typeinfo; /* (128) (0x80) */
    int sizetypeinfo; /* (136) (0x88) */
    int sizeupvalues; /* (140) (0x8C) */
    int linedefined; /* (144) (0x90) */
    int sizek; /* (148) (0x94) */
    char gap_1[0x4];
    int linegaplog2; /* (156) (0x9C) */
    int sizelineinfo; /* (160) (0xA0) */
    int sizecode; /* (164) (0xA4) */
    int sizep; /* (168) (0xA8) */
    int sizelocvars; /* (172) (0xAC) */
    struct FeedbackVectorSlot* feedbackvec; /* (176) (0xB0) */
    int feedbackvecsize; /* (184) (0xB8) */
    char gap_2[0x4];
    Proto* optimized; /* (192) (0xC0) */
    Proto* deoptimized; /* (200) (0xC8) */
    uint64_t cost; /* (208) (0xD0) */

};
static_assert(sizeof(Proto) == 216, "sizeof(Proto) == 216");

struct TKey {
    uint64_t value; /* (0) (0x0) */
    int extra[Z4_LUA_EXTRA_SIZE]; /* (8) (0x8) */
    char gap_0[0x4];
};
static_assert(sizeof(TKey) == 16, "sizeof(TKey) == 16");

struct TString {
    Z4_COMMONHEADER; /* (0) (0x0) */
    char gap_0[0x1];
    uint16_t atomflag; /* (4) (0x4) */
    int16_t atom; /* (6) (0x6) */
    TString* next; /* (8) (0x8) */
    TSTRING_HASH_ENC<unsigned int> hash; /* (16) (0x10) */
    unsigned int len; /* (20) (0x14) */
    char data[1]; /* (24) (0x18) */
};
static_assert(sizeof(TString) == 25, "sizeof(TString) == 25");

struct TValue {
    uint64_t value; /* (0) (0x0) */
    int extra[Z4_LUA_EXTRA_SIZE]; /* (8) (0x8) */
    int tt; /* (12) (0xC) */
};
static_assert(sizeof(TValue) == 16, "sizeof(TValue) == 16");

struct Udata {
    Z4_COMMONHEADER; /* (0) (0x0) */
    unsigned char tag; /* (3) (0x3) */
    int len; /* (4) (0x4) */
    UDATA_META_ENC<struct LuaTable*> metatable; /* (8) (0x8) */
    char data[1]; /* (16) (0x10) */
};
static_assert(sizeof(Udata) == 17, "sizeof(Udata) == 17");

struct UpValOpen {
    UpVal* prev; /* (0) (0x0) */
    UpVal* next; /* (8) (0x8) */
    UpVal* threadnext; /* (16) (0x10) */
};
static_assert(sizeof(UpValOpen) == 24, "sizeof(UpValOpen) == 24");

struct UpValStorage {
    union {
        UpValOpen open; /* (0) (0x0) */
        TValue value; /* (0) (0x0) */
    };
};
static_assert(sizeof(UpValStorage) == 24, "sizeof(UpValStorage) == 24");

struct UpVal {
    Z4_COMMONHEADER; /* (0) (0x0) */
    uint8_t markedopen; /* (3) (0x3) */
    char gap_0[0x4];
    TValue* v; /* (8) (0x8) */
    UpValStorage u; /* (16) (0x10) */
};
static_assert(sizeof(UpVal) == 40, "sizeof(UpVal) == 40");

struct lua_Callbacks {
    char gap_0[0x8];
    void (*debugbreak)(lua_State* L, lua_Debug* ar); /* (8) (0x8) */
    void (*postresume)(lua_State* L); /* (16) (0x10) */
    void (*debugstep)(lua_State* L, lua_Debug* ar); /* (24) (0x18) */
    char gap_1[0x10];
    void (*preresume)(lua_State* L); /* (48) (0x30) */
    void (*debugprotectederror)(lua_State* L); /* (56) (0x38) */
    void (*debuginterrupt)(lua_State* L, lua_Debug* ar); /* (64) (0x40) */
    void (*onfree)(lua_State* L, void* block); /* (72) (0x48) */
    void (*userthread)(lua_State* LP, lua_State* L); /* (80) (0x50) */
    void (*interrupt)(lua_State* L, int gc); /* (88) (0x58) */
    char gap_2[0x8];
    void (*userfinalizer)(lua_State* L, lua_State* co); /* (104) (0x68) */

};
static_assert(sizeof(lua_Callbacks) == 112, "sizeof(lua_Callbacks) == 112");

struct lua_ExecutionCallbacks {
    void* context; /* (0) (0x0) */
    void (*close)(lua_State* L); /* (8) (0x8) */
    void (*destroy)(lua_State* L, Proto* proto); /* (16) (0x10) */
    int (*enter)(lua_State* L, Proto* proto); /* (24) (0x18) */
    void (*disable)(lua_State* L, Proto* proto); /* (32) (0x20) */
    size_t (*getmemorysize)(lua_State* L, Proto* proto); /* (40) (0x28) */
    uint8_t (*gettypemapping)(lua_State* L, const char* str, size_t len); /* (48) (0x30) */
    char* (*getcounterdata)(
        lua_State* L,
        Proto* proto,
        size_t* count
    ); /* (56) (0x38) */
    Proto* (*inlinefunction)(lua_State* L, Closure* caller, Closure* target, uint32_t pc); /* (64) (0x40) */
};
static_assert(sizeof(lua_ExecutionCallbacks) == 72, "sizeof(lua_ExecutionCallbacks) == 72");

struct global_State {
    char gap_0[0x10];
    unsigned char currentwhite; /* (16) (0x10) */
    unsigned char gcstate; /* (17) (0x11) */
    char gap_1[0x2];
    int gcgoal; /* (20) (0x14) */
    int gcstepmul; /* (24) (0x18) */
    int gcstepsize; /* (28) (0x1C) */
    uintptr_t frealloc; /* (32) (0x20) */
    void* ud; /* (40) (0x28) */
    GCObject* gray; /* (48) (0x30) */
    GCObject* grayagain; /* (56) (0x38) */
    GCObject* weak; /* (64) (0x40) */
    size_t GCthreshold; /* (72) (0x48) */
    char gap_2[0x150];
    lua_State* mainthread; /* (416) (0x1A0) */
    UpVal uvhead; /* (424) (0x1A8) */
    char gap_3[0x140];
    lua_Page* sweepgcopage; /* (784) (0x310) */
    lua_Page* allgcopages; /* (792) (0x318) */
    char gap_4[0x70];
    TString* tmname[Z4_TM_N]; /* (912) (0x390) */
    TString* ttname[Z4_LUA_T_COUNT]; /* (1080) (0x438) */
    char gap_5[0x160];
    TValue pseudotemp; /* (1544) (0x608) */
    TValue registry; /* (1560) (0x618) */
    char registryfree[0x4]; /* (1576) (0x628) */
    char gap_6[0x2C];
    uint64_t rngstate; /* (1624) (0x658) */
    lua_Callbacks cb; /* (1632) (0x660) */
    lua_ExecutionCallbacks ecb; /* (1744) (0x6D0) */
    char gap_7[0x42C8];
    Closure* builtinPcall; /* (18912) (0x49E0) */
    Closure* builtinXpcall; /* (18920) (0x49E8) */
    char gap_8[0x40];
    bool ptrencactive; /* (18992) (0x4A30) */
    char gap_9[0x29F];

};
static_assert(sizeof(global_State) == 19664, "sizeof(global_State) == 19664");

struct lua_State {
    Z4_COMMONHEADER; /* (0) (0x0) */
    uint8_t status; /* (3) (0x3) */
    uint8_t activememcat; /* (4) (0x4) */
    bool singlestep; /* (5) (0x5) */
    bool isactive; /* (6) (0x6) */
    char gap_0[0x1];
    LuaTable* gt; /* (8) (0x8) */
    char gap_1[0x8];
    CallInfo* base_ci; /* (24) (0x18) */
    unsigned short nCcalls; /* (32) (0x20) */
    unsigned short baseCcalls; /* (34) (0x22) */
    char gap_2[0x4];
    struct RobloxExtraSpace* userdata; /* (40) (0x28) */
    LuaTable* finalizers; /* (48) (0x30) */
    TString* namecall; /* (56) (0x38) */
    GCObject* gclist; /* (64) (0x40) */
    UpVal* openupval; /* (72) (0x48) */
    CallInfo* ci; /* (80) (0x50) */
    TValue* base; /* (88) (0x58) */
    TValue* stack; /* (96) (0x60) */
    TValue* stack_last; /* (104) (0x68) */
    TValue* top; /* (112) (0x70) */
    global_State* global; /* (120) (0x78) */
    LSTATE_STACKSIZE_ENC<int> stacksize; /* (128) (0x80) */
    int size_ci; /* (132) (0x84) */

};
static_assert(sizeof(lua_State) == 136, "sizeof(lua_State) == 136");

#pragma pack(pop)

} // namespace z4
