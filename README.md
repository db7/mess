mess -- more or less with links
===============================

`mess` a wrapper program that adds link navigation support to pagers, eg,
`less`. It watches the output of the underlying pager for [OSC8 sequences][OSC8]
and lets you tab through the links. Refer to `mess(1)` man page for command-line
flags, environment variables, and exit behavior. This README is focused on the
overview, workflows, and developer notes. See [DESIGN.md](DESIGN.md) for
architecture notes and [CONTRIBUTING.md](CONTRIBUTING.md) before sending
patches.

[OSC8]: https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda
[lynx]: https://lynx.browser.org/
[lowdown]: https://kristaps.bsd.lv/lowdown/

`mess` relies on external tools such as `less`, [`lynx`][lynx], and
[`lowdown`][lowdown], and targets POSIX environments where standard utilities
like `man`, `less`, and `wordexp(3)` are available.

## Requirements

- Operating systems: POSIX hosts such as macOS 12+, NetBSD 10+, and Linux
  distributions with glibc 2.31+ or musl 1.2+.
- Toolchain: a C11 compiler (Clang 14+ or GCC 11+), `make`, and the standard
  libc headers visible via `/usr/include`.
- Runtime helpers: `less -R` (or another pager), [`mdcat`](https://github.com/swsnr/mdcat)
  or [`lowdown`][lowdown] for Markdown rendering, and `lynx` plus a GUI opener
  such as `open`/`xdg-open`. The dispatcher uses whatever you export via
  `MESS_PAGER`, `BROWSER`, and `MESS_MDRENDER`.

## Quickstart

1. Install the prerequisites above on a supported macOS/NetBSD/Linux host.
2. Clone the repository and enter it: `git clone <repo> mess && cd mess`.
3. Build it: `make all`

## Highlights

- Tab through OSC8 hyperlinks emitted by downstream pagers without
  giving up normal pager key bindings.
- Detect manual cross references (`printf(3)`, `socket(2)`, etc.) automatically
  when reading `man` output or when `mess -m` is forced through `MANPAGER`.
- Launch URIs, Markdown files, and `man://` targets directly via the dispatcher,
  letting `mess` hand work to the Markdown renderer, `man`, or your configured
  browser.
- Drop into `$VISUAL`/`$EDITOR` with `e` from navigation mode.

## Usage

See `man mess` for every flag and environment knob; the quick tour:

```
mdcat README.md | mess             # page rendered Markdown
lowdown -tterm README.md | mess    # same idea using lowdown
mess README.md                     # dispatch a file through the wrapper
```

When the input is a man page (e.g. a `foo.1` file, `man://` link, or the output
of `man -P cat`), `mess` scans the rendered text for references such as
`printf(3)` or `socket(2)` and exposes them as navigation targets. Press `Tab`
to highlight those tokens and `Enter` to jump via the new `man://` helper.
Combine this with `MANPAGER='mess -m'` when you want `man` to feed directly into
mess's manual-aware navigation.

While the pager is active, pressing `Enter` opens the selected target through
the dispatcher. Markdown links are rendered and paged through `mess` as well;
when you quit that child pager, control naturally returns to the parent
document. You can also press `e` to open the current document in
`$VISUAL`/`$EDITOR` (falling back to `vi`). The dispatcher
(`mess <file-or-uri>`) sets `MESSFILE` automatically for that shortcut; wrapper
mode (`... | mess`) can opt in by exporting `MESSFILE` before launching the
pager.

## Design Overview

`mess` runs the downstream pager (`less` by default) behind a PTY and proxies
terminal input and output between the pager and the user. While proxying output,
it records OSC8 links and synthesized manual references together with their
rendered positions, so navigation can highlight the same text the pager shows.
When a target is activated, control returns to the dispatcher so browsers,
Markdown renderers, `man`, and nested `mess` pagers are launched outside the PTY
wrapper. The parent process also mirrors `SIGWINCH` into the child PTY so the
pager keeps its layout in sync with your terminal.

The off-screen capture helper gives navigation code a snapshot of child PTY
output when it needs to reason about rendered content without disturbing the
live pager.

## License

The code in this repository is released under the permissive terms described in
`LICENSE`. External tools that `mess` shells out to (`less`, `lynx`, `lowdown`,
`mdcat`, `tikl`, etc.) are separate binaries under their own licenses and are
not covered by this project's license file.
