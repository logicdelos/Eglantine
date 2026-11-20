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
c++ -std=c++20 -O2 -Isrc tools/replay_qualify/make_gpu_witness.cpp src/video_core/replay/format_io.cpp -o /tmp/make_gpu_witness
python -I -B tools/replay_qualify/run_gpu_witness.py --producer /tmp/make_gpu_witness --cli /path/to/eden-cli --out /new/private/output
```

The output directory must not exist and must be outside a git worktree or release
staging. Each invocation creates an empty profile. The driver regenerates and
compares the stream bytes, qualifies two independently rendered image directories,
compares them, and checks that an unmapped final backing page and an extra declared
but unreproduced effect are rejected for their respective reasons. Reports and PNGs
remain in that directory; reference PNGs are never copied into observed output.

`--preroll` converts the first frame into a validated warmup: its four effect rows and
composed dimensions/checksum belong exclusively in `preroll_effects.jsonl` and
`preroll_frames.jsonl`. No live comparison or selected PNG is generated for that frame.
`--unmet-present-fence` adds an external threshold to frame 2 without a captured
syncpoint raise. An idle GPU is insufficient: standalone replay must fail before composition.
Exercise both with:

```sh
python -I -B tools/replay_qualify/test_gpu_boundaries.py --producer /tmp/make_gpu_witness --cli /path/to/eden-cli
```
