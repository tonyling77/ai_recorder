#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MEETING_NOTES_CAP 8
#define MEETING_NOTES_REMOVED_CAP 16
#define MEETING_NOTES_TODO_CAP 4

typedef struct {
    char id[24];
    char issued[20];
    char title[72];
    char conclusions[180];
    char todos[MEETING_NOTES_TODO_CAP][72];
    unsigned char todo_count;
} meeting_note_t;

typedef struct {
    meeting_note_t items[MEETING_NOTES_CAP];
    size_t count;
    char removed[MEETING_NOTES_REMOVED_CAP][24];
    size_t removed_count;
    size_t open_index;
    bool open;
} meeting_notes_t;

void meeting_notes_init(meeting_notes_t *notes);
const char *meeting_notes_empty_text(void);
int meeting_notes_merge_card(meeting_notes_t *notes, const char *id, const char *issued, const char *title,
                             const char *conclusions, const char *const *todos, size_t todo_count);
size_t meeting_notes_count(const meeting_notes_t *notes);
void meeting_notes_row(const meeting_notes_t *notes, size_t index, char *title, size_t title_cap, char *meta,
                       size_t meta_cap);
bool meeting_notes_open(meeting_notes_t *notes, size_t index);
bool meeting_notes_is_open(const meeting_notes_t *notes);
void meeting_notes_close(meeting_notes_t *notes);
void meeting_notes_detail(const meeting_notes_t *notes, char *title, size_t title_cap, char *when, size_t when_cap,
                          char *conclusions, size_t conclusions_cap, char *todos, size_t todos_cap);
bool meeting_notes_remove_open(meeting_notes_t *notes);
int meeting_notes_merge_document(meeting_notes_t *notes, const char *document);
/* A fingerprint of the minutes.
 *
 * The minutes arrive from the server over and over, and most of the time
 * they are the same ones as last time. The fingerprint says whether they
 * changed, so the copy on the flash is only written when it is worth
 * writing. */
unsigned meeting_notes_fingerprint(const meeting_notes_t *notes);
int meeting_notes_write(const meeting_notes_t *notes, char *out, size_t cap);
int meeting_notes_read(meeting_notes_t *notes, const char *document);
int meeting_notes_load(meeting_notes_t *notes, const char *path);
int meeting_notes_save(const meeting_notes_t *notes, const char *path);

#define MEETING_NOTES_PATH "/nand/meeting-notes.txt"
