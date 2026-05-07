# Contributing

Thanks for helping improve `mess`. The project aims to keep the build strict,
portable across POSIX systems, and easy to validate locally. Please follow the
guidelines below when proposing changes.

## Prerequisites

- macOS, a BSD, or Linux with full POSIX userland utilities.
- A C11 compiler such as Clang 14+ or GCC 11+, plus `make`.
- Runtime helpers (`less -R`, `lynx`, `mdcat` or `lowdown`, `open` or
  `xdg-open`) available in `PATH`.
- The `tikl` binary to run `tests/dispatcher/*`.

## Workflow

1. Fork/clone the repository and create a topic branch.
2. Build everything with the default strict flags (see `Makefile`):

   ```
   make all
   ```

3. Run the full suite, including integration tests:

   ```
   make test
   ```

   Some PTY-driven tests need permission to become the controlling terminal; run
   them outside restrictive sandboxes when necessary.
4. Ensure your patch keeps the tree warning-free under `-Wall -Wextra -Werror`
   and add tests when fixing or introducing behavior.
5. Update documentation (`README.md`, `mess.1.in`, `DESIGN.md`) whenever the
   user experience or architecture changes.

## Coding style

- Stick to portable C11 and ASCII source.
- Existing code prefers small helper functions over deeply nested logic; follow
  that pattern and keep functions focused.
- Use the provided `make format` target before sending larger formatting
  changes.

## Licensing

All contributions are accepted under the terms in `LICENSE`. External tools that
`mess` shells out to remain under their respective licenses.
