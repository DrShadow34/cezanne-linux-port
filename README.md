# About:

### What this?

Vibecoded port of mainline linux to Redmi K30 Ultra. That's it.

### Current status?

Most of it works. All drivers at least ported and confirmed working. There are few missing non-priority features, like wired headset's buttons support. And some things are implemented, but not tested: bi-directional calls for example - I have no one to call, so yeah. 

Other than that, it's drop-in replacement for stock ROM. Sort of.

### My device have same {driver_name}, what do I need to port it?

You can check info on each driver and it's progress in [hardware.md](hardware.md) 

Pay close attention to used firmware - some of it wasn't reverse engineered and driver may still expect original data from MIUI to exist in it's dedicated partition.  

### Can we address the elephant in the room? 

There are couple caveats:
1. Project fully vibesloped. It was intended to be local only thing, but some people asked me to release the files, hence I'm releasing it;
2. No support whatsoever, if you decided to use anything within this repo;
3. Nothing from this repo will be pushed to mainline as is. If you need something, you will need to review it before push and then maintain on your own.
