#ifndef UNUSED
#define UNUSED __attribute__((unused))
#endif

#include "nav.h"
#include "readq.h"
#include "support/osc8_samples.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int launch_count_;
static char last_launched_[256];
static int editor_count_;
static char last_editor_path_[256];

static bool
highlight_contains_text_(const char *rendered, const char *text)
{
    char needle[128];
    int n = snprintf(needle, sizeof(needle), "\x1b[7m%s\x1b[27m", text);
    assert(n > 0 && (size_t)n < sizeof(needle));
    return strstr(rendered, needle) != NULL;
}

static const char DUPLICATE_LINKS_SAMPLE_[] =
    "\x1b]8;;https://dup.example\x1b\\One\x1b]8;;\x1b\\ "
    "\x1b]8;;https://dup.example\x1b\\Two\x1b]8;;\x1b\\ "
    "\x1b]8;;https://unique.example\x1b\\Unique\x1b]8;;\x1b\\";

static const char SINGLE_LINK_SAMPLE_[] =
    "\x1b]8;;https://only.example\x1b\\One\x1b]8;;\x1b\\ "
    "\x1b]8;;https://only.example\x1b\\Two\x1b]8;;\x1b\\ "
    "\x1b]8;;https://only.example\x1b\\Three\x1b]8;;\x1b\\";

static const char CYCLE_SCENARIO_SAMPLE_[] =
    "\x1b]8;;https://l1.example\x1b\\L1 first\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l2.example\x1b\\L2 first\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l3.example\x1b\\L3 a\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l3.example\x1b\\L3 b\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l2.example\x1b\\L2 second\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l1.example\x1b\\L1 second\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l4.example\x1b\\L4\x1b]8;;\x1b\\";

static int
fake_launcher_(const char *link, const char *text)
{
    (void)text;
    launch_count_++;
    if (link) {
        strncpy(last_launched_, link, sizeof(last_launched_) - 1);
        last_launched_[sizeof(last_launched_) - 1] = '\0';
    } else {
        last_launched_[0] = '\0';
    }
    return 0;
}

static int
fake_editor_(const char *path)
{
    editor_count_++;
    if (path) {
        strncpy(last_editor_path_, path, sizeof(last_editor_path_) - 1);
        last_editor_path_[sizeof(last_editor_path_) - 1] = '\0';
    } else {
        last_editor_path_[0] = '\0';
    }
    return 0;
}

static void
prepare_links_(void)
{
    struct readq queue;
    readq_init(&queue, -1);
    size_t len = strlen(OSC8_SAMPLE_TWO_LINKS_);
    assert(len < READQ_SIZE);
    memcpy(queue.buffer, OSC8_SAMPLE_TWO_LINKS_, len);
    queue.start = 0;
    queue.end = len;
    queue.buffer[len] = '\0';
    nav_process_output(&queue);
}

static void
prepare_duplicate_links_(void)
{
    struct readq queue;
    readq_init(&queue, -1);
    size_t len = strlen(DUPLICATE_LINKS_SAMPLE_);
    assert(len < READQ_SIZE);
    memcpy(queue.buffer, DUPLICATE_LINKS_SAMPLE_, len);
    queue.start = 0;
    queue.end = len;
    queue.buffer[len] = '\0';
    nav_process_output(&queue);
}

static void
prepare_single_link_list_(void)
{
    struct readq queue;
    readq_init(&queue, -1);
    size_t len = strlen(SINGLE_LINK_SAMPLE_);
    assert(len < READQ_SIZE);
    memcpy(queue.buffer, SINGLE_LINK_SAMPLE_, len);
    queue.start = 0;
    queue.end = len;
    queue.buffer[len] = '\0';
    nav_process_output(&queue);
}

static void
prepare_cycle_scenario_(void)
{
    struct readq queue;
    readq_init(&queue, -1);
    size_t len = strlen(CYCLE_SCENARIO_SAMPLE_);
    assert(len < READQ_SIZE);
    memcpy(queue.buffer, CYCLE_SCENARIO_SAMPLE_, len);
    queue.start = 0;
    queue.end = len;
    queue.buffer[len] = '\0';
    nav_process_output(&queue);
}

static bool
nav_hit_(char ch)
{
    char buf = ch;
    ssize_t len = 1;
    bool forward = nav_process_input(&buf, &len, 1);
    if (forward)
        assert(len == 1);
    else
        assert(len == 0);
    return forward;
}

static void
render_highlighted_sample_(const char *sample, char *out, size_t cap)
{
    size_t input_len = strlen(sample);
    size_t written   = nav_render_highlighted(sample, input_len, out, cap);
    assert(written > 0);
    if (written >= cap)
        written = cap - 1;
    out[written] = '\0';
}

