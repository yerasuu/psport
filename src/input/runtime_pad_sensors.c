/* The gamepad's own motion sensors: the gyroscope and accelerometer of a DualShock 4, DualSense,
 * Switch Pro or Steam Deck (when SDL reaches its HID device, not Steam's virtual pad), read through
 * SDL3 at every pad read and reported as the DualShock 4 does: the orientation quaternion, the
 * acceleration in g and the angular velocity in rad/s.
 *
 * Axes: SDL gives every gamepad's sensors in one frame, the pad held in front of you: +X right, +Y
 * up out of its face, +Z towards the player; acceleration in m/s^2 with gravity reading +Y at rest,
 * rotation in rad/s counter-clockwise seen from the positive end of each axis (x pitch, y yaw, z
 * roll). That is libScePad's DualShock 4 frame too (1 g on +Y lying flat, the frame the motion
 * emulation and shadPS4's and AnyPS5's pads report), so the axes and signs pass through unchanged
 * and only the acceleration is divided by standard gravity.
 *
 * Orientation, as libScePad computes it (kb #553): it starts at identity, the first read after an
 * open only latches the time, then the angular velocity is integrated into the quaternion. With
 * scePadSetTiltCorrectionState (off by default, as on the console) its tilt is pulled towards the
 * measured gravity, only while the acceleration is within 0.08 g of 1 g, by libScePad's gain.
 * Yaw has no reference and drifts; scePadResetOrientation sets the quaternion back to identity.
 *
 * Off with PS_PAD_SENSORS=0 (the motion emulation or the pad at rest instead); the game turns the
 * reports off and on with scePadSetMotionSensorState and the tilt correction with
 * scePadSetTiltCorrectionState. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_pad.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#define TILT_MAX_OFF_G 0.08f /* further from 1 g the pad is being moved: gravity is not what it reads */
#define TILT_MIN_AXIS 0.01f   /* a smaller correction axis is noise */
#define DEADBAND_RAD_PER_S 0.0174533f /* 1 degree/s, scePadSetAngularVelocityDeadbandState */
#define MAX_STEP_S 0.1f /* a longer gap between reads (a menu, a load) is not integrated */

static struct {
    SDL_Gamepad *p_gamepad;
    int has_gyro, has_accel;
    int has_found_gyro, has_found_accel, is_wanted, is_logged;
    float orientation[4], acceleration[3]; /* x y z w; g */
    int has_orientation;
    size_t c_reads, c_read_failures, c_enable_failures;
} s_sensors;
static int s_is_reporting=1, s_is_tilt_corrected, s_has_deadband;

static void set_identity(float q[4]) { q[0]=q[1]=q[2]=0.0f; q[3]=1.0f; }
static void normalize(float q[4]) {
    const float len=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if (len<1e-6f) { set_identity(q); return; }
    for (int i=0;i<4;++i) q[i]/=len;
}
/* The tilt that brings the measured up (a, unit) to the world's: the shortest turn, so with no yaw. */
static void seed_from_gravity(float q[4], const float a[3]) {
    if (a[1]<-0.999f) { q[0]=1.0f; q[1]=q[2]=q[3]=0.0f; return; } /* face down: half a turn about x */
    q[0]=-a[2]; q[1]=0.0f; q[2]=a[0]; q[3]=1.0f+a[1];
    normalize(q);
}
/* A pad with an accelerometer and no gyroscope (not a console pad) shows its tilt from gravity. */
static void tilt_from_gravity(void) {
    const float *p_a=s_sensors.acceleration;
    const float len=sqrtf(p_a[0]*p_a[0]+p_a[1]*p_a[1]+p_a[2]*p_a[2]);
    if (len<1e-3f) { set_identity(s_sensors.orientation); return; }
    const float a[3]={p_a[0]/len,p_a[1]/len,p_a[2]/len};
    seed_from_gravity(s_sensors.orientation,a);
}

static int enable_sensor(SDL_Gamepad *p_gamepad, SDL_SensorType type) {
    if (!SDL_GamepadHasSensor(p_gamepad,type)) return 0;
    if (SDL_SetGamepadSensorEnabled(p_gamepad,type,true)) return 1;
    ++s_sensors.c_enable_failures;
    printf("Runtime: gamepad sensors: enabling the %s failed: %s\n",type==SDL_SENSOR_GYRO ? "gyroscope" : "accelerometer",
           SDL_GetError());
    return 0;
}

