# fileward

Run a command whenever files change. One small C file, no dependencies beyond
libc and Linux inotify. Build it, drop the binary anywhere, done.

```
make            # builds ./fileward
```

## Usage

```
fileward [options] -- command [args...]

  -p PATH    watch PATH (file or directory); repeatable, at least one required
  -r         watch directories recursively (new subdirectories are picked up)
  -d MS      settle delay in milliseconds before running (default 300)
  -c         clear the screen before each run
  -k         kill a running command and restart it immediately on new changes
             (default: let it finish, then run once more if changes pile up)
  -s         run the command once at startup, before any change
  -q         quiet: suppress fileward's own status lines
  -h         help
```

Examples:

```
fileward -p main.c -- make
fileward -r -p src -p tests -- make test
fileward -r -s -c -p site -- sh -c 'python3 -m http.server & wait'   # shell features need sh -c
fileward -k -r -p . -- go run .                                      # restart a server on save
```

The command is executed directly, without a shell. For pipelines, redirection
or variables, wrap it: `sh -c '...'`.

A change only triggers a run after things go quiet for the settle delay, so a
save that writes five files runs the command once. If changes arrive while the
command is still running, the default is one more run after it finishes; `-k`
instead stops the current run and restarts immediately (useful for dev
servers). Editors that save by writing a temporary file and renaming it over
the original are handled: the watch re-arms on the replaced file.

## Tests

```
make test     # or: sh tests/test.sh
```

Eight end-to-end checks: basic trigger, burst settling, recursive pickup of
new subdirectories, atomic-replace saves, `-k` restart, `-s` startup run, and
usage errors.

## Limits

- Linux only (inotify). No macOS or BSD support (those need kqueue).
- Recursive watching uses one inotify watch per directory; very large trees
  can hit the per-user watch limit (`fs.inotify.max_user_watches`).
- An atomic replace has a short unarmed window between the rename and the
  re-added watch; the replace itself always triggers a run.
- Not a build system and not a daemon supervisor: it runs one command,
  in the foreground, until you stop it.

## Alternatives

fileward is not the first tool in this space and does not try to replace the
established ones:

- [entr](https://github.com/eradman/entr) - the classic; reads file lists on stdin, very mature.
- [watchexec](https://github.com/watchexec/watchexec) - feature-rich, cross-platform, Rust.
- [funzzy](https://github.com/cristianoliveira/funzzy) - YAML-configured task runner on file events.
- `inotifywait` from inotify-tools - the building block many people script around.

The point of fileward is to be small enough to read in one sitting and vendor
into a project: a single C99 file with no dependencies.

## License

MIT. See LICENSE.
