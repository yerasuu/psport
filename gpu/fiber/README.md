# Fiber

The PS4 `libSceFiber` (user-level fibers with their own stacks, switched in place), from shadPS4
(https://github.com/shadps4-emu/shadPS4, GPL-2.0-or-later) at commit `3a9f572`,
`src/core/libraries/fiber`. libSceUlt's job threads run on it. The first commit here is the files
as they were there; later ones adapt them to libpsport-gpu.
