# Pip-Net DATA-page FPS regression (experiment)

### User-observed problem
With the experimental surface probe enabled, selecting DATA in Pip-Boy caused
the game to freeze heavily and fall to about 4 FPS. The test DDS was visible.

### Likely root cause (confirmed from source, performance not yet profiled)
The initial `SetSurfaceVisible` called `FindInMap("PN_WebPanel")` from every
`kMessage_MainGameLoop` callback. `FindInMap` recursively searches up to
8,192 nodes and repeatedly calls `VirtualQuery`. Even when the browser
was not active, merely selecting DATA could trigger this walk every frame.
With F11 active, `DrawSurfaceProbeFrame` also searched for a surface and
called `IDirect3DTexture9::LockRect` on every presented frame.

### Remediation
1. Skip all tree traversal while inactive and hidden.
2. Cache panel and surface per MapMenu root; retry unresolved names at 1 Hz.
3. Toggle a tile only when the requested visible state changes.
4. Make surface animation opt-in. A static DDS stays in the game's normal
   image path without any CPU texture-writing work.
5. If enabled, cap animation to 10 updates/second maximum (default 8).
6. Clear cached pointers when MapMenu closes/reinitializes.

### Verification plan
- With `SurfaceProbe=1` and `SurfaceProbeAnimation=0`, open DATA without
  pressing F11: FPS should remain near baseline.
- F11 shows the static green grid; press F11 again to hide.
- Open/close Pip-Boy repeatedly and switch DATA -> ITEMS -> DATA.
- Then set `SurfaceProbeAnimation=1`, test `SurfaceProbeFps=2`, then 8.
- Check borderless vs fullscreen and DXVK.
- With `SurfaceProbe=0` confirm the external WebView2 browser remains usable.

### Status
Win32 compile is the automated qualification. No FPS claim is verified
until a FalloutNV runtime session is completed.
