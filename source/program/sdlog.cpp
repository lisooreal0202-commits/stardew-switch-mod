#include "program/sdlog.hpp"

/* Deliberately includes only libnx, not the exlaunch headers: both define
 * u8/u32/u64 and they cannot be mixed in one translation unit.
 *
 * Uses the raw `fs` service API rather than fsdevMountSdmc()/fopen().  The
 * newlib route pulls libnx's switch_crt0.o, which cannot be linked into a
 * `-nostartfiles` exlaunch module (dangerous relocation against
 * __eh_frame_hdr_start etc.). */
#include <switch.h>

#include <cstdarg>
#include <cstdio>

namespace sdlog {
namespace {

FsFileSystem g_fs{};
FsFile g_file{};
s64 g_off = 0;
bool g_open = false;
/* A filesystem is mounted and a directory inside it has been proven writable.
 * Deliberately separate from g_open: the mount can succeed while every file
 * operation still fails, and that distinction is the whole point of the probe
 * below. */
bool g_fs_ready = false;
/* 0 = nothing, 1 = SD root, 2 = SD /switch, 3 = save data. */
int g_chan = 0;
/* Directory prefix inside the mounted filesystem ("" or "/switch").  The
 * settings file has to live where the log does, because a config written
 * somewhere it cannot be read back from is worse than none. */
const char* g_dir = "";
char g_pathbuf[64] = {0};
/* Result codes of each step, so a failed mount can be reported on screen when
 * there is no log file to read.  0 = that step succeeded.
 *   [0] fsInitialize        [1] fsOpenSdCardFileSystem
 *   [2] last fsFsCreateFile [3] smInitialize
 *   [4] fsOpen_SaveData     */
unsigned g_diag[5] = {0, 0, 0, 0, 0};

int AppendStr(char* out, int cap, int n, const char* s) {
    if (s != nullptr) {
        for (const char* p = s; *p != '\0' && n < cap - 1; p++) {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return n;
}

/* `dir` may be "" (filesystem root) or "/switch". */
bool JoinPath(const char* dir, const char* name, char* out, int cap) {
    if (out == nullptr || name == nullptr || cap < 8) {
        return false;
    }
    out[0] = '\0';
    int n = AppendStr(out, cap, 0, (dir != nullptr) ? dir : "");
    n = AppendStr(out, cap, n, "/");
    n = AppendStr(out, cap, n, name);
    return n > 1;
}

/* `out` receives g_dir + "/" + name. */
bool MakePath(char* out, int cap, const char* name) {
    if (!g_fs_ready) {
        return false;
    }
    return JoinPath(g_dir, name, out, cap);
}

bool WriteSmall(const char* path, const char* data, u64 len) {
    fsFsDeleteFile(&g_fs, path);
    const unsigned cr = fsFsCreateFile(&g_fs, path, 0, 0);
    g_diag[2] = cr;
    if (R_FAILED(cr)) {
        return false;
    }
    FsFile f{};
    if (R_FAILED(fsFsOpenFile(&g_fs, path, FsOpenMode_Write, &f))) {
        return false;
    }
    if (len == 0) {
        /* Creating it was the point (the log is truncated, not written). */
        fsFileFlush(&f);
        fsFileClose(&f);
        return true;
    }
    const unsigned wr = fsFileWrite(&f, 0, data, len, 0);
    fsFileFlush(&f);
    fsFileClose(&f);
    return R_SUCCEEDED(wr);
}

/* Returns true only when `len` bytes came back and the first one matches. */
bool ReadSmall(const char* path, char* out, u64 len) {
    FsFile f{};
    if (R_FAILED(fsFsOpenFile(&g_fs, path, FsOpenMode_Read, &f))) {
        return false;
    }
    char tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    u64 got = 0;
    const unsigned rr = fsFileRead(&f, 0, tmp, (len < 8u) ? len : 8u, 0, &got);
    fsFileClose(&f);
    if (R_FAILED(rr) || got < len) {
        return false;
    }
    for (u64 i = 0; i < len; i++) {
        out[i] = tmp[i];
    }
    return true;
}

/* Prove that `dir` accepts a create + write + read-back round trip.
 *
 * Writing the log itself as the probe would be cheaper, but the probe must not
 * touch `sdv_map.cfg`: a probe that overwrites the stored value would reset
 * the very setting we are trying to restore.  So a throwaway file, deleted
 * afterwards. */
bool ProbeDir(const char* dir) {
    char path[64];
    if (!JoinPath(dir, "sdv_probe.tmp", path, sizeof(path))) {
        return false;
    }
    const char probe[2] = {'P', '\n'};
    if (!WriteSmall(path, probe, 2)) {
        return false;
    }
    char back[2] = {0, 0};
    const bool ok = ReadSmall(path, back, 2) && back[0] == 'P';
    fsFsDeleteFile(&g_fs, path);
    return ok;
}

/* Last-resort channel: the game's own save data.
 *
 * The SD card is only reachable when the process was granted the SdCard fs
 * permission, and on retail that permission comes from the npdm.  Save data is
 * granted to every application by definition -- the game writes its own save
 * there -- so this channel works even when the SD card does not.
 *
 * The owner uid comes from enumerating save data, NOT from `acc`.
 * accountInitialize() drags in libnx's applet.o, which drags in env.o, which
 * references __nx_exit, which drags in switch_crt0.o -- and that object cannot
 * be linked into a -nostartfiles exlaunch module (dangerous relocations
 * against __eh_frame_hdr_start).  Enumerating also happens to be the more
 * correct answer on a multi-user console: it picks the slot that actually
 * exists for this application instead of whichever profile was last opened. */
bool MountSaveData() {
    u64 pid = 0;
    if (R_FAILED(svcGetInfo(&pid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) ||
        pid == 0) {
        /* Stardew Valley.  Only reached if svcGetInfo refuses, which would be
         * surprising; the constant keeps the fallback working anyway. */
        pid = 0x0100E65002BB8000ull;
    }

    FsSaveDataInfoReader rd{};
    const unsigned orc = fsOpenSaveDataInfoReader(&rd, FsSaveDataSpaceId_User);
    if (R_FAILED(orc)) {
        g_diag[4] = orc;
        return false;
    }
    AccountUid uid{};
    bool haveUid = false;
    FsSaveDataInfo info{};
    s64 total = 0;
    for (int i = 0; i < 64; i++) {
        if (R_FAILED(fsSaveDataInfoReaderRead(&rd, &info, 1, &total)) ||
            total <= 0) {
            break;
        }
        if (info.application_id == pid && accountUidIsValid(&info.uid)) {
            uid = info.uid;
            haveUid = true;
            break;
        }
    }
    fsSaveDataInfoReaderClose(&rd);
    if (!haveUid) {
        g_diag[4] = 0xDEAD0001u;
        return false;
    }
    const unsigned rc = fsOpen_SaveData(&g_fs, pid, uid);
    g_diag[4] = rc;
    return R_SUCCEEDED(rc);
}

}  // namespace

bool Open() {
    if (g_open) {
        return true;
    }
    if (!g_fs_ready) {
        /* libnx's fsInitialize calls smInitialize internally; doing it first
         * and recording its result separates "no sm: session" from
         * "fsp-srv refused". */
        g_diag[3] = smInitialize();
        const unsigned rc = fsInitialize();
        g_diag[0] = rc;
        if (R_FAILED(rc)) {
            return false;
        }
        const unsigned sd = fsOpenSdCardFileSystem(&g_fs);
        g_diag[1] = sd;
        if (R_SUCCEEDED(sd)) {
            /* Root first; some Atmosphere configs hide the root from app
             * processes but leave /switch reachable. */
            if (ProbeDir("")) {
                g_chan = 1;
                g_dir = "";
                g_fs_ready = true;
            }
            else if (ProbeDir("/switch")) {
                g_chan = 2;
                g_dir = "/switch";
                g_fs_ready = true;
            }
            else {
                /* Mounted but not writable: free the handle before the save
                 * data attempt reuses the same struct. */
                fsFsClose(&g_fs);
            }
        }
        if (!g_fs_ready) {
            if (!MountSaveData() || !ProbeDir("")) {
                /* Leave fs initialized so a retry only repeats the cheap part. */
                return false;
            }
            g_chan = 3;
            g_dir = "";
            g_fs_ready = true;
        }
    }

    if (!MakePath(g_pathbuf, static_cast<int>(sizeof(g_pathbuf)),
                  "sdv_map.log")) {
        return false;
    }
    if (!WriteSmall(g_pathbuf, "", 0)) {
        return false;
    }
    if (R_FAILED(fsFsOpenFile(&g_fs, g_pathbuf, FsOpenMode_Write, &g_file))) {
        return false;
    }
    g_off = 0;
    g_open = true;
    return true;
}

int ReadSetting() {
    char path[64];
    if (!MakePath(path, static_cast<int>(sizeof(path)), "sdv_map.cfg")) {
        return -1;
    }
    char b[2] = {0, 0};
    if (!ReadSmall(path, b, 2)) {
        return -1;
    }
    return (b[0] == '1') ? 1 : 0;
}

bool WriteSetting(int value) {
    char path[64];
    if (!MakePath(path, static_cast<int>(sizeof(path)), "sdv_map.cfg")) {
        return false;
    }
    const char b[2] = {(value != 0) ? '1' : '0', '\n'};
    if (!WriteSmall(path, b, 2)) {
        return false;
    }
    /* Read back: a write that the filesystem accepted but did not persist is
     * indistinguishable from a successful one until the next boot, and by then
     * the player has already lost the setting. */
    char back[2] = {0, 0};
    return ReadSmall(path, back, 2) && back[0] == b[0];
}

int Channel() { return g_chan; }
unsigned DiagSm() { return g_diag[3]; }
unsigned DiagInit() { return g_diag[0]; }
unsigned DiagOpenFs() { return g_diag[1]; }
unsigned DiagCreate() { return g_diag[2]; }
unsigned DiagSaveData() { return g_diag[4]; }

bool IsOpen() { return g_open; }

void Line(const char* fmt, ...) {
    if (!g_open || fmt == nullptr) {
        return;
    }
    char buf[256];
    std::va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    int len = (n < static_cast<int>(sizeof(buf) - 2)) ? n
                                                     : static_cast<int>(sizeof(buf) - 2);
    buf[len++] = '\n';

    if (R_FAILED(fsFileWrite(&g_file, g_off, buf, static_cast<u64>(len), 0))) {
        return;
    }
    g_off += len;
    /* Flush every line so the last one survives a hang or a forced power-off. */
    fsFileFlush(&g_file);
}

void Close() {
    if (!g_open) {
        return;
    }
    fsFileClose(&g_file);
    g_open = false;
}

}  // namespace sdlog
