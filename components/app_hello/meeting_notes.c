#include "meeting_notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char *out, size_t cap, const char *value)
{
    size_t length;
    if (!out || cap == 0) {
        return;
    }
    if (!value) {
        out[0] = '\0';
        return;
    }
    length = strlen(value);
    if (length >= cap) {
        length = cap - 1;
    }
    memcpy(out, value, length);
    out[length] = '\0';
}

static int removed_contains(const meeting_notes_t *notes, const char *id)
{
    for (size_t i = 0; i < notes->removed_count; i++) {
        if (strcmp(notes->removed[i], id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void remember_removed(meeting_notes_t *notes, const char *id)
{
    if (!id || !id[0] || removed_contains(notes, id) || notes->removed_count >= MEETING_NOTES_REMOVED_CAP) {
        return;
    }
    copy_text(notes->removed[notes->removed_count], sizeof(notes->removed[0]), id);
    notes->removed_count++;
}

static void sort_notes(meeting_notes_t *notes)
{
    for (size_t i = 1; i < notes->count; i++) {
        meeting_note_t value = notes->items[i];
        size_t j = i;
        while (j > 0 && strcmp(notes->items[j - 1].issued, value.issued) < 0) {
            notes->items[j] = notes->items[j - 1];
            j--;
        }
        notes->items[j] = value;
    }
}

static int field_number(const char *issued, size_t offset, size_t width)
{
    int value = 0;
    if (!issued || strlen(issued) < offset + width) {
        return 0;
    }
    for (size_t i = 0; i < width; i++) {
        char digit = issued[offset + i];
        if (digit < '0' || digit > '9') {
            return 0;
        }
        value = value * 10 + (digit - '0');
    }
    return value;
}

static void format_day(const char *issued, char *out, size_t cap, int with_time)
{
    int month = field_number(issued, 5, 2);
    int day = field_number(issued, 8, 2);
    const char *clock = issued && strlen(issued) >= 16 ? issued + 11 : "";
    if (with_time) {
        snprintf(out, cap, "%d月%d日  %s", month, day, clock);
    } else {
        snprintf(out, cap, "%d月%d日", month, day);
    }
}

void meeting_notes_init(meeting_notes_t *notes)
{
    if (!notes) {
        return;
    }
    memset(notes, 0, sizeof(*notes));
}

const char *meeting_notes_empty_text(void)
{
    return "还没有纪要";
}

static unsigned fingerprint_text(unsigned hash, const char *text)
{
    for (const char *cursor = text ? text : ""; *cursor; cursor++) {
        hash = (hash ^ (unsigned char)*cursor) * 16777619u;
    }
    return hash * 16777619u;
}

unsigned meeting_notes_fingerprint(const meeting_notes_t *notes)
{
    unsigned hash = 2166136261u;
    if (!notes) {
        return 0;
    }
    for (size_t i = 0; i < notes->count; i++) {
        const meeting_note_t *note = &notes->items[i];
        hash = fingerprint_text(hash, note->id);
        hash = fingerprint_text(hash, note->issued);
        hash = fingerprint_text(hash, note->title);
        hash = fingerprint_text(hash, note->conclusions);
        for (size_t t = 0; t < note->todo_count; t++) {
            hash = fingerprint_text(hash, note->todos[t]);
        }
    }
    for (size_t i = 0; i < notes->removed_count; i++) {
        hash = fingerprint_text(hash, notes->removed[i]);
    }
    return hash;
}

int meeting_notes_merge_card(meeting_notes_t *notes, const char *id, const char *issued, const char *title,
                             const char *conclusions, const char *const *todos, size_t todo_count)
{
    meeting_note_t *slot = NULL;
    if (!notes || !id || !id[0] || !issued || !issued[0] || !title || !title[0]) {
        return -1;
    }
    if (removed_contains(notes, id)) {
        return 0;
    }
    for (size_t i = 0; i < notes->count; i++) {
        if (strcmp(notes->items[i].id, id) == 0) {
            slot = &notes->items[i];
            break;
        }
    }
    if (!slot) {
        if (notes->count >= MEETING_NOTES_CAP) {
            slot = &notes->items[notes->count - 1];
            if (strcmp(issued, slot->issued) <= 0) {
                return 0;
            }
        } else {
            slot = &notes->items[notes->count++];
        }
    }
    memset(slot, 0, sizeof(*slot));
    copy_text(slot->id, sizeof(slot->id), id);
    copy_text(slot->issued, sizeof(slot->issued), issued);
    copy_text(slot->title, sizeof(slot->title), title);
    copy_text(slot->conclusions, sizeof(slot->conclusions), conclusions ? conclusions : "");
    if (todo_count > MEETING_NOTES_TODO_CAP) {
        todo_count = MEETING_NOTES_TODO_CAP;
    }
    slot->todo_count = (unsigned char)todo_count;
    for (size_t i = 0; i < todo_count; i++) {
        copy_text(slot->todos[i], sizeof(slot->todos[i]), todos && todos[i] ? todos[i] : "");
    }
    sort_notes(notes);
    return 0;
}

size_t meeting_notes_count(const meeting_notes_t *notes)
{
    return notes ? notes->count : 0;
}

void meeting_notes_row(const meeting_notes_t *notes, size_t index, char *title, size_t title_cap, char *meta,
                       size_t meta_cap)
{
    char day[32];
    if (!notes || index >= notes->count) {
        copy_text(title, title_cap, "");
        copy_text(meta, meta_cap, "");
        return;
    }
    copy_text(title, title_cap, notes->items[index].title);
    format_day(notes->items[index].issued, day, sizeof(day), 0);
    snprintf(meta, meta_cap, "%s  %u 条待办", day, (unsigned)notes->items[index].todo_count);
}

bool meeting_notes_open(meeting_notes_t *notes, size_t index)
{
    if (!notes || index >= notes->count) {
        return false;
    }
    notes->open = true;
    notes->open_index = index;
    return true;
}

bool meeting_notes_is_open(const meeting_notes_t *notes)
{
    return notes && notes->open;
}

void meeting_notes_close(meeting_notes_t *notes)
{
    if (notes) {
        notes->open = false;
    }
}

void meeting_notes_detail(const meeting_notes_t *notes, char *title, size_t title_cap, char *when, size_t when_cap,
                          char *conclusions, size_t conclusions_cap, char *todos, size_t todos_cap)
{
    const meeting_note_t *note;
    size_t used = 0;
    if (!notes || !notes->open || notes->open_index >= notes->count) {
        copy_text(title, title_cap, "");
        copy_text(when, when_cap, "");
        copy_text(conclusions, conclusions_cap, "");
        copy_text(todos, todos_cap, "");
        return;
    }
    note = &notes->items[notes->open_index];
    copy_text(title, title_cap, note->title);
    format_day(note->issued, when, when_cap, 1);
    copy_text(conclusions, conclusions_cap, note->conclusions[0] ? note->conclusions : "无明确结论。");
    if (!todos || todos_cap == 0) {
        return;
    }
    todos[0] = '\0';
    for (size_t i = 0; i < note->todo_count; i++) {
        int wrote = snprintf(todos + used, todos_cap - used, "%s%u  %s", i == 0 ? "" : "\n", (unsigned)(i + 1),
                             note->todos[i]);
        if (wrote < 0 || (size_t)wrote >= todos_cap - used) {
            break;
        }
        used += (size_t)wrote;
    }
}

bool meeting_notes_remove_open(meeting_notes_t *notes)
{
    meeting_note_t *note;
    if (!notes || !notes->open || notes->open_index >= notes->count) {
        return false;
    }
    note = &notes->items[notes->open_index];
    remember_removed(notes, note->id);
    for (size_t i = notes->open_index + 1; i < notes->count; i++) {
        notes->items[i - 1] = notes->items[i];
    }
    notes->count--;
    memset(&notes->items[notes->count], 0, sizeof(notes->items[0]));
    notes->open = false;
    return true;
}

static int flush_card(meeting_notes_t *notes, char *id, char *issued, char *title, char *conclusions, char todos[][72],
                      size_t todo_count)
{
    const char *todo_ptrs[MEETING_NOTES_TODO_CAP];
    int result;
    if (!id[0]) {
        return 0;
    }
    for (size_t i = 0; i < todo_count; i++) {
        todo_ptrs[i] = todos[i];
    }
    result = meeting_notes_merge_card(notes, id, issued, title, conclusions, todo_ptrs, todo_count);
    id[0] = issued[0] = title[0] = conclusions[0] = '\0';
    return result;
}

/* The card fields the server is asked for. */
static const char *const MEETING_NOTES_FIELDS[] = {
    "id ", "issued ", "title ", "conclusion ", "todo ",
};
#define MEETING_NOTES_FIELD_COUNT (sizeof(MEETING_NOTES_FIELDS) / sizeof(MEETING_NOTES_FIELDS[0]))

/* Find the field that starts at this position, if any.
 *
 * A field may sit at the start of a line or follow another field on the same
 * line. The server is free to answer with the whole card on one line, and a
 * parser that only looks at the start of a line would then find no card at
 * all. */
static int meeting_notes_field_at(const char *text, const char *start)
{
    for (size_t f = 0; f < MEETING_NOTES_FIELD_COUNT; f++) {
        size_t length = strlen(MEETING_NOTES_FIELDS[f]);
        if (strncmp(start, MEETING_NOTES_FIELDS[f], length) != 0) {
            continue;
        }
        if (start != text && start[-1] != ' ' && start[-1] != '\n') {
            continue;
        }
        return (int)f;
    }
    return -1;
}

int meeting_notes_merge_document(meeting_notes_t *notes, const char *document)
{
    char id[24] = "";
    char issued[20] = "";
    char title[72] = "";
    char conclusions[180] = "";
    char todos[MEETING_NOTES_TODO_CAP][72];
    size_t todo_count = 0;
    const char *cursor;
    if (!notes || !document) {
        return -1;
    }
    cursor = document;
    while (*cursor) {
        const char *line_end = strchr(cursor, '\n');
        size_t line_length = line_end ? (size_t)(line_end - cursor) : strlen(cursor);
        const char *line_limit = cursor + line_length;
        const char *scan = cursor;
        while (scan < line_limit) {
            int field = meeting_notes_field_at(cursor, scan);
            const char *value;
            const char *value_end;
            char value_text[192];
            size_t value_length;
            if (field < 0) {
                scan++;
                continue;
            }
            value = scan + strlen(MEETING_NOTES_FIELDS[field]);
            /* The value runs to the next field or to the end of the line. */
            value_end = value;
            while (value_end < line_limit) {
                if (meeting_notes_field_at(cursor, value_end) >= 0) {
                    break;
                }
                value_end++;
            }
            value_length = (size_t)(value_end - value);
            while (value_length > 0 && value[value_length - 1] == ' ') {
                value_length--;
            }
            if (value_length >= sizeof(value_text)) {
                value_length = sizeof(value_text) - 1;
            }
            memcpy(value_text, value, value_length);
            value_text[value_length] = '\0';
            switch (field) {
            case 0:
                /* A second id means a second card.
                 *
                 * The card is identified by its id, so the id that comes
                 * first in the document is the one that was read. The
                 * fields that followed it belong to it. */
                if (id[0]) {
                    if (flush_card(notes, id, issued, title, conclusions, todos, todo_count) != 0) {
                        return -1;
                    }
                    todo_count = 0;
                }
                copy_text(id, sizeof(id), value_text);
                break;
            case 1:
                copy_text(issued, sizeof(issued), value_text);
                break;
            case 2:
                copy_text(title, sizeof(title), value_text);
                break;
            case 3:
                copy_text(conclusions, sizeof(conclusions), value_text);
                break;
            case 4:
                if (todo_count < MEETING_NOTES_TODO_CAP) {
                    copy_text(todos[todo_count], sizeof(todos[todo_count]), value_text);
                    todo_count++;
                }
                break;
            default:
                break;
            }
            scan = value_end;
        }
        if (!line_end) {
            break;
        }
        cursor = line_end + 1;
    }
    /* The last card has no id after it, so it is flushed here. */
    return flush_card(notes, id, issued, title, conclusions, todos, todo_count);
}

int meeting_notes_write(const meeting_notes_t *notes, char *out, size_t cap)
{
    size_t used = 0;
    if (!notes || !out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    for (size_t i = 0; i < notes->removed_count; i++) {
        int wrote = snprintf(out + used, cap - used, "removed %s\n", notes->removed[i]);
        if (wrote < 0 || (size_t)wrote >= cap - used) {
            return -1;
        }
        used += (size_t)wrote;
    }
    for (size_t i = 0; i < notes->count; i++) {
        const meeting_note_t *note = &notes->items[i];
        int wrote = snprintf(out + used, cap - used, "id %s\nissued %s\ntitle %s\nconclusion %s\n", note->id,
                             note->issued, note->title, note->conclusions);
        if (wrote < 0 || (size_t)wrote >= cap - used) {
            return -1;
        }
        used += (size_t)wrote;
        for (size_t todo = 0; todo < note->todo_count; todo++) {
            wrote = snprintf(out + used, cap - used, "todo %s\n", note->todos[todo]);
            if (wrote < 0 || (size_t)wrote >= cap - used) {
                return -1;
            }
            used += (size_t)wrote;
        }
        wrote = snprintf(out + used, cap - used, "\n");
        if (wrote < 0 || (size_t)wrote >= cap - used) {
            return -1;
        }
        used += (size_t)wrote;
    }
    return 0;
}

int meeting_notes_read(meeting_notes_t *notes, const char *document)
{
    const char *cursor;
    if (!notes || !document) {
        return -1;
    }
    meeting_notes_init(notes);
    cursor = document;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        char line[96];
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (strncmp(cursor, "removed ", 8) == 0) {
            if (length >= sizeof(line)) {
                length = sizeof(line) - 1;
            }
            memcpy(line, cursor, length);
            line[length] = '\0';
            remember_removed(notes, line + 8);
        }
        if (!end) {
            break;
        }
        cursor = end + 1;
    }
    return meeting_notes_merge_document(notes, document);
}

/* The document buffer.
 *
 * The buffer was on the stack of the caller, and it was eight kilobytes of
 * it. The task that saves the minutes already uses most of its stack, and
 * the one that loads them at startup does the same on the stack of the main
 * task. The buffer belongs on the heap.
 *
 * The size is derived from the data it holds, so it cannot fall behind the
 * structure. A note carries an id, a time, a title, a conclusion and up to
 * four to-do lines, and every one of them is written on a line of its own
 * with its name in front of it. The removed ids are written the same way.
 */
#define MEETING_NOTES_LINE(id_len) (sizeof("id ") + (id_len) + 1)
#define MEETING_NOTES_BUFFER_SIZE                                                        \
    (MEETING_NOTES_CAP *                                                                  \
         (MEETING_NOTES_LINE(sizeof(((meeting_note_t *)0)->id)) +                         \
          MEETING_NOTES_LINE(sizeof(((meeting_note_t *)0)->issued)) +                      \
          MEETING_NOTES_LINE(sizeof(((meeting_note_t *)0)->title)) +                       \
          MEETING_NOTES_LINE(sizeof(((meeting_note_t *)0)->conclusions)) +                 \
          MEETING_NOTES_TODO_CAP *                                                        \
              (sizeof("todo ") + sizeof(((meeting_note_t *)0)->todos[0]) + 1) +             \
          1) +                                                                            \
     MEETING_NOTES_REMOVED_CAP * (sizeof("removed ") + sizeof(((meeting_notes_t *)0)->removed[0]) + 1) + \
     256)

int meeting_notes_load(meeting_notes_t *notes, const char *path)
{
    FILE *file;
    char *buffer;
    size_t count;
    int result;
    if (!notes || !path) {
        return -1;
    }
    file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    buffer = malloc(MEETING_NOTES_BUFFER_SIZE);
    if (!buffer) {
        fclose(file);
        return -1;
    }
    count = fread(buffer, 1, MEETING_NOTES_BUFFER_SIZE - 1, file);
    fclose(file);
    buffer[count] = '\0';
    result = meeting_notes_read(notes, buffer);
    free(buffer);
    return result;
}

int meeting_notes_save(const meeting_notes_t *notes, const char *path)
{
    char *buffer;
    FILE *file;
    int result = -1;
    if (!notes || !path) {
        return -1;
    }
    buffer = malloc(MEETING_NOTES_BUFFER_SIZE);
    if (!buffer) {
        return -1;
    }
    if (meeting_notes_write(notes, buffer, MEETING_NOTES_BUFFER_SIZE) != 0) {
        free(buffer);
        return -1;
    }
    file = fopen(path, "w");
    if (!file) {
        free(buffer);
        return -1;
    }
    if (fputs(buffer, file) >= 0) {
        result = 0;
    }
    fclose(file);
    free(buffer);
    return result;
}
