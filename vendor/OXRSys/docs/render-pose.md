# Submitted projection poses (local experiment)

The 1.2.0 streaming path sampled current tracking when sending every frame, including repeated images from a bridge. This local change captures the center of submitted projection views in fixed LOCAL, LOCAL_FLOOR or STAGE spaces, including their reference-space offset, and carries that pose with the pixels into video metadata. VIEW/action spaces retain the old fallback because their historical world transform is not available. No wire format changes or headset reinstall is needed.

Math and queue-preservation tests cover opposite-sign eye quaternions, reference-space translation/rotation, repeated images, invalid inputs, and dynamic-space fallback. Actual headset validation remains required.

## Frame-clock experiment

A separate build also keeps an absolute pacing deadline grid rather than anchoring the next sleep to a late wake-up. The observed bridge was ticking around63Hz while nominally reporting72Hz. Late applications do not wait for an additional period; missed deadlines are skipped without a catch-up queue. Refresh changes and session restarts reset the clock. Tests simulate oversleep, late applications, long pauses, refresh changes, and session reset. A five-second diagnostic reports actual wait rate. This timing change has not yet been validated in the headset.
