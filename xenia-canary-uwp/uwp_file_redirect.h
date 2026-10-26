/**
 * Copyright (c) 2026 Skitzelles - https://github.com/skitzellez
 * MIT License: free to use in any project; keep this notice.
 */

#ifndef XENIA_UWP_FILE_REDIRECT_H_
#define XENIA_UWP_FILE_REDIRECT_H_

// Lets the app read games from any USB drive (FAT32, exFAT, or NTFS without
// the "ALL APPLICATION PACKAGES" permission step). Call once, first thing at
// start-up. Returns how many imports were pointed at the FromApp functions.
unsigned UwpRedirectFileApis();

#endif  // XENIA_UWP_FILE_REDIRECT_H_
