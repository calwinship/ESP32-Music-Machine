# Play start times

One file per tune: `markers/<tune title>.txt`, where the title is the recording's file name without `.m4a`
(spaces at the ends don't matter, nor does upper/lower case). Each line is the time one play of the tune starts,
slowest first, as `m:ss.s` (or plain seconds). `#` starts a comment.

```
# Sporting Paddy
0:07.4
1:25.0
2:22.3
```

- Times must increase and fall inside the recording; `./sync-tunes` stops with the file and line number otherwise.
- At most 8 start times. A tune with no file plays from 0:00.
- When a file is missing, `./sync-tunes` proposes the start of play 1 and prints it. It writes the file only when you
  run `./sync-tunes --accept "<title>"` (or `--accept-all`), and it never changes a file that already exists:
  edit these by hand, then run `./sync-tunes` again to put the new times on the board.
- To find a time, play the recording in QuickTime and read the position when the play starts.

## Theme (optional)

Two more lines set how the tune looks on the board:

```
icon: leaf
colour: #2EC4B6
```

- `icon:` is a name from `tools/icons.txt` (54 to choose from). Without it the tune shows a music note.
- `colour:` is `#RRGGBB`; it tints the icon and the glow behind it. Without it the tune gets a bright colour picked
  from its title, the same every time.
- Run `./sync-tunes` after changing either.
