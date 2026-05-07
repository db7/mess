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

// Count how many times the fake link launcher was invoked.
static int launch_count_;
// Store the most recent URL handed to the fake launcher.
static char last_launched_[256];
// Count how many times the fake editor callback was invoked.
static int editor_count_;
// Keep the last editor path provided during tests.
static char last_editor_path_[256];

// Detect whether the highlighted rendering includes the expected text.
static bool
highlight_contains_text_(const char *rendered, const char *text)
{
    char needle[128];
    int n = snprintf(needle, sizeof(needle), "\x1b[7m%s\x1b[27m", text);
    assert(n > 0 && (size_t)n < sizeof(needle));
    return strstr(rendered, needle) != NULL;
}

// OSC8 payload containing duplicate and unique links.
static const char DUPLICATE_LINKS_SAMPLE_[] =
    "\x1b]8;;https://dup.example\x1b\\One\x1b]8;;\x1b\\ "
    "\x1b]8;;https://dup.example\x1b\\Two\x1b]8;;\x1b\\ "
    "\x1b]8;;https://unique.example\x1b\\Unique\x1b]8;;\x1b\\";

// OSC8 payload where all entries share the same URL.
static const char SINGLE_LINK_SAMPLE_[] =
    "\x1b]8;;https://only.example\x1b\\One\x1b]8;;\x1b\\ "
    "\x1b]8;;https://only.example\x1b\\Two\x1b]8;;\x1b\\ "
    "\x1b]8;;https://only.example\x1b\\Three\x1b]8;;\x1b\\";

// OSC8 payload designed to exercise grouped navigation cycling.
static const char CYCLE_SCENARIO_SAMPLE_[] =
    "\x1b]8;;https://l1.example\x1b\\L1 first\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l2.example\x1b\\L2 first\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l3.example\x1b\\L3 a\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l3.example\x1b\\L3 b\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l2.example\x1b\\L2 second\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l1.example\x1b\\L1 second\x1b]8;;\x1b\\ "
    "\x1b]8;;https://l4.example\x1b\\L4\x1b]8;;\x1b\\";

// Man-like output with colour sequences for link text.
static const char MAN_COLORED_SAMPLE_[] =
    "See \x1b[1;94mwordexp(3)\x1b[0m for details.\n";

// OSC8 sample containing colour codes inside the link text.
static const char OSC_COLORED_SAMPLE_[] =
    "\x1b]8;;https://color.example\x1b\\\x1b[1;93mfirst\x1b[0m\x1b]8;;\x1b\\ ";

// Fake launcher callback used to capture activations.
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

// Fake editor callback used to capture editor invocations.
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

// Seed the navigation buffer with a two-link OSC8 sample.
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

// Populate navigation state with duplicate/unique link groups.
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

// Load a buffer containing repeated single-link entries.
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

// Populate navigation state with a scenario exercising wraparound.
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

// Prepare a colored man page snippet for highlighting tests.
static void
prepare_colored_man_sample_(void)
{
    struct readq queue;
    readq_init(&queue, -1);
    size_t len = strlen(MAN_COLORED_SAMPLE_);
    assert(len < READQ_SIZE);
    memcpy(queue.buffer, MAN_COLORED_SAMPLE_, len);
    queue.start = 0;
    queue.end   = len;
    queue.buffer[len] = '\0';
    nav_process_output(&queue);
}

// Prepare a colored OSC8 snippet for highlighting tests.
static void
prepare_colored_osc_sample_(void)
{
    struct readq queue;
    readq_init(&queue, -1);
    size_t len = strlen(OSC_COLORED_SAMPLE_);
    assert(len < READQ_SIZE);
    memcpy(queue.buffer, OSC_COLORED_SAMPLE_, len);
    queue.start = 0;
    queue.end   = len;
    queue.buffer[len] = '\0';
    nav_process_output(&queue);
}

// Feed a single keypress through nav_process_input for convenience.
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

// Render highlighted output for assertions using the provided sample.
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

// Verify that Tab cycles through multiple links and wraps around.
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

// Ensure Shift-Tab handles escape sequences arriving in pieces.
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

