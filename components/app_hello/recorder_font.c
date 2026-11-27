#include "recorder_font.h"

#include <stddef.h>

#if defined(ESP_PLATFORM)
extern const unsigned char recorder_notes_font_start[] asm("_binary_MosaicoNotesCN_GB_ttf_start");
extern const unsigned char recorder_notes_font_end[] asm("_binary_MosaicoNotesCN_GB_ttf_end");
#endif

void recorder_font_apply(esp_gsp_config_t *config)
{
#if defined(ESP_PLATFORM)
    if (!config || &recorder_notes_font_end[0] <= &recorder_notes_font_start[0]) {
        return;
    }
    config->ttf = recorder_notes_font_start;
    config->ttf_size = (size_t)(&recorder_notes_font_end[0] - &recorder_notes_font_start[0]);
#else
    (void)config;
#endif
}
