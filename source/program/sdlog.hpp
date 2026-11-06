#pragma once

/* Minimal SD-card logger + settings store.
 *
 * exlaunch builds with `-nostartfiles` and an empty LIBS, so the module has no
 * way to write files by default and `svcOutputDebugString` output is invisible
 * on retail hardware without a debugger attached.  This links libnx for the
 * `fs` service wrappers and writes plain text next to the game.
 *
 * Never call these from the render hook every frame -- Open() mounts a
 * filesystem and probes it with a create/write/read round trip.  Call it once,
 * then append small lines.  If Open() fails every call becomes a no-op. */

namespace sdlog {

/* Tries the SD card root, then /switch, then the game's own save data, and
 * keeps the first one that survives a write+read-back probe.  Returns false
 * when none of them works; safe to call again (each retry repeats only the
 * part that failed). */
bool Open();
bool IsOpen();
void Line(const char* fmt, ...);
void Close();

/* One-byte on/off setting stored next to the log (`sdv_map.cfg`).
 *
 * The FastAnimations checkbox is an element we append to the options list
 * ourselves, so it is not part of GameOptions and its checked state is
 * recreated from scratch every launch.  Mirroring the value into a file is
 * the only way to make "ticked" survive a restart.
 *
 * ReadSetting() returns -1 when there is no readable file (first run), so a
 * caller can tell "off" from "never chosen".  WriteSetting() is meant to be
 * called once when the box is toggled, not every frame; it re-reads what it
 * wrote and returns false if the value did not stick. */
int ReadSetting();
bool WriteSetting(int value);

/* Which store Open() settled on: 0 = none, 1 = SD root, 2 = SD /switch,
 * 3 = save data.  Anything other than 1 is worth reporting, because the SD
 * root is where a log file is expected to appear. */
int Channel();

/* Result codes of each mount step, so a failed mount can be reported on
 * screen when there is no log file to read. 0 = that step succeeded.
 *   DiagSm / DiagInit / DiagOpenFs / DiagCreate / DiagSaveData */
unsigned DiagSm();
unsigned DiagInit();
unsigned DiagOpenFs();
unsigned DiagCreate();
unsigned DiagSaveData();

}  // namespace sdlog
