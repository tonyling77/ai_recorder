#include "wifi_key_place.h"

static const wifi_key_place_t ON_SCREEN[WIFI_KEY_COUNT] = {
    {12, 104},  {58, 104},  {104, 104}, {150, 104}, {196, 104},
    {242, 104}, {288, 104}, {334, 104}, {380, 104}, {426, 104},
    {12, 158},  {58, 158},  {104, 158}, {150, 158}, {196, 158},
    {242, 158}, {288, 158}, {334, 158}, {380, 158}, {426, 158},
    {35, 212},  {81, 212},  {127, 212}, {173, 212}, {219, 212},
    {265, 212}, {311, 212}, {357, 212}, {403, 212},
    {12, 266},  {58, 266},  {104, 266}, {150, 266}, {196, 266},
    {242, 266}, {288, 266}, {334, 266}, {380, 266}, {426, 266},
};

wifi_key_place_t wifi_key_place(size_t index, bool shown)
{
    wifi_key_place_t parked = {.x = 0, .y = WIFI_KEY_PARK_Y};
    if (!shown || index >= WIFI_KEY_COUNT) {
        return parked;
    }
    return ON_SCREEN[index];
}
