# Original standalone GPU replay witness

`witness.egcap` is generated entirely by `tools/replay_qualify/make_gpu_witness.cpp`.
It contains a reset Maxwell channel, one defined mapping, two submitted inline NOPs,
two changing semaphore values, and two opaque monochrome 32×18 surfaces (black then
white). No application, title, key, device dump or library census supplied any byte.
The expectation ledger describes those authored operations, not values read from
replay. The renderer must actually compose each surface and the pusher must consume
each word. No guest executes; zero library calls is correct only for this witness.

From the source root, compile the producer and run the *integrated* CLI serially:

```sh
g++ -std=c++20 -O2 -flto -fwhole-program -Isrc tools/replay_qualify/make_gpu_witness.cpp src/video_core/replay/format_io.cpp src/video_core/textures/decoders.cpp -o /tmp/make_gpu_witness
python -I -B tools/replay_qualify/run_gpu_witness.py --producer /tmp/make_gpu_witness --cli /path/to/eden-cli --out /new/private/output
```

The output directory must not exist and must be outside a git worktree or release
staging. Each invocation creates an empty profile. The driver regenerates and
compares the stream bytes, qualifies two independently rendered image directories,
compares them, and checks three separate refusals: a missing backing page before frame 1,
a missing fifth effect on frame 2, and an unsatisfied frame-2 present fence. Reports and PNGs
remain in that directory; reference PNGs are never copied into observed output.

`replay.pictures_written` counts completed selected frames with pictures. Each frame writes
two PNGs: `live_*.png` is the reference from the capture; `replay_*.png` is the standalone
render. `replay.picture_files_written` counts individual successfully written PNG files.

`--preroll` converts the first frame into a validated warmup: its four effect rows and
composed dimensions/checksum belong exclusively in `preroll_effects.jsonl` and
`preroll_frames.jsonl`. No live comparison or selected PNG is generated for that frame.
`--unmet-present-fence` adds an external threshold to frame 2 without a captured
syncpoint raise. An idle GPU is insufficient: standalone replay must fail before composition.
`--guest-readback` issues a Maxwell conditional-render comparison: its two guest-command
cache readbacks must survive replay verification in the same frame. `--lost-guest-readback`
omits the command while retaining the declared count; it must fail on frame 1.
Exercise these boundaries with:

```sh
python -I -B tools/replay_qualify/test_gpu_boundaries.py --producer /tmp/make_gpu_witness --cli /path/to/eden-cli
```
