#ifndef OSC8_SAMPLES_H
#define OSC8_SAMPLES_H

#ifndef UNUSED
#define UNUSED __attribute__((unused))
#endif

#ifndef OSC8_SAMPLE_SIMPLE_
static const char *const OSC8_SAMPLE_SIMPLE_ UNUSED =
    "\x1b]8;;https://example.com\x1b\\Example\x1b]8;;\x1b\\";
#endif

#ifndef OSC8_SAMPLE_WITH_PREFIX_
static const char *const OSC8_SAMPLE_WITH_PREFIX_ UNUSED =
    "prefix text "
    "\x1b]8;;https://example.org/docs\x1b\\Example Docs\x1b]8;;\x1b\\"
    " suffix";
#endif

#ifndef OSC8_SAMPLE_TWO_LINKS_
static const char *const OSC8_SAMPLE_TWO_LINKS_ UNUSED =
    "\x1b]8;;https://first.example\x1b\\First\x1b]8;;\x1b\\ "
    "\x1b]8;;https://second.example\x1b\\Second\x1b]8;;\x1b\\";
#endif

#ifndef OSC8_SAMPLE_SPLIT_PART1_
static const char *const OSC8_SAMPLE_SPLIT_PART1_ UNUSED =
    "\x1b]8;;https://split.test\x1b\\Split";
#endif

#ifndef OSC8_SAMPLE_SPLIT_PART2_
static const char *const OSC8_SAMPLE_SPLIT_PART2_ UNUSED =
    " Link\x1b]8;;\x1b\\";
#endif

#ifndef OSC8_SAMPLE_MALFORMED_
static const char *const OSC8_SAMPLE_MALFORMED_ UNUSED =
    "\x1b]8;;https://broken.example\x1b\\Broken without terminator";
#endif

#endif // OSC8_SAMPLES_H