static void
test_tab_cycles_links_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    launch_count_ = 0;
    last_launched_[0] = '\0';
    prepare_links_();

    bool forwarded = nav_hit_('\t');
    assert(!forwarded);
    assert(nav_selected_index() == 0);

    forwarded = nav_hit_('\t');
    assert(!forwarded);
    assert(nav_selected_index() == 1);

    forwarded = nav_hit_('\t');
    assert(!forwarded);
    assert(nav_selected_index() == 0);
}

static void
test_shift_tab_handles_split_sequence_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    launch_count_ = 0;
    prepare_links_();
    assert(nav_selected_index() == -1);

    int pipefd[2];
    assert(pipe(pipefd) == 0);
    nav_set_input_fd(pipefd[0]);

    const char rest[2] = {'[', 'Z'};
    ssize_t written = write(pipefd[1], rest, 2);
    assert(written == 2);

    char buf[4] = {'\x1b', 0, 0, 0};
    ssize_t len = 1;
    bool forwarded = nav_process_input(buf, &len, sizeof(buf));
    assert(!forwarded);
    assert(len == 0);
    assert(nav_selected_index() == 1);

    close(pipefd[0]);
    close(pipefd[1]);
    nav_set_input_fd(-1);
}

static void
test_enter_triggers_launcher_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    launch_count_ = 0;
    last_launched_[0] = '\0';
    prepare_links_();

    (void)nav_hit_('\t');
    (void)nav_hit_('\t');
    assert(nav_selected_index() == 1);

    bool forwarded = nav_hit_('\n');
    assert(!forwarded);
    assert(launch_count_ == 1);
    assert(strcmp(last_launched_, "https://second.example") == 0);
    assert(nav_selected_index() == -1);
}

static void
test_regular_keys_pass_through_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    launch_count_ = 0;
    last_launched_[0] = '\0';

    bool forwarded = nav_hit_('x');
    assert(forwarded);
    assert(nav_selected_index() == -1);
}

static void
test_regular_keys_keep_selection_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    prepare_links_();

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 0);
    assert(!nav_status_visible());

    bool forwarded = nav_hit_('x');
    assert(!forwarded);
    assert(nav_selected_index() == 0);
    assert(nav_status_visible());
}

static void
test_status_toggle_with_s_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    prepare_links_();

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 0);
    assert(!nav_status_visible());

    bool forwarded = nav_hit_('s');
    assert(!forwarded);
    assert(nav_status_visible());

    forwarded = nav_hit_('x');
    assert(!forwarded);
    assert(nav_selected_index() == 0);
    assert(nav_status_visible());

    forwarded = nav_hit_('s');
    assert(!forwarded);
    assert(nav_status_visible());

    forwarded = nav_hit_('\t');
    assert(!forwarded);
    assert(nav_selected_index() == 1);
    assert(!nav_status_visible());
}

static void
test_cycle_skips_duplicate_links_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    prepare_duplicate_links_();

    bool forwarded = nav_hit_('\t');
    assert(!forwarded);
    assert(nav_selected_index() == 0);
    const char *first = nav_link_at(nav_selected_index());
    assert(first != NULL);

    forwarded = nav_hit_('\t');
    assert(!forwarded);
    assert(nav_selected_index() == 2);
    const char *second = nav_link_at(nav_selected_index());
    assert(second != NULL);
    assert(strcmp(first, second) != 0);

    forwarded = nav_hit_('\t');
    assert(!forwarded);
    assert(nav_selected_index() == 0);
    const char *third = nav_link_at(nav_selected_index());
    assert(third != NULL);
    assert(strcmp(second, third) != 0);
}

static void
test_cycle_handles_single_link_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    prepare_single_link_list_();

    bool forwarded = nav_hit_('\t');
    assert(!forwarded);
    int first_idx = nav_selected_index();
    assert(first_idx == 0);
    const char *first = nav_link_at(first_idx);

    forwarded = nav_hit_('\t');
    assert(!forwarded);
    int second_idx = nav_selected_index();
    assert(second_idx == first_idx);
    const char *second = nav_link_at(second_idx);
    if (first && second)
        assert(strcmp(first, second) == 0);
    else
        assert(first == second);
}

static void
test_highlight_contiguous_duplicates_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    prepare_duplicate_links_();

    (void)nav_hit_('\t');
    char rendered[512];
    render_highlighted_sample_(DUPLICATE_LINKS_SAMPLE_, rendered, sizeof(rendered));
    assert(strstr(rendered, "\x1b[7mOne\x1b[27m") != NULL);
    assert(strstr(rendered, "\x1b[7mTwo\x1b[27m") != NULL);
    assert(strstr(rendered, "\x1b[7mUnique\x1b[27m") == NULL);
}

