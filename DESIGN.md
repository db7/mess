# mess design notes

The program runs in two major configurations that share low-level modules
(`links`, `nav`, `offscr`, `pager`, `dispatcher`, etc.) but differ on who drives
standard input.

## Pager mess (wrapper mode)

This is the primary mode and the one most users experience when they run
`... | mess` or set `PAGER=mess`. Key behaviors:

- wrapper scope: the process only reads from its stdin; it never seeks or
  reopens files on disk.
- subpager lifecycle: `mess` spawns the configured pager (`less -R` fallback)
  inside a pseudo-terminal, wiring the PTY master to its own stdout/stderr while
  letting the child inherit stdin directly. The parent proxies bytes between the
  user TTY and the child until a navigation hotkey is detected.
- navigation trigger — the proxy loop inspects keystrokes for Tab. Once it is
  seen the wrapper pauses the PTY traffic and enters navigation mode.
- screen capture — in navigation mode the wrapper sends `Ctrl-L` to the child
  pager to force a redraw, then uses the `offscr` component to capture the PTY
  output. `offscr` uses inter-byte timeouts to decide when the dump is complete
  so the wrapper knows when it has a full snapshot. While this happens no data
  flows between the pager and the real terminal, effectively freezing the child.
- link harvesting: the captured buffer is parsed by the `links` component to
  find OSC8 spans and manual cross references. The `nav` layer drives selection
  state using keystrokes read directly from the TTY, paints highlights on top of
  the cached `offscr` buffer, and keeps the PTY frozen so the on-screen content
  matches what the user navigates.
- link activation: pressing Enter while a link is selected calls back into the
  dispatcher code path so that helper programs (browsers, Markdown renderers,
  `man`, etc.) run outside the wrapper. Markdown targets are rendered and paged
  through a nested `mess`; when that child pager exits, the dispatcher returns
  and the parent pager resumes. Pressing `e` opens the current document in the
  configured editor when `MESSFILE` is known.
- exit: leaving navigation mode (Esc, `Ctrl-L`, or after the dispatcher
  returns) sends another `Ctrl-L` to the child pager and resumes the byte proxy
  loop so input and output continue to flow through the PTY uninterrupted.

In short, pager mode is a transparent shim around an existing pager with a
suspendable navigation overlay that relies on `offscr` snapshots and link
parsing to provide context-aware hotkeys.

## Dispatcher mess (launcher mode)

This mode runs when the user passes a target argument or `--open`. It never
touches stdin and instead decides which helper to spawn:

- `man://` topics and manual files go through the system `man`, with `MANPAGER`
  set so downstream invocations still reach `mess -m`.
- Markdown files run through the configured renderer (`MESS_MDRENDER`,
  defaulting to `mdcat` with a fallback to `lowdown`) and the renderer’s
  output is piped into `mess -m -o`, so link navigation stays active while the
  wrapper still honors `MESS_PAGER` for its downstream pager.
- Regular files are opened on stdin through the same wrapper path. Everything
  else forwards to the platform browser (`open`/`xdg-open`).

Dispatcher mode is intentionally thinner: it prepares data for the real pager
and hands control off, while pager mode is responsible for PTY juggling,
snapshotting, and interactive navigation.
