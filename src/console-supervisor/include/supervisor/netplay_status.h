/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SC_NETPLAY_STATUS_H
#define SC_NETPLAY_STATUS_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
struct sc_netplay_stage { const char *id, *label, *help; bool cancellable; };
static const struct sc_netplay_stage sc_netplay_stages[] = {
    {"initializing", "Preparing the session", "Checking the selected game and save.", true},
    {"initializing-tgbdual", "Checking the link core", "Both devices need the matching TGB Dual version.", true},
    {"checking-own-battery-save", "Preparing your save", "Your normal game checkpoint stays separate.", true},
    {"starting-hotspot", "Starting the hotspot", "Start Join on the other handheld.", true},
    {"joining-hotspot", "Connecting to the host", "Keep both handhelds close together.", true},
    {"finding-associated-peer-address", "Waiting for player 2", "Choose Join on the other handheld and accept its prompts.", true},
    {"pairing-cartridges-and-saves", "Synchronizing games and saves", "Accept the confirmation on the Miyoo when it appears.", true},
    {"fetching-peer-cookie", "Reading the other player's game", "Waiting for the other handheld's selected game.", true},
    {"checking-peer-tgbdual-version", "Checking core compatibility", "Checking that both handhelds can play together.", true},
    {"fetching-peer-cartridge", "Synchronizing the cartridges", "The games can differ within the same Pokemon generation.", true},
    {"uploading-own-battery-save", "Sending your save", "Only the selected game's link save is transferred.", true},
    {"waiting-peer-battery-save", "Waiting for the other save", "Accept the save prompt on the other handheld.", true},
    {"first-ready-marker", "Games and saves synchronized", "Accept the ready prompt on the other handheld.", true},
    {"second-ready-marker", "Waiting for the host to start", "Choose Start now on the hosting Miyoo.", true},
    {"starting-emulator", "Starting the game", "Opening both linked cartridges.", false},
    {"waiting-emulator-listen", "Starting the host game", "Waiting for the emulator's network listener.", false},
    {"sending-ready", "The host is ready", "The other handheld can now start its game.", false},
    {"emulator-running", "The game is running", "Exit RetroArch normally to finish returning saves.", false},
    {"returning-saves", "Returning the updated saves", "Keep both handhelds on until save transfer finishes.", false},
    {"committing-own-save", "Saving your link progress", "Please wait. Do not remove the card or power off.", false},
    {"waiting-save-return", "Waiting for your updated save", "Exit the game normally on the hosting handheld too.", false},
    {"sending-peer-save", "Returning the other player's save", "Keep both handhelds on until save transfer finishes.", false},
    {"restoring-network", "Restoring your Wi-Fi", "Finishing cleanup before returning to MainUI.", false}
};
static inline const struct sc_netplay_stage *sc_netplay_stage(const char *id) {
    for (size_t i=0U;i<sizeof(sc_netplay_stages)/sizeof(sc_netplay_stages[0]);++i)
        if (strcmp(id,sc_netplay_stages[i].id)==0) return &sc_netplay_stages[i];
    return &sc_netplay_stages[0];
}
/* MENU must not kill the wrapper during save publication or network cleanup. */
static inline bool sc_netplay_menu_cancel(const char *phase) {
    return strcmp(phase,"emulator-running\n")!=0 &&
           strcmp(phase,"returning-saves\n")!=0 &&
           strcmp(phase,"restoring-network\n")!=0;
}
#endif
