# Working VRChat setup — 5 November 2026

Confirmed by the user: headset visuals, tracking, and smooth head turning work. The regular judder appears resolved with DXMT and the paced-pose runtime.

- Launch through Mac Steam using VRConMac Linux engine (`notproton-vrconmac-lkd`).
- Graphics backend: DXMT.
- Voile Linux engine: v26.3.0-24, with matched Linux Steam SDK build1792618285.
- VRChat-only Wine network-cost replacement passed its probe and was installed; the original crash stopped.
- Native runtime: `runtime-host/oxrsys-paced-pose/oxrsys-runtime.json`.
- Bridge render scale:0.7.
- OXRSys: fixed60Mbps, adaptive bitrate off, speed preset, resolution scale0.75, foveated encoding off. Effective headset refresh72Hz.

Start the bridge:

```bash
cd /path/to/VRonMac
bridge/build/vrconmac-bridge --backend openxr --render-scale 0.7 \
  --runtime-json runtime-host/oxrsys-paced-pose/oxrsys-runtime.json
```

Then launch VRChat from Steam in VR mode.

Latest observed bridge frame-clock samples:71.21–72.00Hz, compared with about63Hz before deadline pacing. Latest game log contained no debugger-detection alert at the time checked; the earlier EAC detection remains unexplained, and longer-session stability is not established.

The native runtime includes two local changes: preserve the image's actual submitted pose in video metadata, and keep absolute frame-clock deadlines instead of accumulating timer oversleep. Sources, licenses and test results are packaged beside the runtime in the project. The original OXRSys installation and pose-only runtime remain available.
