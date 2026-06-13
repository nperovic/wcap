wcap
====

原始 [wcap][] 的繁體中文與深色介面改版。

本專案是 [mmozeiko/wcap][wcap] 的小幅介面改版，主要變更如下：

* 為設定視窗與系統匣選單加入完整的深色主題。
* 新增繁體中文（台灣）介面，並保留英文介面。

除了上述介面調整之外，錄影功能、編碼能力、快捷鍵、設定項目與操作方式皆與原始專案相同。

This project is a lightly modified edition of [mmozeiko/wcap][wcap]. It adds a complete dark theme
and a Traditional Chinese (Taiwan) interface while retaining the English interface. All recording
features, encoding capabilities, shortcuts, settings, and behavior remain the same as the original
project.

Simple and efficient screen recording utility for Windows.

Get latest binary here: [wcap-x64.exe][] or [wcap-arm64.exe][]

**WARNING**: Windows Defender or other AV software might report false positive detection

Features
========

 * press <kbd>Ctrl + PrintScreen</kbd> to start recording current monitor (where mouse cursor currently is positioned)
 * press <kbd>Ctrl + Win + PrintScreen</kbd> to start recording currently active window
 * press <kbd>Ctrl + Shift + PrintScreen</kbd> to select & record fixed region on current monitor
 * press any of previous combinations to stop recording
 * right or double-click on tray icon to change settings
 * video encoded using [H264/AVC][], [H265/HEVC][] or [AV1][], with 10-bit support for HEVC and AV1
 * audio encoded using [AAC][] or [FLAC][]
 * for window capture record full window area (including title bar/borders) or just the client area
 * window capture can record **application local audio**, no other system/process audio included
 * options to exclude mouse cursor from capture, disable recording indication borders, or rounded window corners
 * can limit recording length in seconds or file size in MB's
 * can limit max width, height or framerate - captured frames will be automatically downscaled
 * when limiting max width/height - can perform **gamma correct resize**
 * optional **improved color conversion** - adjust output YUV values to better match brightness to original RGB input

Details
=======

wcap uses [Windows.Graphics.Capture][wgc] API available since **Windows 10 version 1903, May 2019 Update (19H1)** to capture
contents of window or whole monitor. Captured texture is submitted to Media Foundation to encode video to mp4 file with
hardware accelerated codec. Using capture from compositor and hardware accelerated encoder allows it to consume very
little CPU and memory.

You can choose in settings to capture only client area or full size of window - client area will not include title bar and
borders for standard windows style. Recorded video size is determined by initial window size.

By default hardware encoder is enabled, you can disable it in settings Make sure your GPU drivers are updated if something is
not working with hardware video encoding. Then video will be encoded using [Microsoft Media Foundation H264][MSMFH264]
software encoder. You might want to explicitly use software encoder on older GPU's as their hardware encoder quality is not great.

Audio is captured using [WASAPI loopback recording][] and encoded using [Microsoft Media Foundation AAC][MSMFAAC] encoder, or
undocumented Media Foundation FLAC encoder (it seems it always is present in Windows 10 and 11).

Recorded mp4 file can be set to use fragmented mp4 format in settings (only for H264 codec). Fragmented mp4 file does not
require "finalizing" it. Which means that in case application or GPU driver crashes or if you run out of disk space then
the partial mp4 file will be valid for playback. The disadvantage of fragmented mp4 file is that it is a bit larger than
normal mp4 format, and seeking is slower.

You can use settings dialog to restrict max resolution of video - captured image will be scaled down to keep aspect ratio
if you set any of max width/height settings to non-zero value. Similarly framerate of capture can be reduced to limit
maximum amount of frames per second. Setting it to zero will use compositor framerate which is typically monitor refresh
rate. Lower video framerate will give higher quality video for same bitrate and reduced GPU usage. If you notice too many
dropped frames during recording, try reducing video resolution and framerate.

Capture of mouse cursor can be disabled only when using Windows 10 version 2004, May 2020 Update (20H1) or newer.

On Windows 11 you can disable yellow recording borders, or rounded window corners. On Windows 11 24H2 version can enable secondary
window capture, when they intersect main window in window-only capture mode - for popups & tool windows like "Open File" dialog.

HEVC Software Encoding
======================

HEVC encoding in software (on CPU) will require installing HEVC Video Extensions from Windows Store. It will support only
8-bit encoding. You can get direct download to installer package without using Windows Store application with following steps:

1) open https://store.rg-adguard.net/
2) search `https://www.microsoft.com/store/productId/9n4wgh0z6vhq` for `Retail` channel
3) download & run .appxbundle package it gives you

Creating gif from mp4
=====================

If you want to create gif file out of recorded mp4 file, you can use following .bat file:

    ffmpeg.exe -hide_banner -nostdin -loglevel fatal -stats -y -i %1 -filter_complex "[0]fps=15,split[v0][v1];[v0]palettegen=stats_mode=full[p];[v1][p]paletteuse" %~n1.gif

Or to create new palette every frame for more colors, but larger file size:

    ffmpeg.exe -hide_banner -nostdin -loglevel fatal -stats -y -i %1 -filter_complex "[0]fps=15,split[v0][v1];[v0]palettegen=stats_mode=single[p];[v1][p]paletteuse=new=1" %~n1.gif

Put this line in `make_gif.bat` file, place [ffmpeg][] executable next to it and then simply drag & drop .mp4 file on top of it.
Change `fps=15` to desired gif fps (or remove to use original video fps). Check the [paletteuse][] filter arguments for
different dither methods.

Building
========

The complete and authoritative build procedure is documented in [BUILDING.md](BUILDING.md).

For the standard x64 release build, install Visual Studio Build Tools 2022 with the MSVC x64/x86
tools and Windows 11 SDK, then run from the repository root:

```powershell
cmd /c build.cmd x64
```

This produces `wcap-x64.exe`. Follow `BUILDING.md` for exact component requirements, optional
build modes, verification steps, and troubleshooting.

License
=======

This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this software, either in source code form or as
a compiled binary, for any purpose, commercial or non-commercial, and by any means.

[wcap-x64.exe]: https://raw.githubusercontent.com/wiki/mmozeiko/wcap/wcap-x64.exe
[wcap-arm64.exe]: https://raw.githubusercontent.com/wiki/mmozeiko/wcap/wcap-arm64.exe
[wcap]: https://github.com/mmozeiko/wcap
[wgc]: https://blogs.windows.com/windowsdeveloper/2019/09/16/new-ways-to-do-screen-capture/
[MSMFH264]: https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder
[VS]: https://visualstudio.microsoft.com/vs/
[WASAPI loopback recording]: https://docs.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording
[MSMFAAC]: https://docs.microsoft.com/en-us/windows/win32/medfound/aac-encoder
[ffmpeg]: https://ffmpeg.org/
[paletteuse]: https://ffmpeg.org/ffmpeg-filters.html#paletteuse
[H264/AVC]: https://en.wikipedia.org/wiki/Advanced_Video_Coding
[H265/HEVC]: https://en.wikipedia.org/wiki/High_Efficiency_Video_Coding
[AV1]: https://en.wikipedia.org/wiki/High_Efficiency_Video_Coding
[AAC]: https://en.wikipedia.org/wiki/Advanced_Audio_Coding
[FLAC]: https://en.wikipedia.org/wiki/FLAC
