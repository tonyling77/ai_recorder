#include "meeting_notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failed;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        g_failed++;
    }
}

static void an_empty_page_says_there_are_no_notes(void)
{
    meeting_notes_t notes;
    meeting_notes_init(&notes);
    expect(meeting_notes_count(&notes) == 0, "a new store has no notes");
    expect(strcmp(meeting_notes_empty_text(), "还没有纪要") == 0, "the empty page names itself");
}

static void newer_notes_are_listed_first(void)
{
    meeting_notes_t notes;
    char title[64];
    char meta[64];
    const char *one[] = {"整理录音"};
    const char *three[] = {"确认左滑", "整理录音", "约下次会议"};
    meeting_notes_init(&notes);
    expect(meeting_notes_merge_card(&notes, "old", "2026-11-09 09:00", "周会", "本周继续", one, 1) == 0,
           "the older note is stored");
    expect(meeting_notes_merge_card(&notes, "new", "2026-11-11 15:20", "产品评审会", "纪要放到设备上",
                                    three, 3) == 0,
           "the newer note is stored");
    meeting_notes_row(&notes, 0, title, sizeof(title), meta, sizeof(meta));
    expect(strcmp(title, "产品评审会") == 0, "the newest note is the first row");
    expect(strcmp(meta, "11月11日  3 条待办") == 0, "the row shows the date and the todo count");
    meeting_notes_row(&notes, 1, title, sizeof(title), meta, sizeof(meta));
    expect(strcmp(title, "周会") == 0, "the older note follows");
    expect(strcmp(meta, "11月9日  1 条待办") == 0, "a single-digit day drops the leading zero");
}

static void opening_a_note_shows_conclusions_and_todos(void)
{
    meeting_notes_t notes;
    char title[64];
    char when[64];
    char conclusions[128];
    char todos[128];
    const char *items[] = {"确认左滑进入纪要", "纪要写入 Obsidian 后下发"};
    meeting_notes_init(&notes);
    meeting_notes_merge_card(&notes, "1", "2026-11-11 15:20", "产品评审会", "纪要放到设备上阅读。", items, 2);
    expect(meeting_notes_open(&notes, 0), "the first row opens");
    meeting_notes_detail(&notes, title, sizeof(title), when, sizeof(when), conclusions, sizeof(conclusions), todos,
                         sizeof(todos));
    expect(strcmp(title, "产品评审会") == 0, "the detail keeps the title");
    expect(strcmp(when, "11月11日  15:20") == 0, "the detail shows the date and time");
    expect(strcmp(conclusions, "纪要放到设备上阅读。") == 0, "the detail shows the conclusion");
    expect(strcmp(todos, "1  确认左滑进入纪要\n2  纪要写入 Obsidian 后下发") == 0, "todos are numbered");
}

static void removing_a_note_keeps_it_off_the_device(void)
{
    meeting_notes_t notes;
    char stored[512];
    meeting_notes_t restored;
    const char *todos[] = {"整理录音"};
    meeting_notes_init(&notes);
    meeting_notes_merge_card(&notes, "1", "2026-11-11 15:20", "产品评审会", "结论", todos, 1);
    expect(meeting_notes_open(&notes, 0), "the note opens before removal");
    expect(meeting_notes_remove_open(&notes), "the open note can be removed");
    expect(meeting_notes_count(&notes) == 0, "the device list no longer has it");
    expect(!meeting_notes_is_open(&notes), "removal closes the detail");
    expect(meeting_notes_merge_card(&notes, "1", "2026-11-11 15:20", "产品评审会", "结论", todos, 1) == 0,
           "a later copy of the same note is ignored");
    expect(meeting_notes_count(&notes) == 0, "removal stays on the device");
    expect(meeting_notes_write(&notes, stored, sizeof(stored)) == 0, "the store can be written");
    meeting_notes_init(&restored);
    expect(meeting_notes_read(&restored, stored) == 0, "the store can be read back");
    expect(meeting_notes_count(&restored) == 0, "the removed note is still absent after restart");
    expect(meeting_notes_merge_card(&restored, "1", "2026-11-11 15:20", "产品评审会", "结论", todos, 1) == 0,
           "the restarted store still recognizes the removed note");
    expect(meeting_notes_count(&restored) == 0, "restart does not bring the removed note back");
}

