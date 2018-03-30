#ifndef INPUT_KEYNAMES_H
#define INPUT_KEYNAMES_H

#include <linux/input.h>

#if !defined(KEY_OK)
#include "input_fake.h"
#endif


struct input_key_name {
        const char *name;
        int         key;
};


static struct input_key_name key_name [] = {
        { "AB", KEY_AB },
        { "ANGLE", KEY_ANGLE },
        { "ARCHIVE", KEY_ARCHIVE },
        { "AUDIO", KEY_AUDIO },
        { "AUX", KEY_AUX },
        { "BLUE", KEY_BLUE },
        { "BREAK", KEY_BREAK },
        { "CALENDAR", KEY_CALENDAR },
        { "CD", KEY_CD },
        { "CHANNEL", KEY_CHANNEL },
        { "CHANNELDOWN", KEY_CHANNELDOWN },
        { "CHANNELUP", KEY_CHANNELUP },
        { "CLEAR", KEY_CLEAR },
        { "DIGITS", KEY_DIGITS },
        { "DIRECTORY", KEY_DIRECTORY },
        { "DVD", KEY_DVD },
        { "EPG", KEY_EPG },
        { "FAVORITES", KEY_FAVORITES },
        { "FIRST", KEY_FIRST },
        { "GOTO", KEY_GOTO },
        { "GREEN", KEY_GREEN },
        { "INFO", KEY_INFO },
        { "KEYBOARD", KEY_KEYBOARD },
        { "LANGUAGE", KEY_LANGUAGE },
        { "LAST", KEY_LAST },
        { "LIST", KEY_LIST },
        { "MEMO", KEY_MEMO },
        { "MHP", KEY_MHP },
        { "MODE", KEY_MODE },
        { "MP3", KEY_MP3 },
        { "NEXT", KEY_NEXT },
        { "OK", KEY_OK },
        { "OPTION", KEY_OPTION },
        { "PC", KEY_PC },
        { "PLAYER", KEY_PLAYER },
        { "POWER2", KEY_POWER2 },
        { "PREVIOUS", KEY_PREVIOUS },
        { "PROGRAM", KEY_PROGRAM },
        { "PVR", KEY_PVR },
        { "RADIO", KEY_RADIO },
        { "RED", KEY_RED },
        { "RESTART", KEY_RESTART },
        { "SAT", KEY_SAT },
        { "SAT2", KEY_SAT2 },
        { "SCREEN", KEY_SCREEN },
        { "SELECT", KEY_SELECT },
        { "SHUFFLE", KEY_SHUFFLE },
        { "SLOW", KEY_SLOW },
        { "SUBTITLE", KEY_SUBTITLE },
        { "TAPE", KEY_TAPE },
        { "TEEN", KEY_TEEN },
        { "TEXT", KEY_TEXT },
        { "TIME", KEY_TIME },
        { "TITLE", KEY_TITLE },
        { "TUNER", KEY_TUNER },
        { "TV", KEY_TV },
        { "TV2", KEY_TV2 },
        { "TWEN", KEY_TWEN },
        { "VCR", KEY_VCR },
        { "VCR2", KEY_VCR2 },
        { "VENDOR", KEY_VENDOR },
        { "VIDEO", KEY_VIDEO },
        { "YELLOW", KEY_YELLOW },
        { "ZOOM", KEY_ZOOM },
};

static struct input_key_name btn_name [] = {
};

#endif /* INPUT_KEYNAMES_H */

