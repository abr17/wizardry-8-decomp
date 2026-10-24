/* Experimental (2026-10-23, the patch author's request), part of [Experimental] FrameRateFix:
   the party's movement no longer depends on the frame rate.

   The original moves the party with one step per rendered frame (W8GameData::ApplyCameraMotion
   0x0041F5F0, called once per frame at 0x0044FD84 from UpdateWorldCameraAndPaths), sized by the
   frame time. Its collision code has no step-up: a ledge or doorstep is climbed only when one
   frame's travel jumps the band where the riser stops it, and from rest a step must beat a fixed
   0.1-unit cut. So at high frame rates the party gets stuck on stairs and the Umpani doorsteps,
   climbs stairs slower, takes a little more falling damage, and uncapped it may not start at all
   (build\tmp\fps-indep\movement\findings.md, not in git).

   Fixed: the call goes to a wrapper that runs the original step at a fixed rate (30 per
   second, the original physics at that frame rate) as often as the elapsed time asks, and
   RenderFrame draws the world from a point interpolated between the last two steps, so the
   picture stays as smooth as the frame rate allows. Turning (mouse and keys) is not part of the
   step: it still happens every frame. With the switch off, or while the step cannot be split
   (no level data yet, a camera path running), the original single step runs; while the game
   is paused (no frame time) the original's step runs too and the picture stays where the
   interpolation left it.

   Moving props (lifts, doors) push the party by their move since the last rendered frame
   (W8GameData::ProbePropsAlongMotion 0x0041B770 reads W8Prop::GetDelta 0x0044E130 at
   0x0041B85E; the prop's pose pair rolls once per rendered frame). In our steps the push is
   the prop's move since the last step: its position now minus where the previous step found
   it (the per-frame moves are uneven at high frame rates, so no scaling of one frame's move
   matches); when the previous step did not probe it, the frame's move scaled by the time since
   the previous step / this frame's time. The further steps of one frame get none (the prop did
   not move between them). So the party rides a lift as at 30 fps at any frame rate. */
#include <windows.h>
#include <string.h>
#include "wiz8/experimental.h"
#include "patch_util.h"
#include "wiz8/camera_motion.h"
#include "wiz8/engine_code/GameData.h"
#include "wiz8/engine_code/GameTimeAccumulator.h"
#include "wiz8/engine_code/GDCamera.h"
#include "wiz8/engine_code/Levels.h"
#include "wiz8/local_screens/MainGameScreen.h"
#include "wiz8/local_screens/Screens.h"

