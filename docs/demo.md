# The demo

`docs/images/demo.mp4` is a 37 second recording of the pipeline running end to end. This page is how it was shot and how to shoot it again.

## What the video shows

The script prints each command before running it, so the video reads as a session rather than a series of cuts. Five commands, then a hold on the last line:

1. `wc -l` over the sources, which prints the size of the thing.
2. `check` on `examples/feature_showcase.c`, which prints what semantic analysis found.
3. `codegen` on `examples/asm_fib.c`, which prints the textual IR.
4. `--target=x86_64 codegen` on the same file, which prints x86-64 assembly for `fib`.
5. `build` and then run the binary, which prints `fib(0)=0` through `fib(9)=34`.

The last block is the point. If the recording does not end on `fib(9)=34`, it is not finished.

## The script

```sh
#!/bin/sh
set -eu
cd "$(git rev-parse --show-toplevel)"
CC=./build/bin/C-Compiler
step() {
    printf '\n$ %s\n' "$1"
    sleep 2.2
    eval "$1"
    sleep 2.0
}
sleep 4
printf 'LumiComp: a C compiler in C, every stage runnable on its own\n'
sleep 3
step "wc -l src/ccompiler/*.c include/ccompiler/*.h | tail -1"
step "$CC check examples/feature_showcase.c"
step "$CC codegen examples/asm_fib.c | head -9"
step "$CC --target=x86_64 codegen examples/asm_fib.c | sed -n '/^_fib:/,/^\.Lfib\.entry/p' | head -9"
step "$CC build examples/asm_fib.c fib && ./fib"
rm -f fib
sleep 30
```

Build first. The script assumes `build/bin/C-Compiler` exists and does not build it, because a make log in the middle of a demo is thirty seconds of scrolling that says nothing.

```sh
make
```

## Expected output

This is the real output of that script on a clean build. Your line count will differ by a few lines as the code changes; the rest should match.

```text
LumiComp: a C compiler in C, every stage runnable on its own

$ wc -l src/ccompiler/*.c include/ccompiler/*.h | tail -1
   14002 total

$ ./build/bin/C-Compiler check examples/feature_showcase.c
semantic analysis succeeded
functions: 2
globals: 0
typedefs: 1

$ ./build/bin/C-Compiler codegen examples/asm_fib.c | head -9
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

$ ./build/bin/C-Compiler --target=x86_64 codegen examples/asm_fib.c | sed -n '/^_fib:/,/^\.Lfib\.entry/p' | head -9
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

## How to shoot it

Two things matter, and the first one cost four dead takes.

**Do not pass `-V`.** `screencapture -V <seconds>` writes a file whose first forty-five seconds are live and whose tail repeats a single frame. The terminal underneath keeps running, so the video stops before the last command while the file reports its full length. Start the recording without a duration limit and stop it with a signal instead.

**Record the window, not the screen.** macOS wants a window id for that and `screencapture` will not look one up, so save this as `/tmp/winid.swift`:

```swift
import CoreGraphics
import Foundation
let needle = CommandLine.arguments[1]
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
for w in list where (w[kCGWindowOwnerName as String] as? String ?? "").contains(needle) {
    print(w[kCGWindowNumber as String] as? Int ?? 0)
}
```

Then:

```sh
swift /tmp/winid.swift Ghostty
screencapture -v -l<id> demo.mov &
sleep 40
kill -INT %1
```

`kill -INT` is what closes the file cleanly. `kill -TERM` leaves nothing behind.

## Checking the take before committing it

The file's duration says nothing about whether its frames are live. Pull frames and read them. `ffmpeg` is not installed on the machine this was shot on, so here is the same job in Swift, saved as `/tmp/frame.swift`:

```swift
import Foundation
import AVFoundation
import AppKit
let a = CommandLine.arguments
let asset = AVURLAsset(url: URL(fileURLWithPath: a[1]))
let gen = AVAssetImageGenerator(asset: asset)
gen.requestedTimeToleranceBefore = .zero
gen.requestedTimeToleranceAfter = .zero
let cg = try gen.copyCGImage(at: CMTime(seconds: Double(a[3])!, preferredTimescale: 600), actualTime: nil)
let rep = NSBitmapImageRep(cgImage: cg)
try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: a[2]))
```

```sh
for t in 4 12 20 28 34; do swift /tmp/frame.swift demo.mov "frame-$t.png" "$t"; done
```

Open the frames. They must differ from each other, the frame at 20 seconds must show the assembly, and the last one must show `fib(9)=34`. A repeated tail means the capture froze and the take is dead.

## Trimming it

`ffmpeg` is not on this machine, so trimming is also Swift. Save as `/tmp/clip.swift`:

```swift
import Foundation
import AVFoundation
let a = CommandLine.arguments
let asset = AVURLAsset(url: URL(fileURLWithPath: a[1]))
let output = URL(fileURLWithPath: a[2])
try? FileManager.default.removeItem(at: output)
let export = AVAssetExportSession(asset: asset, presetName: AVAssetExportPresetHighestQuality)!
export.outputURL = output
export.outputFileType = .mp4
export.shouldOptimizeForNetworkUse = true
export.timeRange = CMTimeRange(start: CMTime(seconds: Double(a[3])!, preferredTimescale: 600),
                               end: CMTime(seconds: Double(a[4])!, preferredTimescale: 600))
let done = DispatchSemaphore(value: 0)
export.exportAsynchronously { done.signal() }
done.wait()
```

```sh
swift /tmp/clip.swift demo.mov docs/images/demo.mp4 3 40
```

## Putting it in the README

The file is committed at `docs/images/demo.mp4`, and the README links it. GitHub plays a video only from its own attachment store, which you get by dragging the file into a comment box on github.com and copying the `user-attachments` URL it returns. Replace the link in the README with that URL on a line of its own and the recording becomes an inline player.

There is a second copy at a capy-hosted URL from the same take, which is not useful in a README and can be ignored.