void runtime_pad_sensors_open(SDL_Gamepad *p_sdl_gamepad) {
    s_sensors.p_gamepad=p_sdl_gamepad;
    s_sensors.has_gyro=s_sensors.has_accel=s_sensors.has_orientation=s_sensors.is_logged=0;
    set_identity(s_sensors.orientation);
    s_sensors.acceleration[0]=s_sensors.acceleration[2]=0.0f;
    s_sensors.acceleration[1]=1.0f;
    if (!p_sdl_gamepad) return;
    s_sensors.has_found_gyro=SDL_GamepadHasSensor(p_sdl_gamepad,SDL_SENSOR_GYRO);
    s_sensors.has_found_accel=SDL_GamepadHasSensor(p_sdl_gamepad,SDL_SENSOR_ACCEL);
    s_sensors.is_wanted=runtime_env_flag("PS_PAD_SENSORS",1);
    if (!s_sensors.is_wanted) return;
    s_sensors.has_gyro=enable_sensor(p_sdl_gamepad,SDL_SENSOR_GYRO);
    s_sensors.has_accel=enable_sensor(p_sdl_gamepad,SDL_SENSOR_ACCEL);
}

/* On the pad's first read rather than when it opens, because the motion emulation's setting is
 * read with the bindings, after the first pad is opened. */
static void log_sensors(void) {
    s_sensors.is_logged=1;
    const int has_gyro=s_sensors.has_found_gyro, has_accel=s_sensors.has_found_accel;
    const char *sz_found=has_gyro && has_accel ? "gyro+accel" : has_gyro ? "gyro" : has_accel ? "accel" : "none";
    const char *sz_fallback=runtime_pad_motion_is_on() ? "motion emulated" : "at rest";
    if (s_sensors.has_gyro || s_sensors.has_accel)
        printf("Runtime: gamepad sensors: %s (used%s)\n",sz_found,
               s_sensors.has_gyro && s_sensors.has_accel ? "" : s_sensors.has_gyro ? ", gyro only: no tilt correction"
                                                                                   : ", accel only: tilt, no turn");
    else if (has_gyro || has_accel)
        printf("Runtime: gamepad sensors: %s (%s): %s\n",sz_found,s_sensors.is_wanted ? "not enabled" : "PS_PAD_SENSORS=0",
               sz_fallback);
    else
        printf("Runtime: gamepad sensors: none: %s\n",sz_fallback);
}

static int read_sensor(SDL_SensorType type, float out[3]) {
    ++s_sensors.c_reads;
    if (SDL_GetGamepadSensorData(s_sensors.p_gamepad,type,out,3)) return 1;
    const size_t c_failures=++s_sensors.c_read_failures;
    if ((c_failures&(c_failures-1))==0)
        printf("Runtime: gamepad sensors: reading the %s failed (%zu failures): %s\n",
               type==SDL_SENSOR_GYRO ? "gyroscope" : "accelerometer",c_failures,SDL_GetError());
    return 0;
}

/* The world's up (0, 1, 0) in the pad's frame, q turning the pad's frame into the world's. */
static void up_in_frame(const float q[4], float out[3]) {
    const float x=q[0], y=q[1], z=q[2], w=q[3];
    out[0]=2.0f*(x*y+w*z);
    out[1]=1.0f-2.0f*(x*x+z*z);
    out[2]=2.0f*(y*z-w*x);
}

