/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "supervisor/netplay_status.h"
#include "launcher/i18n.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    for(size_t i=0U;i<sizeof(sc_netplay_stages)/sizeof(sc_netplay_stages[0]);++i) {
        const struct sc_netplay_stage *stage=&sc_netplay_stages[i];
        assert(sc_netplay_stage(stage->id)==stage);
        assert(strlen(stage->id)<80U && strlen(stage->label)<160U && strlen(stage->help)<256U);
        for(size_t j=0U;j<i;++j)assert(strcmp(stage->id,sc_netplay_stages[j].id)!=0);
        cl_language_set(CL_SPANISH);
        assert(strcmp(CL_TR(stage->label),stage->label)!=0);
        assert(strcmp(CL_TR(stage->help),stage->help)!=0);
        cl_language_set(CL_ENGLISH);
        assert(strcmp(CL_TR(stage->label),stage->label)==0);
    }
    assert(sc_netplay_stage("untrusted arbitrary peer text")==&sc_netplay_stages[0]);
    assert(sc_netplay_menu_cancel("initializing\n"));
    assert(sc_netplay_menu_cancel("pairing-cartridges-and-saves\n"));
    assert(!sc_netplay_menu_cancel("emulator-running\n"));
    assert(!sc_netplay_menu_cancel("returning-saves\n"));
    assert(!sc_netplay_menu_cancel("restoring-network\n"));
    puts("NETPLAY_PHASE_LABEL_LOCALIZATION_AND_SAVE_RETURN_MENU_GUARD_PASS");return 0;
}