static void a_document_adds_a_note(void)
{
    meeting_notes_t notes;
    char title[64];
    char meta[64];
    const char *document =
        "id 9\n"
        "issued 2026-11-10 08:30\n"
        "title 客户沟通\n"
        "conclusion 下周再确认范围\n"
        "todo 发送纪要\n"
        "todo 约下次会议\n";
    meeting_notes_init(&notes);
    expect(meeting_notes_merge_document(&notes, document) == 0, "a delivered document is accepted");
    expect(meeting_notes_count(&notes) == 1, "the document becomes one note");
    meeting_notes_row(&notes, 0, title, sizeof(title), meta, sizeof(meta));
    expect(strcmp(title, "客户沟通") == 0, "the document title is listed");
    expect(strcmp(meta, "11月10日  2 条待办") == 0, "the document todos are counted");
}

static void the_template_of_the_listing_makes_a_card(void)
{
    /* The template that the listing gives to the user of the Xiaozhi
     * channel. The agent of the user answers with this, and the board has
     * to read it as one card with two to-dos. The template is published,
     * so it is pinned here: a change of the format has to change this
     * text and the listing together. */
    meeting_notes_t notes;
    char title[64];
    char when[64];
    char conclusions[128];
    char todos[192];
    const char *document =
        "id 001\n"
        "issued 2026-11-17 14:30\n"
        "title 录音笔固件发布评审\n"
        "conclusion 分区布局按平台要求改到 0x210000；发布包已经可以上传\n"
        "todo 小李 11 月 18 日前把 Recovery 升到带 OTA 选槽修复的版本\n"
        "todo 小王 11 月 20 日前在社区页面上传固件并提交审核\n";
    meeting_notes_init(&notes);
    expect(meeting_notes_merge_document(&notes, document) == 0, "the template of the listing is accepted");
    expect(meeting_notes_count(&notes) == 1, "the template makes one card");
    expect(meeting_notes_open(&notes, 0), "the card of the template opens");
    meeting_notes_detail(&notes, title, sizeof(title), when, sizeof(when), conclusions, sizeof(conclusions), todos,
                         sizeof(todos));
    expect(strcmp(title, "录音笔固件发布评审") == 0, "the template title is read");
    expect(strcmp(when, "11月17日  14:30") == 0, "the template date and time are read");
    expect(strcmp(conclusions, "分区布局按平台要求改到 0x210000；发布包已经可以上传") == 0,
           "the template conclusion is read");
    expect(strcmp(todos, "1  小李 11 月 18 日前把 Recovery 升到带 OTA 选槽修复的版本\n"
                         "2  小王 11 月 20 日前在社区页面上传固件并提交审核") == 0,
           "the template to-dos are read and numbered");
}

static void the_fingerprint_says_whether_the_minutes_changed(void)
{
    /* The server sends the same minutes over and over, so the copy on the
     * flash may only be written when they really changed. */
    meeting_notes_t notes;
    const char *document =
        "id 9\n"
        "issued 2026-11-10 08:30\n"
        "title 客户沟通\n"
        "conclusion 下周再确认范围\n"
        "todo 发送纪要\n";
    const char *other =
        "id 9\n"
        "issued 2026-11-10 08:30\n"
        "title 客户沟通\n"
        "conclusion 下周就确认范围\n"
        "todo 发送纪要\n";
    unsigned before;

    meeting_notes_init(&notes);
    expect(meeting_notes_merge_document(&notes, document) == 0, "a document is accepted");
    before = meeting_notes_fingerprint(&notes);
    expect(before != 0, "the minutes have a fingerprint");
    expect(meeting_notes_merge_document(&notes, document) == 0, "the same document is accepted again");
    expect(meeting_notes_fingerprint(&notes) == before, "the same minutes leave the fingerprint alone");
    expect(meeting_notes_merge_document(&notes, other) == 0, "a changed document is accepted");
    expect(meeting_notes_fingerprint(&notes) != before, "changed minutes change the fingerprint");
}

