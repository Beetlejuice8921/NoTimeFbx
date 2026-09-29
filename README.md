<img src="icon.png" width="185" alt="NoTime Fbx">

**English** | [Русский](README.ru.md)

# NoTime Fbx

A very fast FBX viewer for Windows. Double-click an `.fbx` and the model is on screen in about
10–20 ms. You can rotate it and zoom, and that's all it does.

> Powered by **WARP**. Yes, like the warp drive: while other programs crawl towards your model at
> sublight speed, NoTime Fbx simply bends space-time, and the model is already there.
> *(The boring truth: WARP is Microsoft's software Direct3D, which doesn't have to wait for the GPU driver.)*

## Speed

<table>
  <tr>
    <td><img src="docs/benchmark.png" width="385" alt="Average time to open, seconds"></td>
    <td align="center"><img src="docs/notimefbx.gif" width="385" alt="NoTime Fbx"><br><sub><i>The only thing I can compare it to</i></sub></td>
  </tr>
</table>

Average time from launching the program to the model on screen, in seconds, over five models from
1k to 2.4M triangles (Windows 10, RTX 4060). FBX Review could not open the heaviest model, so its
average is over the other four.

## Install

Download `NoTimeFbx-setup.exe` from [Releases](../../releases) and run it. It installs for the
current user (no admin rights needed) into `%LOCALAPPDATA%\Programs\NoTimeFbx`, adds a Start menu
shortcut and offers to open `.fbx` files with it. Uninstall from Settings → Apps.

`NoTimeFbx-setup.exe` is the program itself: rename it to `NoTimeFbx.exe` and it runs as a plain
viewer without installing.

## Controls

| Action | How |
|---|---|
| Rotate | left mouse button |
| Zoom | mouse wheel |
| Open another file | drop an `.fbx` onto the window |
| Close | Esc |

## Under the hood

- **Fast start.** Light models (up to ~150k triangles) are drawn with WARP, Microsoft's software
  Direct3D 11, which is ready in ~5 ms instead of the ~140 ms it takes to load the GPU driver.
  Heavy models go straight to the GPU; if WARP can't keep up (big window, slow CPU), the model
  moves to the GPU on the fly.
- **Parallel loading.** The file is parsed ([ufbx](https://github.com/ufbx/ufbx)) on all cores
  while the window and Direct3D are being created.
- **Cache.** Models of 8 MB and up are stored ready to draw after the first open
  (`%LOCALAPPDATA%\NoTimeFbx\cache`, at most 2 GB), so reopening them skips FBX parsing.
- **Textures** stream in after the first frame: PNG, JPEG, TIFF, BMP, DDS, TGA; cut-outs (foliage,
  fences) are detected automatically.
- Binary and ASCII FBX, including files from "kn5 converter" (Assetto Corsa).

## Building

You need MSVC (Visual Studio 2022 or newer with "Desktop development with C++") and Windows SDK
10.0.22621 or newer. Then:

```
build.bat          :: release: build\NoTimeFbx.exe and build\NoTimeFbx-setup.exe
build.bat debug    :: debug build
```

The icon is built from `icon.png`: `powershell -ExecutionPolicy Bypass -File res\make_icon.ps1`.

### Diagnostic environment variables

| Variable | Effect |
|---|---|
| `NOTIMEFBX_TIMING=1` | startup timings per stage in the window title |
| `NOTIMEFBX_LOG=1` | log to `%TEMP%\NoTimeFbx.log` (materials, textures, renderer choice) |
| `NOTIMEFBX_NOCACHE=1` | don't use the cache |
| `NOTIMEFBX_RENDERER=gpu\|warp` | force the renderer |
| `NOTIMEFBX_WARP_MAX_TRIS=N` | triangle threshold for WARP (default 150000) |

## License

MIT, see [LICENSE](LICENSE). The bundled [ufbx](https://github.com/ufbx/ufbx) library keeps its own
license (MIT / Public Domain, `third_party/ufbx/LICENSE`), with a small change described in
`third_party/ufbx/PATCHES.txt`.
