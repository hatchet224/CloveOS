#pragma once
#include <SDL.h>
#include <string>

bool commanderManagedOpen();
bool commanderManagedClose();
bool commanderMediaReady();
bool commanderPathAllowed(const std::string &path);
bool commanderValidName(const std::string &name);
bool commanderPollEvent(SDL_Event &event);
bool commanderPresent();
