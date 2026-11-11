/*
 * ps3recomp - cellUserInfo HLE implementation
 *
 * Reports the default user (ID=00000001, name="User") plus, for local
 * split-screen co-op, PS3_LOCAL_USERS-1 extra local users (default 2 users in
 * total; ID=00000002 is named by PS3_USER2_NAME, default "Player 2"). The user
 * selection dialogs (SelectUser_ListType / _SetList) auto-pick the first
 * selectable user that is not the current one and answer through the sysutil
 * callback pump, as the system dialog does.
 */

#include "cellUserInfo.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/memory/vm.h"        /* VM_HLE_INJECT_BASE */   /* vm_base (guest mem) */
/* HLE args arrive as guest effective addresses; translate before deref. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))
/* The guest is big-endian; u32 fields we write into guest structs must be stored
 * byte-swapped or the guest reads them mangled (e.g. a count of 1 read as
 * 0x01000000 = 16.7M, blowing up the caller's enumeration loop). */
#define BE32(v) ( (((uint32_t)(v) & 0x000000FFu) << 24) | (((uint32_t)(v) & 0x0000FF00u) << 8) | \
                  (((uint32_t)(v) & 0x00FF0000u) >> 8) | (((uint32_t)(v) & 0xFF000000u) >> 24) )

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* The emulated PS3's local users: 1 is the signed-in (current) user. */
#define EMU_USER_ID     CELL_USERINFO_DEFAULT_USER_ID
#define EMU_USER_NAME   "User"
#define EMU_USER_MAX    4

static u32 s_selected_user = EMU_USER_ID;

/* Real SDK error/result values (the header's base is not the firmware's). */
#define USERINFO_RET_OK        0
#define USERINFO_RET_CANCEL    1
#define USERINFO_ERR_NOUSER    0x8002C304u

static u32 emu_user_count(void)
{
    static int n = -1;
    if (n < 0) {
        const char* e = getenv("PS3_LOCAL_USERS");
        n = e && *e ? atoi(e) : 2;
        if (n < 1) n = 1;
        if (n > EMU_USER_MAX) n = EMU_USER_MAX;
    }
    return (u32)n;
}

static int emu_user_valid(u32 id) { return id >= 1 && id <= emu_user_count(); }

