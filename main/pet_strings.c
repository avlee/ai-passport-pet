// main/pet_strings.c
#include "pet_strings.h"

#define PET_UI_STRING_ENTRY(key, text, px) \
    { #key, text, (unsigned char)(px) },

const pet_string_t PET_UI_STRINGS[] = {
    PET_UI_STRING_LIST(PET_UI_STRING_ENTRY)
};

const unsigned PET_UI_STRINGS_COUNT =
    sizeof(PET_UI_STRINGS) / sizeof(PET_UI_STRINGS[0]);
