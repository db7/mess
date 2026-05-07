// RUN: %x
#include "strbuf.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
test_init_and_append(void)
{
    struct strbuf sb = STRBUF_INIT;
    strbuf_init(&sb, 8);
    assert(strbuf_len(&sb) == 0);
    assert(strbuf_data(&sb) != NULL);
    assert(strbuf_append(&sb, "hi", 2) == 0);
    assert(strbuf_len(&sb) == 2);
    assert(strcmp(strbuf_data(&sb), "hi") == 0);
    strbuf_free(&sb);
}

static void
test_growth_and_bulk_append(void)
{
    struct strbuf sb   = STRBUF_INIT;
    const char chunk[] = "abcdefghijklmnopqrstuvwxyz";
    for (int i = 0; i < 200; ++i)
        assert(strbuf_append(&sb, chunk, sizeof(chunk) - 1) == 0);
    assert(strbuf_len(&sb) == (sizeof(chunk) - 1) * 200);
    const char *data = strbuf_data(&sb);
    assert(data != NULL);
    assert(strncmp(data, chunk, sizeof(chunk) - 1) == 0);
    strbuf_free(&sb);
}

static void
test_reset_preserves_buffer(void)
{
    struct strbuf sb = STRBUF_INIT;
    assert(strbuf_append_str(&sb, "12345") == 0);
    const char *before = strbuf_data(&sb);
    strbuf_reset(&sb);
    assert(strbuf_len(&sb) == 0);
    assert(before == strbuf_data(&sb));
    assert(strbuf_push(&sb, 'z') == 0);
    assert(strcmp(strbuf_data(&sb), "z") == 0);
    strbuf_free(&sb);
}

static void
test_append_str_null_safe(void)
{
    struct strbuf sb = STRBUF_INIT;
    assert(strbuf_append_str(&sb, NULL) == 0);
    assert(strbuf_len(&sb) == 0);
    assert(strbuf_push(&sb, 'a') == 0);
    assert(strbuf_append_str(&sb, NULL) == 0);
    assert(strbuf_len(&sb) == 1);
    strbuf_free(&sb);
}

static void
test_detach_transfers_ownership(void)
{
    struct strbuf sb = STRBUF_INIT;
    assert(strbuf_append_str(&sb, "detached") == 0);
    char *data = strbuf_detach(&sb);
    assert(data != NULL);
    assert(strcmp(data, "detached") == 0);
    assert(strbuf_data(&sb) == NULL);
    assert(strbuf_len(&sb) == 0);
    free(data);
    strbuf_free(&sb);
}

static void
test_reserve_overflow_fails(void)
{
    struct strbuf sb = STRBUF_INIT;
    sb.data          = malloc(1);
    assert(sb.data != NULL);
    sb.cap = 1;
    sb.len = SIZE_MAX - 2;
    assert(strbuf_reserve(&sb, 10) == -1);
    free(sb.data);
}

static void
test_append_zero_length_is_noop(void)
{
    struct strbuf sb = STRBUF_INIT;
    assert(strbuf_append(&sb, "data", 0) == 0);
    assert(strbuf_len(&sb) == 0);
    assert(strbuf_append(&sb, NULL, 0) == 0);
    strbuf_free(&sb);
}

int
main(void)
{
    test_init_and_append();
    test_growth_and_bulk_append();
    test_reset_preserves_buffer();
    test_append_str_null_safe();
    test_detach_transfers_ownership();
    test_reserve_overflow_fails();
    test_append_zero_length_is_noop();
    puts("strbuf tests OK");
    return 0;
}