static void the_minutes_survive_a_save_and_a_load(void)
{
    /* The buffer of the document was on the stack of the caller and is on
     * the heap now, so the round trip through the file is the check that it
     * still holds everything. */
    meeting_notes_t notes;
    meeting_notes_t loaded;
    char path[] = "/tmp/ai-recorder-notes-XXXXXX";
    const char *todos[] = {"整理录音", "确认左滑", "约下次会议"};
    expect(mkdtemp(path) != NULL, "temp directory");
    expect(meeting_notes_merge_card(&notes, "9001", "2026-11-11 15:20", "产品评审会", "纪要放到设备上",
                                    todos, 3) == 0,
           "the note is stored");
    expect(meeting_notes_save(&notes, "/tmp/ai-recorder-notes-file.txt") == 0, "the minutes are saved");
    expect(meeting_notes_load(&loaded, "/tmp/ai-recorder-notes-file.txt") == 0, "the minutes are loaded");
    expect(meeting_notes_count(&loaded) == 1, "the loaded minutes have the note");
    expect(meeting_notes_fingerprint(&loaded) == meeting_notes_fingerprint(&notes),
           "the loaded minutes are the same minutes");
    expect(meeting_notes_load(&loaded, "/tmp/ai-recorder-notes-missing.txt") != 0,
           "a missing file is refused");
    remove("/tmp/ai-recorder-notes-file.txt");
    rmdir(path);
}