static void
test_cycle_groups_sequence_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    prepare_cycle_scenario_();

    char rendered[1024];

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 0);
    render_highlighted_sample_(CYCLE_SCENARIO_SAMPLE_, rendered, sizeof(rendered));
    assert(highlight_contains_text_(rendered, "L1 first"));
    assert(!highlight_contains_text_(rendered, "L1 second"));

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 1);
    render_highlighted_sample_(CYCLE_SCENARIO_SAMPLE_, rendered, sizeof(rendered));
    assert(highlight_contains_text_(rendered, "L2 first"));
    assert(!highlight_contains_text_(rendered, "L2 second"));

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 2);
    render_highlighted_sample_(CYCLE_SCENARIO_SAMPLE_, rendered, sizeof(rendered));
    assert(highlight_contains_text_(rendered, "L3 a"));
    assert(highlight_contains_text_(rendered, "L3 b"));

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 4);
    render_highlighted_sample_(CYCLE_SCENARIO_SAMPLE_, rendered, sizeof(rendered));
    assert(highlight_contains_text_(rendered, "L2 second"));
    assert(!highlight_contains_text_(rendered, "L2 first"));

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 5);
    render_highlighted_sample_(CYCLE_SCENARIO_SAMPLE_, rendered, sizeof(rendered));
    assert(highlight_contains_text_(rendered, "L1 second"));
    assert(!highlight_contains_text_(rendered, "L1 first"));

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 6);
    render_highlighted_sample_(CYCLE_SCENARIO_SAMPLE_, rendered, sizeof(rendered));
    assert(highlight_contains_text_(rendered, "L4"));

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 0);
    render_highlighted_sample_(CYCLE_SCENARIO_SAMPLE_, rendered, sizeof(rendered));
    assert(highlight_contains_text_(rendered, "L1 first"));
}

static void
test_escape_exits_selection_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    launch_count_ = 0;
    last_launched_[0] = '\0';
    prepare_links_();

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 0);

    bool forwarded = nav_hit_('\e');
    assert(!forwarded);
    assert(nav_selected_index() == -1);
}

static void
test_v_opens_editor_when_document_set_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_editor_launcher(fake_editor_);
    nav_set_document_path("docs/file.md");
    editor_count_ = 0;
    last_editor_path_[0] = '\0';

    bool forwarded = nav_hit_('v');
    assert(!forwarded);
    assert(editor_count_ == 1);
    assert(strcmp(last_editor_path_, "docs/file.md") == 0);
    nav_set_document_path(NULL);
}

static void
test_v_passes_through_without_document_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_editor_launcher(fake_editor_);
    nav_set_document_path(NULL);
    editor_count_ = 0;

    bool forwarded = nav_hit_('v');
    assert(forwarded);
    assert(editor_count_ == 0);
}

static void
test_v_exits_selection_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_editor_launcher(fake_editor_);
    nav_set_document_path("docs/file.md");
    editor_count_ = 0;
    last_editor_path_[0] = '\0';
    prepare_links_();

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 0);

    bool forwarded = nav_hit_('v');
    assert(!forwarded);
    assert(editor_count_ == 1);
    assert(nav_selected_index() == -1);
    assert(!nav_status_visible());
    nav_set_document_path(NULL);
}

static void
test_v_exits_selection_without_document_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_editor_launcher(fake_editor_);
    nav_set_document_path(NULL);
    editor_count_ = 0;
    prepare_links_();

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 0);

    bool forwarded = nav_hit_('v');
    assert(!forwarded);
    assert(editor_count_ == 0);
    assert(nav_selected_index() == -1);
    assert(!nav_status_visible());
}

int
main(void)
{
    test_tab_cycles_links_();
    test_shift_tab_handles_split_sequence_();
    test_enter_triggers_launcher_();
    test_regular_keys_pass_through_();
    test_regular_keys_keep_selection_();
    test_cycle_skips_duplicate_links_();
    test_cycle_handles_single_link_();
    test_highlight_contiguous_duplicates_();
    test_cycle_groups_sequence_();
    test_escape_exits_selection_();
    test_v_opens_editor_when_document_set_();
    test_v_passes_through_without_document_();
    test_v_exits_selection_();
    test_v_exits_selection_without_document_();
    test_status_toggle_with_s_();
    puts("navigation tests OK");
    return 0;
}
