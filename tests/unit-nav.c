// RUN: %x
#include "nav.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct nav_case_result {
    nav_result_t result;
    size_t input_start;
    char *output;
};

static const char two_links_[] =
    "\x1b]8;;https://first.example\x1b\\First\x1b]8;;\x1b\\\n"
    "plain text\n"
    "\x1b]8;;https://second.example\x1b\\Second\x1b]8;;\x1b\\\n";

static void
read_all_(int fd, char *buf, size_t cap)
{
    size_t len = 0;
    while (len + 1 < cap) {
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
}

static struct nav_case_result
run_nav_case_(const char *view_data, const char *keys, int parse_flags)
{
    assert(setenv("MESS_STATUS_LINKS", "1", 1) == 0);

    int input_fd[2];
    int output_fd[2];
    assert(pipe(input_fd) == 0);
    assert(pipe(output_fd) == 0);

    struct readq rq;
    readq_init(&rq, input_fd[0]);
    size_t key_len = strlen(keys);
    assert(key_len < sizeof(rq.buffer));
    memcpy(rq.buffer, keys, key_len);
    rq.start          = 0;
    rq.end            = key_len;
    rq.buffer[rq.end] = '\0';

    struct offscr_view view = {
        .data = view_data,
        .len  = strlen(view_data),
    };
    struct nav *nav = nav_create(&(struct nav_opts){
        .parse_flags = parse_flags,
    });
    assert(nav != NULL);

    nav_result_t result = nav_run(nav, &view, &rq, output_fd[1], 80, true);
    nav_destroy(nav);
    close(output_fd[1]);

    char *output = calloc(1, 8192);
    assert(output != NULL);
    read_all_(output_fd[0], output, 8192);

    close(output_fd[0]);
    close(input_fd[0]);
    close(input_fd[1]);

    return (struct nav_case_result){
        .result      = result,
        .input_start = rq.start,
        .output      = output,
    };
}

static void
free_result_(struct nav_case_result *result)
{
    free(result->output);
    result->output = NULL;
}

static void
test_tab_selects_first_link(void)
{
    struct nav_case_result result =
        run_nav_case_(two_links_, "\t\x1b", PAGER_PARSE_OSC8);
    assert(result.result == NAV_RESULT_STOP);
    assert(strstr(result.output, "https://first.example") != NULL);
    free_result_(&result);
}

static void
test_l_moves_to_next_row_link(void)
{
    struct nav_case_result result =
        run_nav_case_(two_links_, "\tl\x1b", PAGER_PARSE_OSC8);
    assert(result.result == NAV_RESULT_STOP);
    assert(strstr(result.output, "https://second.example") != NULL);
    free_result_(&result);
}

static void
test_tab_does_not_group_last_with_first(void)
{
    const char view[] =
        "\x1b]8;;https://same.example\x1b\\First\x1b]8;;\x1b\\\n"
        "\x1b]8;;https://other.example\x1b\\Other\x1b]8;;\x1b\\\n"
        "\x1b]8;;https://same.example\x1b\\Last\x1b]8;;\x1b\\\n";
    struct nav_case_result result =
        run_nav_case_(view, "\t\x1b", PAGER_PARSE_OSC8);
    assert(result.result == NAV_RESULT_STOP);
    assert(strstr(result.output, "\x1b[7mFirst") != NULL);
    assert(strstr(result.output, "\x1b[7mLast") == NULL);
    free_result_(&result);
}

static void
test_ctrl_l_requests_refresh(void)
{
    struct nav_case_result result =
        run_nav_case_(two_links_, "\f", PAGER_PARSE_OSC8);
    assert(result.result == NAV_RESULT_REFRESH);
    assert(strstr(result.output, "\f") != NULL);
    free_result_(&result);
}

static void
test_q_requests_quit(void)
{
    struct nav_case_result result =
        run_nav_case_(two_links_, "q", PAGER_PARSE_OSC8);
    assert(result.result == NAV_RESULT_QUIT);
    free_result_(&result);
}

static void
test_unknown_key_exits_without_consuming(void)
{
    struct nav_case_result result =
        run_nav_case_(two_links_, "x", PAGER_PARSE_OSC8);
    assert(result.result == NAV_RESULT_STOP);
    assert(result.input_start == 0);
    free_result_(&result);
}

static void
test_j_without_further_link_exits_without_consuming(void)
{
    const char one_link[] =
        "\x1b]8;;https://only.example\x1b\\Only\x1b]8;;\x1b\\\n";
    struct nav_case_result result =
        run_nav_case_(one_link, "\tj", PAGER_PARSE_OSC8);
    assert(result.result == NAV_RESULT_STOP);
    assert(result.input_start == 1);
    free_result_(&result);
}

static void
test_man_references_are_selectable(void)
{
    const char view[] = "printf(3) reference\n";
    struct nav_case_result result =
        run_nav_case_(view, "\t\x1b", PAGER_PARSE_MAN);
    assert(result.result == NAV_RESULT_STOP);
    assert(strstr(result.output, "man://printf.3") != NULL);
    free_result_(&result);
}

int
main(void)
{
    test_tab_selects_first_link();
    test_l_moves_to_next_row_link();
    test_tab_does_not_group_last_with_first();
    test_ctrl_l_requests_refresh();
    test_q_requests_quit();
    test_unknown_key_exits_without_consuming();
    test_j_without_further_link_exits_without_consuming();
    test_man_references_are_selectable();
    puts("nav tests OK");
    return 0;
}
