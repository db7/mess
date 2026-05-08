# Tour

This page is the entry point for a small set of Markdown files for trying
`mess` link navigation by hand.

Build `mess` from the repository root, then open this page:

```
make all
./mess tour/README.md
```

Press `Tab` to enter link navigation, move across links, and press `Enter` on a
Markdown link. When you quit a child pager, you should return to this page.

## Things to try

- Visit the [nested notes](notes/index.md) to test links in a subdirectory.
- Open [a deeper Markdown page](notes/reference.md), then quit it.
- Open [ls(1)](man://ls.1) to exercise manual dispatch.
- Open [example.com](https://example.com/) to exercise browser dispatch.
- Open [the project README](../README.md) to test a parent-directory link.

The next paragraph intentionally has links close together so horizontal
navigation has useful targets: [nested notes](notes/index.md),
[reference notes](notes/reference.md), [manual page](man://printf.3), and
[project README](../README.md).

This line has enough text to wrap in narrower terminals while keeping a link
near the end: after reading the sentence, jump to [reference notes](notes/reference.md).

Lorem ipsum dolor sit amet, consectetur adipiscing elit. Vestibulum commodo
felis vitae dolor tincidunt, sed congue neque dignissim. Pellentesque habitant
morbi tristique senectus et netus et malesuada fames ac turpis egestas, with a
late link to [nested notes](notes/index.md) after the line has had a chance to
wrap.
