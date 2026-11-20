# Recording the demo

There is no `demo.mp4` in this repository yet, and the README says so. This page is the script for shooting it in one pass, about four minutes of work including the trim.

The recording was attempted on the machine that wrote this file and came out unusable: `screencapture -V 60` wrote a file whose first forty-five seconds are live and whose remaining fifteen repeat a single frame. The terminal underneath kept running, so the compiler did the work and the video did not show it. Window-scoped capture (`-l <window_id>`), full-screen capture, and capture with `caffeinate` holding the machine awake all produced the same freeze. Whatever the cause, a video that stops before the last command is worse than no video, so none was committed.

## What to record

One terminal window, no other windows visible, dark or light theme, at least 100 columns. The whole demo is the shell: no slides, no narration, no cursor flourishes.

The script below runs in about 35 seconds with its built-in pauses, which leaves room to read each output before the next command. A 60 second capture of it needs no editing beyond trimming the lead-in.

## The script

Save as `demo.sh` in the repository root and run `sh demo.sh`. It prints each command before running it, so the video reads as a transcript of a session rather than a series of cuts.

```sh
#!/bin/sh
# Sixty second LumiComp demo. Run from the repository root.

set -eu

CC=./build/bin/C-Compiler
PAUSE=2.4

step() {
    printf '\n$ %s\n' "$1"
    sleep "$PAUSE"
    eval "$1"
    sleep "$PAUSE"
}

printf 'LumiComp: a small C compiler in C, every stage readable on its own\n'
sleep 3

step "wc -l src/ccompiler/*.c include/ccompiler/*.h | tail -1"

step "$CC check examples/feature_showcase.c"

step "$CC codegen examples/asm_fib.c | head -12"

step "$CC --target=x86_64 codegen examples/asm_fib.c | sed -n '/^_fib:/,/^\.Lfib\.done/p' | head -12"

step "$CC build examples/asm_fib.c fib && ./fib"

rm -f fib
sleep 6
```

Build first. The script assumes `build/bin/C-Compiler` exists and does not build it, because a make log in the middle of a demo is thirty seconds of scrolling that says nothing.

```sh
make
```

## Expected output

This is the real output of that script on a clean build. Your line count will differ by a few lines as the code changes; the rest should match.

```text
LumiComp: a small C compiler in C, every stage readable on its own

$ wc -l src/ccompiler/*.c include/ccompiler/*.h | tail -1
   14002 total

$ ./build/bin/C-Compiler check examples/feature_showcase.c
semantic analysis succeeded
functions: 2
globals: 0
typedefs: 1

$ ./build/bin/C-Compiler codegen examples/asm_fib.c | head -12
declare printf(format: const char *, ...) -> int
func fib(n: int) -> int
entry:
  t0 = lt n, 2
  cjump t0, if.then.0, if.end.1
if.then.0:
  ret n
  jump if.end.1
if.end.1:
  t1 = sub n, 1
  t2 = call fib(t1)
  t3 = sub n, 2

$ ./build/bin/C-Compiler --target=x86_64 codegen examples/asm_fib.c | sed -n '/^_fib:/,/^\.Lfib\.done/p' | head -12
_fib:
  pushq %rbp
  movq %rsp, %rbp
  subq $64, %rsp
  movl %edi, -8(%rbp)
.Lfib.entry:
  movslq -8(%rbp), %rax
  movabsq $2, %rcx
  cmpq %rcx, %rax
  setl %al
  movzbq %al, %rax
  movl %eax, -16(%rbp)

$ ./build/bin/C-Compiler build examples/asm_fib.c fib && ./fib
fib(0)=0
fib(1)=1
fib(2)=1
fib(3)=2
fib(4)=3
fib(5)=5
fib(6)=8
fib(7)=13
fib(8)=21
fib(9)=34
```

The last block is the point of the video. A recursive function, compiled from the front end through the IR to x86-64 assembly, links with the system `cc`, and prints the sequence. If the recording does not end on `fib(9)=34`, it is not finished.

## Shooting it

Record the window rather than the screen, so nothing else on the desktop can end up in the file:

```sh
screencapture -v -l"$(osascript -e 'tell app "Terminal" to id of window 1')" demo.mov
```

Run `demo.sh` once beforehand to warm the binary cache. Then start the capture, switch to the terminal, run `sh demo.sh`, and stop the capture when the script exits.

## Checking the take before committing it

Do not trust the file's duration. Pull frames at fixed timestamps and look at them, because a frozen tail still reports the full length:

```sh
for t in 5 12 20 28 34 38; do
    ffmpeg -loglevel error -ss "$t" -i demo.mov -frames:v 1 "frame-$t.png" -y
done
```

The frames must differ from each other, and the last one must show `fib(9)=34`. If the tail repeats, the capture froze and the take is dead.

Trim to the session and keep the file small:

```sh
ffmpeg -i demo.mov -c:v libx264 -pix_fmt yuv420p -crf 28 -an demo.mp4
```

## Putting it in the README

Commit `demo.mp4` to `docs/images/`, then add this where the README introduces the compiler, replacing the paragraph that says there is no recording:

```markdown
https://github.com/user-attachments/assets/<id>
```

GitHub renders an uploaded video from a plain URL on its own line. Upload `demo.mp4` through the issue or pull request composer to get that URL, then paste it into the README. Delete the sentence that explains why there is no video yet.