namespace {

const unsigned kApplyCameraMotion = 0x0041f5f0;    /* thiscall, 4 arguments, ret 0x10 */
const unsigned kUpdateWorldCameraAndPaths = 0x0044fc20;
const unsigned kMotionCall = 0x0044fd84;           /* call rel32 -> ApplyCameraMotion */
const unsigned kFrameDeltaOffset = 0x28;          /* W8GameTimeAccumulator::m_frame_delta */
const unsigned kProbePropsAlongMotion = 0x0041b770;
const unsigned kPropDeltaCall = 0x0041b85e;        /* call rel32 -> W8Prop::GetDelta */
const unsigned kPropGetDelta = 0x0044e130;         /* thiscall char (float* out, const float* point), ret 8 */
const unsigned kPropPosition = 0x2c;               /* W8Prop::animation_position */

/* The step rate: 30 per second (the patch author's choice, 2026-10-23: the game as it was meant
   to play). The frame time is in tenths of a second, so one step is 1/3. The pure original ran
   at about 30-32 frames per second in the reference measurements; at 30 it climbs the Umpani
   doorsteps from a run-up at almost any angle and the Monastery stairs in 1.3 s. */
const float kTick = 10.0f / 30.0f; /* a literal, not 10 / a const float: no dynamic initializer */
/* At most this many steps of frame time kept (the original caps a frame at 2.0 tenths, 6
   steps; the extra room is for a step taken at once from rest). */
const int kMaxTicks = 8;
/* The physics point moved by something other than our steps (a teleport, a level load, a
   cancelled level exit, Recall, a saved game): no interpolation across it. Squared units. */
const float kSnapDistance2 = 1.0f;
/* The wrapper missed this many rendered frames (another screen was up): forget the keys. */
const unsigned kStaleFrames = 2;
const unsigned kMoveFlags = W8_CAMERA_MOTION_STRAFE_LEFT | W8_CAMERA_MOTION_STRAFE_RIGHT |
                            W8_CAMERA_MOTION_FORWARD | W8_CAMERA_MOTION_BACKWARD |
                            W8_CAMERA_MOTION_UP | W8_CAMERA_MOTION_DOWN;

/* Plain statics (the hybrid has no C runtime start-up). */
int s_valid;            /* s_prev/s_cur describe the party's recent steps */
float s_prev[3];        /* the physics point before the last step */
float s_cur[3];         /* the physics point after the last step */
float s_acc;            /* frame time not yet stepped, tenths of a second */
int s_last_moved;       /* the last step moved the party */
unsigned s_latched;     /* movement keys seen in frames without a step */
unsigned s_prev_move;   /* the movement keys of the last frame */
int s_render_frame;     /* the wrapper split this frame's step: the next RenderFrame may interpolate */
int s_called;           /* s_last_frame is set */
unsigned s_last_frame;  /* FrameRateFrameCount() at the wrapper's last call */
float s_prop_time;      /* frame time since the last step (tenths): the props' move owed */
int s_in_step;          /* inside one of our steps: the props' push is their move since the last step */
float s_prop_scale;     /* the per-frame push -> since the last step, when no mark has it */
unsigned s_step_serial; /* counts our steps (from 1) */
/* Where a prop was when a step last probed it, and the push that step gave. */
struct PropMark {
    const void* prop;
    unsigned step;
    float position[3];
    float push[3];
};
const int kPropMarks = 8;
PropMark s_marks[kPropMarks];
char s_status[48];

float Distance2(const float* a, const float* b)
{
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

void Copy3(float* to, const float* from)
{
    to[0] = from[0];
    to[1] = from[1];
    to[2] = from[2];
}

float* PhysicsPoint(void)
{
    return &g_gd_camera->m_position.x;
}

float* FrameDelta(void)
{
    return reinterpret_cast<float*>(reinterpret_cast<char*>(g_game_time_accumulator) + kFrameDeltaOffset);
}

/* thiscall through a __fastcall pointer (this in ecx, edx unused, the callee pops the rest). A
   real call, not inline asm: the compiler must empty the x87 stack before it, as the original's
   collision maths needs all of it. */
typedef unsigned char(__fastcall* ApplyCameraMotionFn)(void*, void*, unsigned, float*, float*, void*);

unsigned char CallApplyCameraMotion(void* self, unsigned flags, float* position, float* delta, void* saved)
{
    return reinterpret_cast<ApplyCameraMotionFn>(kApplyCameraMotion)(self, 0, flags, position, delta, saved);
}

/* W8Prop::GetDelta for ProbePropsAlongMotion (thiscall: the prop in ecx, the callee pops the
   two arguments). Inside our steps the prop's push is its move since the last step: its
   position now minus where it was at the previous step when that step probed it too (exact,
   as the per-frame moves are uneven at high frame rates), else the per-frame move scaled by
   time (s_prop_scale). Probed again in the same step: the same push. A prop the original
   gives no push (not moving one whole animation frame) gets none. Anywhere else, and with
   the switch off, the original's value. */
char __fastcall FixedTickPropDelta(void* prop, void* /*edx*/, float* out, const float* point)
{
    typedef char(__fastcall * GetDeltaFn)(void*, void*, float*, const float*);
    const char frames = reinterpret_cast<GetDeltaFn>(kPropGetDelta)(prop, 0, out, point);
    if (!s_in_step) {
        return frames;
    }
    const float* now = reinterpret_cast<const float*>(static_cast<const char*>(prop) + kPropPosition);
    PropMark* mark = 0;
    for (int i = 0; i < kPropMarks; ++i) {
        if (s_marks[i].prop == prop) {
            mark = &s_marks[i];
            break;
        }
    }
    float push[3];
    if (mark != 0 && mark->step == s_step_serial) {
        Copy3(push, mark->push);
    } else if (out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f) {
        push[0] = push[1] = push[2] = 0.0f;
    } else if (mark != 0 && mark->step + 1 == s_step_serial) {
        push[0] = now[0] - mark->position[0];
        push[1] = now[1] - mark->position[1];
        push[2] = now[2] - mark->position[2];
    } else {
        push[0] = out[0] * s_prop_scale;
        push[1] = out[1] * s_prop_scale;
        push[2] = out[2] * s_prop_scale;
    }
    if (mark == 0) {
        /* a new prop takes the slot probed longest ago */
        mark = &s_marks[0];
        for (int i = 1; i < kPropMarks; ++i) {
            if (s_marks[i].step < mark->step) {
                mark = &s_marks[i];
            }
        }
        mark->prop = prop;
    }
    mark->step = s_step_serial;
    Copy3(mark->position, now);
    Copy3(mark->push, push);
    Copy3(out, push);
    return frames;
}

/* Something raised by a step that the further steps of the frame must not run past: a modal
   dialog (the level exit question, a trigger's message) or a screen change (a level exit). */
bool StepRaisedStop(int had_modal, bool had_transition)
{
    return (!had_modal && g_modal_owner != 0) || (!had_transition && IsScreenTransitionPending());
}

/* Reached instead of ApplyCameraMotion from UpdateWorldCameraAndPaths (thiscall: this in ecx,
   the callee pops the four arguments - what __fastcall does with an unused edx). Same contract
   as the original: returns whether the party moved, *delta = the move from *position. */
unsigned char __fastcall FixedTickApplyCameraMotion(void* self, void* /*edx*/, unsigned flags, float* position,
                                                    float* delta, void* saved)
{
    s_render_frame = 0;
    s_in_step = 0;
    /* the wrapper did not run for a while (another screen was up): no key from then is held */
    const unsigned frame = FrameRateFrameCount();
    if (!s_called || frame - s_last_frame > kStaleFrames) {
        s_latched = 0;
        s_prev_move = 0;
    }
    s_called = 1;
    s_last_frame = frame;
    float* frame_delta = g_game_time_accumulator != 0 ? FrameDelta() : 0;
    float dt = frame_delta != 0 ? *frame_delta : 0.0f;
    if (!ExperimentalFrameRateFixEnabled() || g_level_data == 0 || g_gd_camera == 0 || frame_delta == 0 ||
        g_camera_path_active) {
        s_valid = 0;
        s_prop_time = 0.0f;
        return CallApplyCameraMotion(self, flags, position, delta, saved);
    }
    if (!(dt > 0.0f)) {
        /* paused (or resuming): the original's step (no time, no move); the picture stays at
           the interpolated point instead of jumping to the last step */
        const unsigned char moved = CallApplyCameraMotion(self, flags, position, delta, saved);
        s_render_frame = s_valid;
        return moved;
    }

    if (!s_valid || Distance2(position, s_cur) > kSnapDistance2) {
        Copy3(s_prev, position);
        Copy3(s_cur, position);
        s_acc = 0.0f;
        s_last_moved = 0;
        s_latched = 0;
        s_prev_move = 0;
        s_prop_time = 0.0f;
        s_valid = 1;
    }
    /* a movement key pressed from rest: step at once, so the first move waits for no step
       boundary (only on the press: a party held back by a wall or a riser keeps the fixed rate) */
    unsigned move = flags & kMoveFlags;
    if (move != 0 && s_prev_move == 0 && !s_last_moved && s_acc < kTick) {
        s_acc = kTick;
    }
    s_prev_move = move;
    s_acc += dt;
    if (s_acc > kTick * kMaxTicks) {
        s_acc = kTick * kMaxTicks;
    }
    s_prop_time += dt;
    if (s_prop_time > kTick * kMaxTicks) {
        s_prop_time = kTick * kMaxTicks;
    }

    const int had_modal = g_modal_owner != 0;
    const bool had_transition = IsScreenTransitionPending();
    float start[3];
    Copy3(start, position);
    unsigned char any = 0;
    int ticks = 0;
    while (s_acc >= kTick) {
        float point[3];
        float step[3];
        Copy3(point, PhysicsPoint());
        /* the props moved through the time since the last step before this frame's first step,
           and not at all between the steps of one frame */
        s_prop_scale = ticks == 0 ? s_prop_time / dt : 0.0f;
        *frame_delta = kTick;
        ++s_step_serial;
        s_in_step = 1;
        unsigned char moved = CallApplyCameraMotion(self, flags | s_latched, point, step, saved);
        s_in_step = 0;
        *frame_delta = dt;
        s_prop_time = 0.0f;
        s_latched = 0;
        s_acc -= kTick;
        ++ticks;
        s_last_moved = moved;
        any |= moved;
        Copy3(s_prev, s_cur);
        Copy3(s_cur, PhysicsPoint());
        /* the step moves the physics point to point + step when it moved the party, and not at
           all when it did not; anything else is a jump inside the step (a trigger's teleport):
           no interpolation from before it */
        float expected[3];
        Copy3(expected, point);
        if (moved) {
            expected[0] += step[0];
            expected[1] += step[1];
            expected[2] += step[2];
        }
        if (Distance2(s_cur, expected) > kSnapDistance2) {
            Copy3(s_prev, s_cur);
        }
        if (StepRaisedStop(had_modal, had_transition)) {
            /* the frame's remaining steps are dropped, as a frame of the original ends there */
            while (s_acc >= kTick) {
                s_acc -= kTick;
            }
            break;
        }
    }
    if (ticks == 0) {
        /* a short key press between two steps still moves the party */
        s_latched |= move;
        /* the caller cleared this frame's "moved" flag; the stamina, camping and combat code
           read it every frame */
        if (s_last_moved && g_level_flags != 0) {
            *g_level_flags |= W8_LEVEL_FLAG_MOVED_THIS_UPDATE;
        }
    }
    float* now = PhysicsPoint();
    delta[0] = now[0] - start[0];
    delta[1] = now[1] - start[1];
    delta[2] = now[2] - start[2];
    s_render_frame = 1;
    return any;
}

} // namespace

const char* FixedTickInstall(void)
{
    /* only over the exact original bytes, and only while both functions are the original's */
    const bool ok = PatchCallsTo(kMotionCall, kApplyCameraMotion) && PatchOriginal(kApplyCameraMotion) &&
                    PatchOriginal(kUpdateWorldCameraAndPaths);
    const bool done =
        ok && PatchBranch(kMotionCall, 0xe8, reinterpret_cast<const void*>(&FixedTickApplyCameraMotion), 5);
    /* the props' push, with the step it belongs to */
    const bool prop_ok = done && PatchCallsTo(kPropDeltaCall, kPropGetDelta) &&
                         PatchOriginal(kProbePropsAlongMotion) && PatchOriginal(kPropGetDelta);
    const bool prop_done =
        prop_ok && PatchBranch(kPropDeltaCall, 0xe8, reinterpret_cast<const void*>(&FixedTickPropDelta), 5);
    strcpy(s_status, PatchResult(ok, done));
    strcat(s_status, ", prop push ");
    strcat(s_status, PatchResult(prop_ok, prop_done));
    return s_status;
}

bool FixedTickRenderPoint(const float* camera, float* out)
{
    /* once per stepped frame: other screens' frames (map, inventory) draw as the original */
    const int stepped = s_render_frame;
    s_render_frame = 0;
    if (!stepped || !s_valid || !ExperimentalFrameRateFixEnabled()) {
        return false;
    }
    /* the camera is where the last step put it: nothing moved it since (a shake, a trigger
       action, a camera path) */
    if (Distance2(camera, s_cur) > kSnapDistance2) {
        return false;
    }
    float alpha = s_acc / kTick;
    if (alpha < 0.0f) {
        alpha = 0.0f;
    } else if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    out[0] = s_prev[0] + (s_cur[0] - s_prev[0]) * alpha;
    out[1] = s_prev[1] + (s_cur[1] - s_prev[1]) * alpha;
    out[2] = s_prev[2] + (s_cur[2] - s_prev[2]) * alpha;
    return true;
}