// Confirm Enter triggers the active launcher and resets selection.
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

// Make sure regular keys bypass navigation when inactive.
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

// Check that Tab enters navigation mode even without stored links.
static void
test_tab_enters_mode_without_links_(void)
{
    nav_reset();
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);

    bool forwarded = nav_hit_('\t');
    assert(!forwarded);
    assert(nav_selected_index() == -1);
    assert(nav_mode_active());

    forwarded = nav_hit_('x');
    assert(!forwarded);

    forwarded = nav_hit_('\e');
    assert(!forwarded);
    assert(nav_selected_index() == -1);
    assert(!nav_mode_active());

    forwarded = nav_hit_('x');
    assert(forwarded);
}

// Ensure unmapped keys during navigation preserve the current selection.
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
    bool freeze_initial = nav_freeze_enabled();

    bool forwarded = nav_hit_('x');
    assert(!forwarded);
    assert(nav_selected_index() == 0);
    assert(nav_freeze_enabled() != freeze_initial);
    assert(!nav_status_visible());
}

// Verify 's' toggles the status overlay without leaving navigation mode.
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

    forwarded = nav_hit_('s');
    assert(!forwarded);
    assert(nav_status_visible());

    forwarded = nav_hit_('\t');
    assert(!forwarded);
    assert(nav_selected_index() == 1);
    assert(!nav_status_visible());
}

// Ensure cycling skips over duplicate URL groups.
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

// Confirm cycling works when all items share the same link.
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

// Check that contiguous duplicates highlight together.
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

// Validate group-based navigation order and highlighting.
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

// Verify coloured man-page tokens still produce clean links.
static void
test_man_colored_token_stripping_(void)
{
    nav_reset();
    nav_set_mode(NAV_MODE_MAN);
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    prepare_colored_man_sample_();

    assert(nav_link_count() == 1);
    const char *link = nav_link_at(0);
    const char *text = nav_text_at(0);
    assert(link && text);
    assert(strcmp(link, "man://wordexp.3") == 0);
    assert(strcmp(text, "wordexp(3)") == 0);

    (void)nav_hit_('\t');
    assert(nav_selected_index() == 0);

    char rendered[256];
    render_highlighted_sample_(MAN_COLORED_SAMPLE_, rendered, sizeof(rendered));
    assert(strstr(rendered, "\x1b[1;94m\x1b[7mwordexp(3)\x1b[27m\x1b[0m") != NULL);
}

// Verify highlighted OSC8 text preserves colour sequences.
static void
test_osc_colored_highlight_(void)
{
    nav_reset();
    nav_set_mode(NAV_MODE_OSC8);
    nav_set_launcher(fake_launcher_);
    nav_set_document_path(NULL);
    prepare_colored_osc_sample_();

    assert(nav_link_count() == 1);
    (void)nav_hit_('\t');
    assert(nav_selected_index() == 0);

    char rendered[256];
    render_highlighted_sample_(OSC_COLORED_SAMPLE_, rendered, sizeof(rendered));
    assert(strstr(rendered, "\x1b[7m\x1b[1;93m\x1b[7mfirst\x1b[0m\x1b[27m") != NULL);
}

// Ensure Escape leaves navigation mode without invoking launchers.
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

// Confirm 'v' opens the editor when a document path is configured.
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

// Ensure 'v' falls through when no document path exists.
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

// Verify 'v' exits selection mode after launching the editor.
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

// Ensure 'v' cancels selection even when no editor runs.
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
    test_tab_enters_mode_without_links_();
    test_regular_keys_keep_selection_();
    test_cycle_skips_duplicate_links_();
    test_cycle_handles_single_link_();
    test_highlight_contiguous_duplicates_();
    test_cycle_groups_sequence_();
    test_man_colored_token_stripping_();
    test_osc_colored_highlight_();
    test_escape_exits_selection_();
    test_v_opens_editor_when_document_set_();
    test_v_passes_through_without_document_();
    test_v_exits_selection_();
    test_v_exits_selection_without_document_();
    test_status_toggle_with_s_();
    puts("navigation tests OK");
    return 0;
}
