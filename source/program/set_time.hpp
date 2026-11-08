#pragma once

/* Direct "set the clock" support.
 *
 * Reached from the Game1._update hook (ServiceTimeRequest in main.cpp), which
 * is the only place performTenMinuteClockUpdate may be called from: the Tesla
 * overlay can write memory from its own thread, but the ten-minute update
 * touches the HUD, the music, the menus and the farmer, so the overlay only
 * publishes a request and this side does the work.
 *
 * The algorithm is CJB Cheats Menu's SafelySetTime
 * (CJBCheatsMenu/Framework/Cheats/Time/SetTimeCheat.cs), which walks the clock
 * through every ten-minute node instead of jumping, so NPC schedules and the
 * per-ten-minute world updates happen for each step.
 */

#include <cstdint>

namespace settime {

/* timeOfDay is a slot: *(slot) is the address of the int. */
constexpr uintptr_t kTimeOfDaySlotOff = 0xBBBB228u;
/* Game1.performTenMinuteClockUpdate -- static, no arguments, no MethodInfo. */
constexpr uintptr_t kPerfTenMinuteOff = 0x14675E0u;

/* What the player may pick: 06:00 .. 25:50, on a ten minute boundary.
 * 2600 (02:00) is a time the clock can *reach*, never a target. */
constexpr int kEarliestTarget = 600;
constexpr int kLatestTarget = 2550;
constexpr int kLatestClock = 2600;

/* HHMM is not decimal: 12:50 -> 13:00 is ten minutes, not fifty. */
inline int ToMinutes(int hhmm) {
    return (hhmm / 100) * 60 + (hhmm % 100);
}

inline int FromMinutes(int minutes) {
    return (minutes / 60) * 100 + (minutes % 60);
}

/* A time the clock may legitimately hold, including the 2600 ceiling. */
inline bool IsValidClock(int hhmm) {
    const int minute = hhmm % 100;
    return hhmm >= kEarliestTarget && hhmm <= kLatestClock &&
           minute < 60 && minute % 10 == 0;
}

/* A time the player may ask for. */
inline bool IsValidTarget(int hhmm) {
    return hhmm >= kEarliestTarget && hhmm <= kLatestTarget &&
           (hhmm % 100) < 60 && (hhmm % 100) % 10 == 0;
}

/* ---------------------------------------------------------------------------
 * Request hand-off: a seqlock.
 *
 * The overlay writes, this side reads, and neither can take a lock.  A plain
 * "read seq, read value, read seq" check is not enough: if the writer updates
 * the value before bumping seq, the reader sees the same seq twice and accepts
 * a half-published request.
 *
 * Writer (overlay, two 32-bit stores, in this order):
 *     seq    = ++n | 1     (odd: write in progress)
 *     target = hhmm
 *     seq    = n + 1       (even: published)
 *
 * Reader: skip odd or zero seq, read target, re-read seq, require the two to
 * match and be even.  A torn read therefore fails one of the checks and the
 * request is dropped rather than half-applied -- losing a request is harmless,
 * applying a half-written one is not.
 * ------------------------------------------------------------------------- */

struct Request {
    /* Fixed marker, and the first thing in the struct on purpose.
     *
     * The overlay has no symbol table, so it used to carry two numbers baked in
     * at build time: the module's text fingerprint and the offset of this
     * struct.  Both move on every rebuild -- the fingerprint is the offset of
     * __nx_mod0, which sits in .rodata and shifts whenever the text grows --
     * so a dev build and a release build needed two different overlays, and
     * forgetting to re-sync showed up as "找不到 subsdk8" with no clue why.
     *
     * Now the overlay scans the module's writable segment for these 16 bytes
     * and takes what follows as seq/target.  Nothing about the address is
     * baked into the overlay, so one .ovl works with every build.  Changing the
     * contents of the marker would break that, which is why it ends in a
     * version suffix. */
    char magic[16];
    /* Bumped by the writer.  Odd means "being written", even means "stable". */
    volatile uint32_t seq;
    /* HHMM the player picked.  Only meaningful when seq is even and stable. */
    volatile int32_t target;
};

struct State {
    uint32_t doneSeq;   /* seq of the last request that was consumed */
};

/* One non-blocking read.  Returns true and fills `out` only when the pair is
 * consistent and new.  Nothing is written to `st` here -- consumption happens
 * in Consume() so a failed ApplyTarget still counts as consumed (a retry would
 * re-run the ten-minute updates, which is worse than dropping the request). */
inline bool PeekEx(void* reqMem, uint32_t doneSeq,
                   uint32_t* outSeq, int* outTarget) {
    if (reqMem == nullptr || outSeq == nullptr || outTarget == nullptr) {
        return false;
    }
    const Request* req = reinterpret_cast<const Request*>(reqMem);

    const uint32_t seq1 = __atomic_load_n(&req->seq, __ATOMIC_ACQUIRE);
    if (seq1 == 0u || (seq1 & 1u) != 0u) {
        return false;                    /* never published, or mid-write */
    }
    const int32_t target = req->target;
    const uint32_t seq2 = __atomic_load_n(&req->seq, __ATOMIC_ACQUIRE);
    if (seq1 != seq2) {
        return false;                    /* writer moved underneath us */
    }
    if (seq1 == doneSeq) {
        return false;                    /* already handled */
    }
    *outSeq = seq1;
    *outTarget = target;
    return true;
}

inline bool Peek(void* reqMem, State* st, int* out) {
    if (st == nullptr) {
        return false;
    }
    uint32_t seq = 0;
    return PeekEx(reqMem, st->doneSeq, &seq, out);
}

/* Consumes exactly the seq that was validated.  Re-reading the slot here would
 * let a request that arrived in between be marked done without ever running. */
inline void ConsumeSeq(State* st, uint32_t seq) {
    if (st != nullptr) {
        st->doneSeq = seq;
    }
}

/* Marks the request as handled.  Called before running the algorithm, so a
 * crash mid-way cannot make the next frame redo it.  The trade-off is explicit:
 * a failed attempt is not retried. */
inline void Consume(void* reqMem, State* st, int target) {
    (void)reqMem;
    (void)target;
    if (st == nullptr) {
        return;
    }
    /* Re-read so doneSeq matches what was actually accepted. */
    if (reqMem != nullptr) {
        const Request* req = reinterpret_cast<const Request*>(reqMem);
        st->doneSeq = __atomic_load_n(&req->seq, __ATOMIC_ACQUIRE);
    }
}

/* Applies `target` the way CJB does.
 *
 *   forward : call performTenMinuteClockUpdate once per ten-minute interval
 *   backward: write (now - 20 minutes) into timeOfDay, then call it once --
 *             the call itself advances by ten, so the net step is ten back.
 *             Subtracting 20 from HHMM directly would give 1280 for 13:00,
 *             which is not a time; ToMinutes/FromMinutes do the borrow.
 *
 * Must be called on the game's update thread, outside menus and events.
 */
inline bool ApplyTarget(void* base, int target,
                        void (*performTenMinute)(void)) {
    if (base == nullptr || performTenMinute == nullptr ||
        !IsValidTarget(target)) {
        return false;
    }
    void** slot = reinterpret_cast<void**>(
        reinterpret_cast<unsigned char*>(base) + kTimeOfDaySlotOff);
    int* now = reinterpret_cast<int*>(*slot);
    if (now == nullptr) {
        return false;
    }

    const int delta = ToMinutes(target) - ToMinutes(*now);
    if (delta == 0) {
        return true;
    }

    const int intervals = delta / 10;
    if (intervals > 0) {
        for (int i = 0; i < intervals; i++) {
            performTenMinute();
        }
    }
    else {
        for (int i = 0; i > intervals; i--) {
            *now = FromMinutes(ToMinutes(*now) - 20);
            performTenMinute();
        }
    }
    return true;
}

/* One poll per frame, from the update hook. */
inline void Poll(void* base, void* reqMem, State* st,
                 void (*performTenMinute)(void)) {
    int target = 0;
    if (!Peek(reqMem, st, &target)) {
        return;
    }
    Consume(reqMem, st, target);         /* one shot, even if it fails */
    ApplyTarget(base, target, performTenMinute);
}

}  // namespace settime
