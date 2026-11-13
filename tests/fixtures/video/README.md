# Video fixture

`clip.webm` is the only committed binary fixture (a Python script cannot write AV1). It is CC0.
It has no third-party content: six solid-color frames (two red, two green, two blue; 64x48, 10 fps,
0.6 s) and a 440 Hz sine tone (Opus, 48 kHz, 0.6 s).

ffmpeg 9.0.2 (libaom, libopus) made it with this command, run in this directory:

```
ffmpeg -y -f lavfi -i "color=c=0xFF0000:s=64x48:r=10:d=0.2" -f lavfi -i "color=c=0x00FF00:s=64x48:r=10:d=0.2" -f lavfi -i "color=c=0x0000FF:s=64x48:r=10:d=0.2" -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=0.6" -filter_complex "[0:v][1:v][2:v]concat=n=3:v=1:a=0,format=rgb24,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p[v]" -map "[v]" -map 3:a -c:v libaom-av1 -cpu-used 8 -crf 30 -b:v 0 -g 100 -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -c:a libopus -b:a 24k -f webm clip.webm
```

The decoded colors are close to (251, 0, 0), (0, 252, 0) and (0, 0, 252). Tests use a tolerance.
