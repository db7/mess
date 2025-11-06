#include "nav.h"
#include "readq.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Structure to hold the OSC 8 data
typedef struct {
    char *link; // The hyperlink URL
    char *text; // The displayed text
} OSC8Pair;

// Global variables
int url_count    = 0;
int selected_url = -1;   // Tracks the currently selected URL index
OSC8Pair urls[MAX_URLS]; // Array to store OSC 8 URLs
int offset = 0;

/*******************************************************************************
 * link data structure and global variables
 ******************************************************************************/
bool handle_key_input(char key);
void display_selected_link(int fd, int idx);
bool
process_input(char *c, ssize_t nread)
{
    if (handle_key_input(*c)) {
        display_selected_link(STDOUT_FILENO, selected_url);
        return false;
    }
    (void)nread;
    return true;
}
// Display the selected link at the bottom of the master_fd
void
display_selected_link(int fd, int idx)
{
    write(fd, "\r\e[K", 4); // Clear line
    if (idx >= 0 && idx < url_count) {
        dprintf(fd, "=> %s: %s", urls[idx].text, urls[idx].link);
    }
}

// Handle key inputs like TAB, Shift-TAB, ESC, and update the selection
bool
handle_key_input(char key)
{
    if (key == '\t') { // TAB for next link
        selected_url = (selected_url + 1) % url_count;
    } else if (key == 9) { // Shift-TAB for previous link
        if (url_count == 1)
            return false;
        selected_url = (selected_url - 1 + url_count) % url_count;
    } else if (key == '\n' && selected_url != -1) {
        char cmd[1024];
        char *link = urls[selected_url % url_count].link;
        // if ((strncmp(link, "file://", 7) == 0)) {
        //   link = strstr(link + 7, "/");
        //   sprintf(cmd, "mdcat -p %s", link);
        // } else if ((strncmp(link, "https://", 8) == 0)) {
        //   sprintf(cmd, "lynx %s", link);
        // } else {
        //   sprintf(cmd, "less %s", link);
        // }
        //
        sprintf(cmd, "mess %s", link);
        system(cmd);
    } else if (key == '\e') { // ESC for exit or other handling
        if (selected_url == -1)
            return false;
        selected_url = -1;
    } else
        return false;
    return true;
}

/*******************************************************************************
 * link data structure and global variables
 ******************************************************************************/
OSC8Pair extract_osc8(const char *input);

ssize_t
process_output(struct readq *bq)
{
    const char *osc8_start = "\e]8";
    const char *osc8_end   = "\e]8;;\e\\";
    // int it = 0;
    while (bq->start != bq->end) {
        assert(bq->start < bq->end);
        const size_t left = bq->end - bq->start;
        const char *str   = (const char *)bq->buffer + bq->start;
        const char *end   = (const char *)bq->buffer + bq->end;

        // if it has no further OSC8 start, then consume to the end of buffer
        // actually should consider \e alone
        const char *start = strstr(str, osc8_start);
        if (start == NULL) {
            // check for \e] ie, potential osc8
            if (left >= 2 && strncmp(end - 2, "\e]", 2) == 0)
                bq->start = bq->end - 2;
            else if (left >= 1 && strncmp(end - 1, "\e", 1) == 0)
                bq->start = bq->end - 1;
            else
                bq->start = bq->end;
            break;
        }

        end = strstr(start, osc8_end);
        if (end == NULL) {
            bq->start += start - str;
            break;
        }

        OSC8Pair pair = extract_osc8(start);
        if (pair.link && pair.text) {
            urls[url_count++ % MAX_URLS] = pair;
        }
#if 0
        size_t sz = end + strlen(osc8_end) - start + 1;
        char *msg = malloc(sz);
        msg[sz - 1] = '\0';
        memcpy(msg, start, sz - 1);

        char *link = malloc(10024);
        sprintf(link, "%d: length=%ld, start=%ld, end=%ld: %s", it, sz, bq->start,
                bq->end, end + strlen(osc8_end));
        urls[url_count % MAX_URLS].text = msg;
        urls[url_count % MAX_URLS].link = link;
        url_count++;
        it++;
#endif

        bq->start += end + strlen(osc8_end) - str;
    }
    assert(bq->start <= bq->end);
    return bq->start;
}

// Function to extract OSC 8 link and text from an input string
OSC8Pair
extract_osc8(const char *input)
{
    const char *osc8_start    = "\e]8;";
    const char *osc8_end      = "\e]8;;\e\\";
    const char *osc8_link_end = "\e\\";
    OSC8Pair result           = {NULL, NULL};

    // Find the start of the OSC 8 sequence
    const char *link_start = strstr(input, osc8_start);
    if (link_start == NULL)
        return result;

    // Find the delimiter ';' for separating params and the link
    link_start = strchr(link_start + strlen(osc8_start), ';');
    if (link_start == NULL)
        return result;
    link_start++; // Move past ';'

    // Find the end of the link (delimited by '\e\\')
    const char *link_end = strstr(link_start, osc8_link_end);
    if (link_end == NULL)
        return result;

    // Extract the link
    size_t link_length = link_end - link_start;
    result.link        = malloc(link_length + 1);
    if (!result.link) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }
    strncpy(result.link, link_start, link_length);
    result.link[link_length] = '\0';

    // Find the end of the displayed text
    const char *text_start = link_end + strlen(osc8_link_end);
    const char *text_end   = strstr(text_start, osc8_end);
    if (text_end == NULL)
        return result;

    // Extract the text
    size_t text_length = text_end - text_start;
    result.text        = malloc(text_length + 1);
    if (!result.text) {
        perror("malloc failed");
        free(result.link);
        exit(EXIT_FAILURE);
    }
    strncpy(result.text, text_start, text_length);
    result.text[text_length] = '\0';

    // remove new lines from link text
#if 0
    char *x = result.text;
    while ((x = strchr(x, '\n'))) {
        *x = ' ';
        x++;
    }
    x = result.text;
    while ((x = strchr(x, '\r'))) {
        *x = ' ';
        x++;
    }
#endif

    return result;
}

void
print_links(void)
{
    for (int i = 0; i < url_count; i++) {
        printf("=> %s: %s\n", urls[i].text, urls[i].link);
    }
}
