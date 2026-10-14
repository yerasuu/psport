# Motion vectors: follow-up work after the 2026-09-26 video

Source material: `video_2026-09-26_04-19-53.mp4`, FSR 3.1 Performance,
render 960×540 → output 1920×1080, and `out/dev/t71.log`.
This describes the current working-tree changes, not a confirmation of in-game quality.

A shared pass for all G-buffer draws turned out too expensive: Performance ran at about
49 FPS with it and 77–78 FPS without it. Object vectors are now enabled by default only
for shaders with a pose/bone buffer (size 640–16384 bytes, excluding the shared 864-byte
scene buffer; 16-byte step). In a trial run with a narrower 1024-byte threshold
this cut the number of processed draws to about 140 per frame, eliminated history
overflow and gave about 81–87 FPS. The remaining draws keep cheap camera vectors.
`BB_OBJECT_MOTION=0` disables the whole feature, `BB_OBJECT_MOTION_ALL=1` restores the shared
pass for A/B. The alpha channel of the auxiliary target stores depth: if a later normal
draw occluded the object, its vector is not used. Quality of the character and weapon in
motion after the selection still needs visual checking.

## Findings

- At the end of the source log, over 600 frames: 257400 G-buffer draws, 256200 declared
  moving, only 30000 got previous positions (~11.7% of all draws).
  This is draw coverage, not a pixel percentage. The old counter did not distinguish the
  cause of each miss.
- History reserved the size of the whole vertex buffer multiplied by the instance
  count **including firstInstance**. For submeshes of shared buffers these are extra
  positions; the budget was 2097152 positions per frame.
- History had no separate barrier between vertex shader writes/reads;
  reuse of the host-parameter ring did not wait for the GPU.
- Disabled writes of all vertices were directed to a single scratch element.
- `CameraMotion::OnDisplayPass` reset Depth() before computing jitter phases:
  Performance got 8 phases instead of 32. Reconstruction of position from depth
  also ignored the viewport offset.

## Changes

- The history range is determined by min/max of the actual indices, accounting for baseVertex
  and primitive restart. FirstInstance is subtracted in the shader instead of consuming memory.
- All eligible direct G-buffer draws are stored, starting from their first appearance.
  The constant-change heuristic was removed: it considered almost all draws moving
  and missed the start of motion. Reading is possible only on a match with the previous frame.
- The key includes all vertex stream descriptions, the shader, the address and content hash
  of the indices, the vertex range, instances and the ordinal among identical draws.
  A topology change or a skipped frame resets the corresponding history.
- Budget: 4194304 positions/frame; the two arrays together take 128 MiB instead of 64 MiB.
  When memory runs out, camera vectors remain. The log separately shows stores,
  matches, missing history, out-of-memory and invalid ranges.
- Added vertex-to-vertex barriers, a GPU wait before reusing
  host parameters, coherent host memory and independent tracking of the motion image layout.
  Reading the image again does not clear the vectors-present flag.
- Disabled accesses are skipped by branching. For repeated invocations of the same
  indexed vertex, atomic writes of the position components are used.
  Positions behind the camera and non-numeric/infinite vectors are not used.
- Performance gets 32 jitter phases after the FSR context is created. Camera vectors
  are reconstructed from unjittered coordinates; object vectors were already obtained
  from clip positions before the viewport offset.

## Checks

Quality on the RX 7800 XT, FPS and coverage percentage after the changes are not yet checked.

## Next in-game run

From `game_files`:

```bash
BB_FRAME_STATS=1 BB_TOGGLE_FILE=out/dev/motion-toggles bash native_probe/run.sh 2>&1 | tee native_probe/out/dev/motion-run.log
```

Bit 29 in the temporary toggle file disables only history writes. For a correct
performance comparison, run the game with `BB_OBJECT_MOTION=0` and without it:
the pipeline selection and the extra target are set at process start.
After switching, give FSR a few frames to refresh its history.
Compare the same scene: character/weapon while running, camera turn, combat, static camera.
After ~600 frames check `Object motion:`: `with history`, `capacity skips`, `unmatched`.

Remaining limitations: identical meshes are matched by draw order, which
can change with object culling; a change of dynamic vertex buffer addresses
loses the match. Indirect draws, tessellation/geometry shaders, MSAA and passes
outside the G-buffer stay on camera vectors. Transparency/particles, blurry UI and moving
scaling to the renderer level are not solved by this change. The cost of index scanning
and atomic writes needs to be measured in a real run.

## September 29: weapons and small skeletons (whirligig saw ghosting)

Video `video_2026-09-29_23-08-26.mp4`: the character stands still, the saw disc spins and leaves
a trail. The `BB_MOTION_SELECT_LOG=1` log showed the cause: the vertex shader's second buffer is
a bone palette of 48 bytes each (3x4), and for weapons/props it is 96–384 bytes (2–8 bones).
The "from 640 bytes" rule did not include such draws, the disc got only camera vectors,
i.e. it was static for FSR.

Changes:

- `Motion::ClassifyBuffer` (`motion_history.h`): 864 — scene, ≥640 — character skeleton,
  96..639 multiple of 48 — small skeleton. Pipelines with any skeleton get the variant
  with vectors.
- There are about a thousand small skeletons per frame, most of them static map parts.
  So they are "gated": the palette hash is compared with the previous frame using a cheap
  key (`History::Moving`) before index scanning. Match: the camera vector is exact,
  nothing is written. Changed: positions are stored, and from the next frame on it gets an
  object vector. A hash with the 416-byte buffer does not work: it changes along with the
  camera for almost all of them (verified: history overflow and FPS drop to ~40).
- Menu: "Show motion vectors (debug)" («Показать векторы движения (отладка)»). Red/green:
  |x|/|y|, blue: the pixel got an object vector. `BB_MOTION_SELECT_LOG=1` prints G-buffer
  shaders with buffer sizes and the decision.

Measurement `out/motion-gated.log`: ~225 stored draws and ~600 "static" ones per frame,
no overflow, 58–80 FPS in game scenes. The disappearance of the saw trail still needs
visual confirmation.
