# batsc: a tiny bats interpreter for the jq track

`batsc.c` is a single-file C program that runs Exercism's jq-track `.bats`
test files without bash or bats installed. It is aimed at minimal
environments that only have a C compiler and `jq`.

## Build

```sh
cc -O2 -o batsc batsc.c -lm
```

## Run

Each exercise lives in its own folder with `test.bats` and a sample
solution named as the test expects. Two are included: `two-fer` (the
simplest shape) and `regular-chatbot` (multi-line jq programs, `<<<`
here-strings, `include`, and `${#lines[@]}`).

Run from inside the exercise folder:

```sh
cd two-fer
BATS_RUN_SKIPPED=true ../batsc test.bats > results.json

cd ../regular-chatbot
BATS_RUN_SKIPPED=true ../batsc test.bats | jq .
```

Setting `BATS_RUN_SKIPPED=true` runs every test. Without it, tests guarded
by `[[ $BATS_RUN_SKIPPED == "true" ]] || skip` are reported as skipped,
matching bats behaviour.

The exit code is 0 when all tests pass and 1 otherwise. Output is an
Exercism results.json (version 2) on stdout.

## How it works

1. Scans the file for `@test 'name' {` ... `}` blocks. `load` lines and
   comments are ignored.
2. Splits each line into words with shell quoting rules: single quotes,
   double quotes with `$var` / `${lines[N]}` expansion, backslashes, and
   `<< 'TAG'` heredocs.
3. `run` forks and execs the command directly (no `/bin/sh`), feeds the
   heredoc to stdin, and captures `$output`, `$status` and `${lines[]}`.
4. Assertions are built-ins compared against that captured state.

## Supported statements

- `run CMD ...` with an optional `<< TAG` heredoc, `<<< 'string'` here-string,
  or `< file` stdin redirect. Arguments may span lines inside quotes or via a
  trailing backslash, and `$'...'` ANSI-C quoting is understood.
- `${#lines[@]}` for the number of output lines
- `VAR='...'` and `VAR="..."` assignments
- `VAR=$(CMD << TAG ... TAG )` command substitution over a heredoc, as used
  for multi-line expected output (`cat`, `jq -c .`, and so on)
- `skip` and `... || skip`
- `assert_success`, `assert_failure [STATUS]`
- `assert_equal A B`
- `assert_output [--partial] STR`, `refute_output [--partial] STR`
- `assert_line [--index N] STR`

And the extra assertions from the track's `bats-jq.bash`:

- `assert_objects_equal JSON JSON` (compared via `jq`, so key order is ignored)
- `assert_float [-d N] [--] A B` (equal after truncating to N decimals, default 2)
- `assert_key_value KEY VALUE` (looks up KEY in the JSON `$output`)

As in `bats-jq.bash`, stderr lines starting with `["DEBUG:",` are treated as
diagnostics and dropped from `$output`.

## Coverage

Running the example solutions for every exercise in the jq track
(77 exercises, 1029 tests) through `batsc` passes them all.

Anything else inside a test marks it as `error` rather than passing silently.

## Known limits

- The heredoc is written to the child before its output is read, so a very
  large input combined with very large output could deadlock.
- Unquoted heredoc tags (which enable `$var` expansion in the body) are
  treated the same as quoted ones.
- There is no per-test timeout yet.