static const char* emu_user_name(u32 id)
{
    static char buf[EMU_USER_MAX + 1][CELL_USERINFO_USERNAME_SIZE];
    if (id == EMU_USER_ID) return EMU_USER_NAME;
    if (id > EMU_USER_MAX) return "";
    if (!buf[id][0]) {
        char key[24];
        snprintf(key, sizeof key, "PS3_USER%u_NAME", id);
        const char* e = getenv(key);
        if (e && *e) snprintf(buf[id], sizeof buf[id], "%s", e);
        else snprintf(buf[id], sizeof buf[id], "Player %u", id);
    }
    return buf[id];
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellUserInfoGetStat(u32 id, CellUserInfoUserStat* stat)
{
    if (!stat)
        return CELL_USERINFO_ERROR_PARAM;
    stat = GUEST_PTR(stat, CellUserInfoUserStat*);

    if (id == 0)   /* CELL_SYSUTIL_USERID_CURRENT */
        id = EMU_USER_ID;
    if (!emu_user_valid(id))
        return CELL_USERINFO_ERROR_NOUSER;

    /* NOTE: write ONLY the real 68-byte struct (id+name). Anything more
     * overflows the guest's stack buffer (see header comment). */
    memset(stat, 0, sizeof(CellUserInfoUserStat));
    stat->id = BE32(id);
    strncpy(stat->name, emu_user_name(id), CELL_USERINFO_USERNAME_SIZE - 1);

    return CELL_OK;
}

s32 cellUserInfoGetList(u32* listNum, CellUserInfoUserList* list, u32* currentUser)
{
    printf("[cellUserInfo] GetList()\n");
    u32* listNum_h = GUEST_PTR(listNum, u32*);
    CellUserInfoUserList* list_h = GUEST_PTR(list, CellUserInfoUserList*);
    u32* currentUser_h = GUEST_PTR(currentUser, u32*);

    if (listNum_h)
        *listNum_h = BE32(emu_user_count());

    if (list_h) {
        memset(list_h, 0, sizeof(CellUserInfoUserList));
        for (u32 i = 0; i < emu_user_count(); i++)
            list_h->userId[i] = BE32(EMU_USER_ID + i);
    }

    if (currentUser_h)
        *currentUser_h = BE32(EMU_USER_ID);

    return CELL_OK;
}

s32 cellUserInfoSelectUser_ListSet(u32 listType,
                                    const CellUserInfoListSet* listSet,
                                    CellUserInfoFinishCallback callback,
                                    void* userdata)
{
    (void)listType;
    (void)listSet;
    printf("[cellUserInfo] SelectUser_ListSet()\n");

    /* Auto-select the default user */
    s_selected_user = EMU_USER_ID;

    /* Invoke callback immediately (synchronous selection) */
    if (callback)
        callback(CELL_OK, EMU_USER_ID, userdata);

    return CELL_OK;
}

s32 cellUserInfoSelectUser_ListGet(u32* selectedUser)
{
    printf("[cellUserInfo] SelectUser_ListGet()\n");

    if (!selectedUser)
        return CELL_USERINFO_ERROR_PARAM;
    selectedUser = GUEST_PTR(selectedUser, u32*);

    *selectedUser = BE32(s_selected_user);
    return CELL_OK;
}

s32 cellUserInfoEnableOverlay(s32 enable)
{
    printf("[cellUserInfo] EnableOverlay(%d)\n", enable);
    /* No-op in recomp -- no system overlay */
    return CELL_OK;
}

s32 cellUserInfoGetHomeDir(u32 id, char* homePath, u32 homePathSize)
{
    printf("[cellUserInfo] GetHomeDir(id=%u)\n", id);

    if (!homePath || homePathSize == 0)
        return CELL_USERINFO_ERROR_PARAM;
    homePath = GUEST_PTR(homePath, char*);

    if (id == 0)   /* CELL_SYSUTIL_USERID_CURRENT */
        id = EMU_USER_ID;
    if (!emu_user_valid(id))
        return CELL_USERINFO_ERROR_NOUSER;

    snprintf(homePath, homePathSize, "/dev_hdd0/home/%08u", id);
    homePath[homePathSize - 1] = '\0';

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * User selection dialogs (real SDK signatures; all arguments are guest EAs)
 *
 * The game opens these when a second player joins split-screen co-op (R3:
 * press START on another pad; R2: the co-op staging screen's P2 join) and
 * waits for funcSelect(result, CellUserInfoUserStat*, userdata). With no
 * implementation the import faked CELL_OK, the callback never came and the
 * join hung. There is no system dialog to show, so pick the user the player
 * would: the first selectable one that is not the current (player 1) user.
 * -----------------------------------------------------------------------*/

extern s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]);

static u32 userinfo_stat_ea(void)
{
    /* Two alternating 128-byte guest cells near the end of the runtime's
     * injected block, which nothing else there reaches. */
    static u32 n;
    return VM_HLE_INJECT_BASE + 0xFF0000u + ((n++ & 1u) * 0x80u);
}

static s32 userinfo_answer(const char* who, u32 func_opd, u32 userdata, u32 id)
{
    const u32 st = userinfo_stat_ea();
    s32 result = USERINFO_RET_OK;
    memset(vm_base + st, 0, sizeof(CellUserInfoUserStat));
    if (id) {
        CellUserInfoUserStat* h = (CellUserInfoUserStat*)(vm_base + st);
        h->id = BE32(id);
        strncpy(h->name, emu_user_name(id), CELL_USERINFO_USERNAME_SIZE - 1);
        s_selected_user = id;
    } else {
        result = (s32)USERINFO_ERR_NOUSER;
    }
    printf("[cellUserInfo] %s -> %s user %u (%s)\n", who,
           id ? "selected" : "no selectable", id, id ? emu_user_name(id) : "-");
    const u64 args[8] = { (u64)(s64)result, st, userdata, 0, 0, 0, 0, 0 };
    return cellSysutilQueueGuestCallbackArgs(func_opd, args);
}

/* CellUserInfoTypeSet { char* title; u32 focus; u32 type; void* reserved; } */
s32 cellUserInfoSelectUser_ListType(u32 type_ea, u32 func_opd, u32 container, u32 userdata)
{
    (void)container;
    if (!type_ea || !func_opd)
        return (s32)0x8002C303u;   /* CELL_USERINFO_ERROR_PARAM */
    const u32 pick = emu_user_count() > EMU_USER_ID ? EMU_USER_ID + 1 : 0;
    return userinfo_answer("SelectUser_ListType", func_opd, userdata, pick);
}

/* CellUserInfoListSet { char* title; u32 focus; u32 fixedListNum;
 *                       CellUserInfoUserList* fixedList; void* reserved; } */
s32 cellUserInfoSelectUser_SetList(u32 set_ea, u32 func_opd, u32 container, u32 userdata)
{
    (void)container;
    if (!set_ea || !func_opd)
        return (s32)0x8002C303u;   /* CELL_USERINFO_ERROR_PARAM */
    const u32 focus = BE32(*(u32*)(vm_base + set_ea + 4));
    u32 num = BE32(*(u32*)(vm_base + set_ea + 8));
    const u32 list = BE32(*(u32*)(vm_base + set_ea + 12));
    if (num > CELL_USERINFO_USER_MAX) num = CELL_USERINFO_USER_MAX;
    u32 pick = 0;
    for (u32 i = 0; list && i < num; i++) {
        const u32 id = BE32(*(u32*)(vm_base + list + 4 * i));
        if (!emu_user_valid(id)) continue;
        if (!pick || id == focus) pick = id;
    }
    return userinfo_answer("SelectUser_SetList", func_opd, userdata, pick);
}

/* The generated NID table predates these two; the port registers them. */
void cellUserInfo_register_select(void)
{
    extern void ps3_hle_register(u32 nid, const char* name, void* handler);
    ps3_hle_register(0x3097CC1Cu, "cellUserInfoSelectUser_ListType", (void*)cellUserInfoSelectUser_ListType);
    ps3_hle_register(0x55123A25u, "cellUserInfoSelectUser_SetList", (void*)cellUserInfoSelectUser_SetList);
}
