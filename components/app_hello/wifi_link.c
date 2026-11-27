#include "wifi_link.h"

const char *wifi_link_text(bool connected)
{
    return connected ? "Wi-Fi" : "Offline";
}
