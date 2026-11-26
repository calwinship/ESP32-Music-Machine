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
- You don't have to make these files. When a recording has none, `./sync-tunes` guesses where each play starts
  (play 1 from the first sustained sound, later plays from the change in tempo) and writes the file. It never changes
  a file that already exists.
- To check or fix the times by ear: `./mark "<title>"` (or `./mark "<title>" --all` to tap them all from the
  beginning). It asks before saving and keeps the icon and colour lines. Then run `./sync-tunes`.
- You can still edit a file by hand (play the recording in QuickTime and read the position); run `./sync-tunes` after.

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