static void the_notes_fit_in_the_buffer(void)
{
    /* The buffer is sized from the structure, so the worst case of the
     * document has to fit in it.
     *
     * The worst case is every note full: an id, a time, a title and a
     * conclusion at their longest, and the most to-do lines a note can
     * hold, each at its longest. On top of that the removed ids are in the
     * document too, so the worst case is the full notes and the full
     * removed list together.
     *
     * A note is only removed after it was there, so the removed ids and
     * the notes cannot both be full at the same time. The document is
     * written with the notes first, so the removed list is what grows the
     * document past the notes.
     *
     * The save has to succeed with all of it, and the file that comes out
     * has to hold the notes that were in. */
    meeting_notes_t notes;
    char path[] = "/tmp/ai-recorder-notes-full-XXXXXX";
    char issued[20];
    char title[72];
    char conclusions[180];
    const char *todos[MEETING_NOTES_TODO_CAP];
    char document[16384];
    memset(issued, '9', sizeof(issued) - 1);
    issued[sizeof(issued) - 1] = '\0';
    memset(title, 't', sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    memset(conclusions, 'c', sizeof(conclusions) - 1);
    conclusions[sizeof(conclusions) - 1] = '\0';
    for (int i = 0; i < MEETING_NOTES_TODO_CAP; i++) {
        static char todo[72];
        memset(todo, 'd', sizeof(todo) - 1);
        todo[sizeof(todo) - 1] = '\0';
        todos[i] = todo;
    }
    expect(mkdtemp(path) != NULL, "temp directory");
    meeting_notes_init(&notes);
    for (int i = 0; i < MEETING_NOTES_CAP; i++) {
        char one_id[24];
        snprintf(one_id, sizeof(one_id), "note-%d", i);
        expect(meeting_notes_merge_card(&notes, one_id, issued, title, conclusions, todos, MEETING_NOTES_TODO_CAP) == 0,
               "a full note is stored");
    }
    expect(meeting_notes_count(&notes) == MEETING_NOTES_CAP, "every note is stored");
    expect(meeting_notes_write(&notes, document, sizeof(document)) == 0, "the full document is written");
    expect(meeting_notes_save(&notes, "/tmp/ai-recorder-notes-full.txt") == 0, "the full minutes are saved");
    {
        meeting_notes_t loaded;
        expect(meeting_notes_load(&loaded, "/tmp/ai-recorder-notes-full.txt") == 0, "the full minutes are loaded");
        expect(meeting_notes_count(&loaded) == MEETING_NOTES_CAP, "the loaded minutes hold every note");
    }
    remove("/tmp/ai-recorder-notes-full.txt");
    rmdir(path);
}

static void a_document_holds_every_card(void)
{
    /* The card is identified by its id, and the fields of a card follow its
     * id. A document with more than one card used to keep only the last
     * one, because the reader only finished a card at the very end of the
     * text.
     *
     * The server sends the cards one after another, and the device writes
     * them back the same way, so both shapes have to be read. */
    const char *server[] = {
        "id A\nissued 2026-01-01 10:00\ntitle T1\nconclusion C1\ntodo dA1\n"
        "id B\nissued 2026-01-02 10:00\ntitle T2\nconclusion C2\ntodo dB1\ntodo dB2\n",
        "id A\nissued 2026-01-01 10:00\ntitle T1\nconclusion C1\ntodo dA1\n\n"
        "id B\nissued 2026-01-02 10:00\ntitle T2\nconclusion C2\ntodo dB1\ntodo dB2\n\n",
    };
    for (size_t shape = 0; shape < sizeof(server) / sizeof(server[0]); shape++) {
        meeting_notes_t notes;
        char title[64];
        char meta[64];
        expect(meeting_notes_read(&notes, server[shape]) == 0, "the document is read");
        expect(meeting_notes_count(&notes) == 2, "both cards are kept");
        meeting_notes_row(&notes, 0, title, sizeof(title), meta, sizeof(meta));
        expect(strcmp(title, "T2") == 0, "the newer card is the first row");
        meeting_notes_row(&notes, 1, title, sizeof(title), meta, sizeof(meta));
        expect(strcmp(title, "T1") == 0, "the older card is the second row");
        /* The to-do lines of a card belong to it. */
        {
            char conclusions[256];
            char todos[512];
            expect(meeting_notes_open(&notes, 0), "the newer card opens");
            meeting_notes_detail(&notes, title, sizeof(title), meta, sizeof(meta), conclusions, sizeof(conclusions),
                                 todos, sizeof(todos));
            expect(strstr(todos, "dB1") != NULL, "the newer card has its own to-do");
            expect(strstr(todos, "dA1") == NULL, "the older card's to-do did not leak into it");
            meeting_notes_close(&notes);
            expect(meeting_notes_open(&notes, 1), "the older card opens");
            meeting_notes_detail(&notes, title, sizeof(title), meta, sizeof(meta), conclusions, sizeof(conclusions),
                                 todos, sizeof(todos));
            expect(strstr(todos, "dA1") != NULL, "the older card has its own to-do");
            expect(strstr(todos, "dB1") == NULL, "the newer card's to-do did not leak into it");
        }
    }
}

int main(void)
{
    an_empty_page_says_there_are_no_notes();
    newer_notes_are_listed_first();
    opening_a_note_shows_conclusions_and_todos();
    removing_a_note_keeps_it_off_the_device();
    a_document_adds_a_note();
    the_template_of_the_listing_makes_a_card();
    the_fingerprint_says_whether_the_minutes_changed();
    the_minutes_survive_a_save_and_a_load();
    the_notes_fit_in_the_buffer();
    a_document_holds_every_card();
    if (g_failed) {
        fprintf(stderr, "%d meeting note checks failed\n", g_failed);
        return 1;
    }
    return 0;
}
