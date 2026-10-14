The first version of object motion vectors is built, the game is running, no errors at startup. But
I have not checked it in the game itself yet: the G-buffer shaders with the new code are compiled
only when a level loads, so the main check is still ahead.

How it works:
- The vertex shader of every G-buffer draw saves the final vertex positions and reads the positions
  of the same vertices from the previous frame.
- The pixel shader writes the difference into an extra G-buffer target.
- Where an object moved, FSR gets this vector; everywhere else, the camera vector.
- Positions are saved only for moving draws: those whose vertex shader constants changed (object
  matrix, bones). Static geometry costs no memory.
- This covers any animation the vertex shader computes: skinning, weapons, cloth, enemies.

Please load into the game and write "ready". I will check the log: how many draws move and how many
of them had previous-frame positions found. Then run around with the saw on your back. For
comparison I can turn object vectors off on the fly, leaving only camera vectors.