static void integrate(float rate[3], float dt) {
    float *q=s_sensors.orientation;
    const float *p_a=s_sensors.acceleration;
    const float len=sqrtf(p_a[0]*p_a[0]+p_a[1]*p_a[1]+p_a[2]*p_a[2]);
    float turn[3]={rate[0],rate[1],rate[2]};
    if (s_is_tilt_corrected && s_sensors.has_accel && fabsf(len-1.0f)<TILT_MAX_OFF_G) {
        float up[3];
        up_in_frame(q,up);
        const float a[3]={p_a[0]/len,p_a[1]/len,p_a[2]/len};
        const float axis[3]={a[1]*up[2]-a[2]*up[1],a[2]*up[0]-a[0]*up[2],a[0]*up[1]-a[1]*up[0]};
        if (sqrtf(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2])>TILT_MIN_AXIS) {
            /* libScePad turns min(dt/8 ms, 2) * 0.04 of the way per update: as a rate, that over dt. */
            const float gain=fminf(dt/0.008f,2.0f)*0.04f/dt;
            for (int i=0;i<3;++i) turn[i]+=gain*axis[i];
        }
    }
    const float hx=0.5f*turn[0]*dt, hy=0.5f*turn[1]*dt, hz=0.5f*turn[2]*dt;
    const float x=q[0], y=q[1], z=q[2], w=q[3];
    q[0]=x+w*hx+y*hz-z*hy;
    q[1]=y+w*hy-x*hz+z*hx;
    q[2]=z+w*hz+x*hy-y*hx;
    q[3]=w-x*hx-y*hy-z*hz;
    normalize(q);
}

int runtime_pad_sensors_read(PadData *p_pad_data, float dt) {
    if (!s_sensors.p_gamepad) return 0;
    if (!s_sensors.is_logged) log_sensors();
    if (!s_sensors.has_gyro && !s_sensors.has_accel) return 0;
    float rate[3]={0.0f,0.0f,0.0f}, accel[3];
    if (s_sensors.has_accel && read_sensor(SDL_SENSOR_ACCEL,accel))
        for (int i=0;i<3;++i) s_sensors.acceleration[i]=accel[i]/SDL_STANDARD_GRAVITY;
    if (s_sensors.has_gyro && !read_sensor(SDL_SENSOR_GYRO,rate)) rate[0]=rate[1]=rate[2]=0.0f;
    if (s_has_deadband)
        for (int i=0;i<3;++i) if (fabsf(rate[i])<DEADBAND_RAD_PER_S) rate[i]=0.0f;
    if (!s_sensors.has_gyro) tilt_from_gravity();
    else if (!s_sensors.has_orientation) s_sensors.has_orientation=1; /* the first read only latches the time */
    else if (dt>0.0f && dt<=MAX_STEP_S) integrate(rate,dt);
    memcpy(p_pad_data->orientation,s_sensors.orientation,sizeof(p_pad_data->orientation));
    memcpy(p_pad_data->acceleration,s_sensors.acceleration,sizeof(p_pad_data->acceleration));
    memcpy(p_pad_data->angular_velocity,rate,sizeof(p_pad_data->angular_velocity));
    return 1;
}

void runtime_pad_sensors_finish(PadData *p_pad_data) {
    if (s_is_reporting) return;
    set_identity(p_pad_data->orientation);
    p_pad_data->acceleration[0]=p_pad_data->acceleration[2]=0.0f;
    p_pad_data->acceleration[1]=1.0f;
    memset(p_pad_data->angular_velocity,0,sizeof(p_pad_data->angular_velocity));
}

void runtime_pad_sensors_set_reporting(int is_enabled) {
    if (s_is_reporting!=!!is_enabled) printf("Runtime: pad motion sensors %s by the game\n",is_enabled ? "on" : "off");
    s_is_reporting=!!is_enabled;
}
void runtime_pad_sensors_set_tilt_correction(int is_enabled) {
    if (s_is_tilt_corrected!=!!is_enabled) printf("Runtime: pad tilt correction %s by the game\n",is_enabled ? "on" : "off");
    s_is_tilt_corrected=!!is_enabled;
}
void runtime_pad_sensors_set_deadband(int is_enabled) { s_has_deadband=!!is_enabled; }
void runtime_pad_sensors_reset_orientation(void) { set_identity(s_sensors.orientation); }

void runtime_pad_sensors_report(void) {
    if (!s_sensors.c_reads && !s_sensors.c_enable_failures) return;
    printf("Runtime: pad sensors: %s%s%s, reads=%zu, read failures=%zu, enable failures=%zu\n",
           s_sensors.has_gyro ? "gyro" : "", s_sensors.has_gyro && s_sensors.has_accel ? "+" : "",
           s_sensors.has_accel ? "accel" : s_sensors.has_gyro ? "" : "none",s_sensors.c_reads,s_sensors.c_read_failures,
           s_sensors.c_enable_failures);
}
