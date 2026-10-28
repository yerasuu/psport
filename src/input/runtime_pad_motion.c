// SPDX-License-Identifier: GPL-2.0-or-later
/* Motion for controllers without sensors. Some games read the DualShock 4's sensors: one asks to
 * hold the controller sideways like a spray can, shake it and aim by tilting it, others to swipe
 * the touchpad. Keyboard and mouse:
 *   G                take the can (the controller held sideways), G again to put it away;
 *   the mouse        aims while the can is held (the camera does not turn meanwhile);
 *   left button      sprays (R2) while the can is held;
 *   H, held          shakes the can;
 *   T                brings the aim back to the middle;
 *   a touchpad key + the mouse  moves the finger that key put down (a swipe); both touchpad keys
 *                    and the mouse move two fingers.
 * A gamepad (one without sensors): View is a modifier, View + A the can, View + B shaking, the
 * right stick aims while the can is held; View + the right stick otherwise swipes the touchpad
 * that way with one finger, View + the left stick with two (one on each half); a short press of
 * View alone is the touchpad, on release.
 * Reported consistently: the orientation quaternion, gravity in the controller's frame (1 g up
 * when flat, plus the shake) and the angular velocity of the aim. Off unless psport.ini has
 * pad_motion=1 or PS_PAD_MOTION=1 (which wins over the file); off, nothing changes. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_pad.h"
#include "gpu/psport_gpu.h"
#include <math.h>
#include <stdio.h>

#define MOTION_KEY_HOLD PS_HOST_INPUT_KEY_G
#define MOTION_KEY_SHAKE PS_HOST_INPUT_KEY_H
#define MOTION_KEY_CENTER PS_HOST_INPUT_KEY_T
#define TOUCH_WIDTH 1919.0f
#define TOUCH_HEIGHT 942.0f
#define AIM_LIMIT_RAD 0.61f /* 35 degrees each way: further, the game's aim leaves the picture */
#define PAD_AIM_RAD_PER_S 1.2f
#define MOUSE_AIM_RAD_PER_PIXEL 0.0012f /* ~800 pixels across the 70 degrees at sensitivity 1 */
#define SWIPE_PX_PER_S 6400.0f
#define VIEW_TAP_US 120000u

static int s_is_enabled;
static struct {
    int is_view_held, is_view_used, was_cross_down, was_hold_key_down, is_sideways, is_shaking;
    float yaw, pitch, prev_yaw, prev_pitch; /* the aim, radians */
    uint64_t last_us, click_until_us;
    /* A swipe from a stick: the finger goes from one side of the touchpad through the middle to
     * the other, in the stick's direction. */
    int is_swiping, c_swipe_fingers;
    float swipe_x, swipe_y, swipe_end_x, swipe_end_y;
    /* A touchpad key + the mouse: how far the mouse has moved the finger since the key went down. */
    float drag_x, drag_y;
} s_motion;

void runtime_pad_motion_configure(int configured) {
    static int s_is_logged;
    const char *sz_env=runtime_env("PS_PAD_MOTION","");
    s_is_enabled=*sz_env ? sz_env[0]=='1' : configured>0;
    if (s_is_enabled && !s_is_logged) {
        s_is_logged=1;
        printf("Runtime: pad motion emulation on (G: spray can, mouse aims, left button sprays, H shakes, T centers, "
               "touchpad key + mouse swipes; pad: View+A can, View+B shakes, right stick aims, View+right stick "
               "swipes, View+left stick two fingers, View tap touchpad)\n");
    }
}
int runtime_pad_motion_is_on(void) { return s_is_enabled; }

float runtime_pad_motion_begin(uint64_t timestamp_us) {
    const float dt=s_motion.last_us && timestamp_us>s_motion.last_us ? (float)(timestamp_us-s_motion.last_us)*1e-6f : 0.0f;
    s_motion.last_us=timestamp_us;
    s_motion.is_shaking=0;
    return dt;
}

static float clamp(float value, float low, float high) { return fmaxf(low,fminf(high,value)); }
static void set_sideways(int is_sideways) {
    s_motion.is_sideways=is_sideways;
    s_motion.yaw=s_motion.pitch=s_motion.prev_yaw=s_motion.prev_pitch=0.0f;
    if (is_sideways) psport_gpu_mouse_capture(); /* the mouse aims now: it must not leave the window */
    printf("Runtime: pad motion: %s\n",is_sideways ? "spray can taken (controller sideways)" : "spray can put away");
}
/* The controller is held rolled 90 degrees, so the turn that moves the game's aim across the
 * screen is its pitch, and up and down its yaw. */
static void aim_screen(float right_rad, float up_rad) {
    s_motion.yaw=clamp(s_motion.yaw+up_rad,-AIM_LIMIT_RAD,AIM_LIMIT_RAD);
    s_motion.pitch=clamp(s_motion.pitch-right_rad,-AIM_LIMIT_RAD,AIM_LIMIT_RAD);
}
static float stick_unit(uint8_t value) { return ((int)value-128)/127.0f; }

/* View + a stick: a one-finger swipe (right stick) or two fingers, one on each half (left stick),
 * a quarter of a second across the touchpad. */
static void swipe_from_sticks(PadData *p_pad_data, float dt) {
    const float rx=stick_unit(p_pad_data->right_x), ry=stick_unit(p_pad_data->right_y);
    const float lx=stick_unit(p_pad_data->left_x), ly=stick_unit(p_pad_data->left_y);
    const int is_two=sqrtf(rx*rx+ry*ry)<=0.5f && sqrtf(lx*lx+ly*ly)>0.5f;
    const float sx=is_two ? lx : rx, sy=is_two ? ly : ry;
    const float len=sqrtf(sx*sx+sy*sy);
    p_pad_data->right_x=p_pad_data->right_y=128; /* not the camera */
    p_pad_data->left_x=p_pad_data->left_y=128;   /* nor walking */
    if (len<=0.5f) { s_motion.is_swiping=0; return; }
    if (!s_motion.is_swiping || s_motion.c_swipe_fingers!=(is_two ? 2 : 1)) {
        const float dx=sx/len, dy=sy/len;
        const float reach_x=is_two ? 400.0f : 800.0f; /* each finger stays on its half */
        s_motion.is_swiping=1;
        s_motion.c_swipe_fingers=is_two ? 2 : 1;
        s_motion.swipe_x=960.0f-dx*reach_x; s_motion.swipe_y=471.0f-dy*400.0f;
        s_motion.swipe_end_x=960.0f+dx*reach_x; s_motion.swipe_end_y=471.0f+dy*400.0f;
    }
    const float step=SWIPE_PX_PER_S*dt;
    const float ex=s_motion.swipe_end_x-s_motion.swipe_x, ey=s_motion.swipe_end_y-s_motion.swipe_y;
    const float left=sqrtf(ex*ex+ey*ey);
    if (left>step) { s_motion.swipe_x+=ex/left*step; s_motion.swipe_y+=ey/left*step; }
    else { s_motion.swipe_x=s_motion.swipe_end_x; s_motion.swipe_y=s_motion.swipe_end_y; }
    const uint16_t y=(uint16_t)clamp(s_motion.swipe_y,0.0f,TOUCH_HEIGHT);
    if (s_motion.c_swipe_fingers==2) {
        p_pad_data->touch_count=0;
        p_pad_data->touches[p_pad_data->touch_count++]=(PadTouch){.x=(uint16_t)clamp(s_motion.swipe_x-480.0f,0.0f,TOUCH_WIDTH),.y=y,.id=0};
        p_pad_data->touches[p_pad_data->touch_count++]=(PadTouch){.x=(uint16_t)clamp(s_motion.swipe_x+480.0f,0.0f,TOUCH_WIDTH),.y=y,.id=1};
    } else if (p_pad_data->touch_count<2) {
        p_pad_data->touches[p_pad_data->touch_count++]=(PadTouch){.x=(uint16_t)clamp(s_motion.swipe_x,0.0f,TOUCH_WIDTH),.y=y,.id=0};
    }
    s_motion.is_view_used=1;
}

void runtime_pad_motion_from_pad(PadData *p_pad_data, PsHostPad *p_host_pad, float dt) {
    const int is_view=ps_host_input_pad_button(p_host_pad,PS_HOST_PAD_BUTTON_BACK);
    const int is_real_touchpad=ps_host_input_pad_button(p_host_pad,PS_HOST_PAD_BUTTON_TOUCHPAD);
    const int is_cross=(p_pad_data->buttons & BTN_CROSS)!=0, is_circle=(p_pad_data->buttons & BTN_CIRCLE)!=0;
    if (is_view) {
        if (!s_motion.is_view_held) { s_motion.is_view_held=1; s_motion.is_view_used=0; s_motion.was_cross_down=is_cross; }
        if (is_cross && !s_motion.was_cross_down) { set_sideways(!s_motion.is_sideways); s_motion.is_view_used=1; }
        if (is_circle) { s_motion.is_shaking=1; s_motion.is_view_used=1; }
        s_motion.was_cross_down=is_cross;
        /* View is the modifier while held: neither a touchpad press nor A or B for the game. */
        p_pad_data->buttons&=~(uint32_t)(BTN_CROSS|BTN_CIRCLE);
        if (!is_real_touchpad) p_pad_data->buttons&=~(uint32_t)BTN_TOUCHPAD;
    } else if (s_motion.is_view_held) {
        s_motion.is_view_held=0;
        if (!s_motion.is_view_used) s_motion.click_until_us=p_pad_data->timestamp+VIEW_TAP_US;
    }
    if (!is_view && p_pad_data->timestamp<s_motion.click_until_us) p_pad_data->buttons|=BTN_TOUCHPAD;
    if (s_motion.is_sideways) {
        const float rx=stick_unit(p_pad_data->right_x), ry=stick_unit(p_pad_data->right_y);
        if (fabsf(rx)>0.05f || fabsf(ry)>0.05f) {
            aim_screen(rx*PAD_AIM_RAD_PER_S*dt,-ry*PAD_AIM_RAD_PER_S*dt);
            if (is_view) s_motion.is_view_used=1;
        }
        p_pad_data->right_x=p_pad_data->right_y=128;
    } else if (is_view) {
        swipe_from_sticks(p_pad_data,dt);
    }
    if (!is_view) s_motion.is_swiping=0;
}

void runtime_pad_motion_from_desktop(PadData *p_pad_data, const bool *p_is_key_down, int is_touch_key_down,
                                     int is_mouse_left_down, float mouse_sensitivity, double *p_dx, double *p_dy,
                                     int is_captured) {
    const int is_hold_key=p_is_key_down && p_is_key_down[MOTION_KEY_HOLD];
    if (is_hold_key && !s_motion.was_hold_key_down) set_sideways(!s_motion.is_sideways);
    s_motion.was_hold_key_down=is_hold_key;
    if (p_is_key_down && p_is_key_down[MOTION_KEY_SHAKE]) s_motion.is_shaking=1;
    if (is_touch_key_down && p_pad_data->touch_count>0) {
        if (is_captured) {
            s_motion.drag_x+=(float)*p_dx*2.0f;
            s_motion.drag_y+=(float)*p_dy*2.0f;
            *p_dx=*p_dy=0; /* not the camera */
        }
        for (int i=0;i<p_pad_data->touch_count;++i) {
            PadTouch *p_touch=&p_pad_data->touches[i];
            p_touch->x=(uint16_t)clamp((float)p_touch->x+s_motion.drag_x,0.0f,TOUCH_WIDTH);
            p_touch->y=(uint16_t)clamp((float)p_touch->y+s_motion.drag_y,0.0f,TOUCH_HEIGHT);
        }
    } else {
        s_motion.drag_x=s_motion.drag_y=0.0f;
    }
    if (!s_motion.is_sideways) return;
    if (p_is_key_down && p_is_key_down[MOTION_KEY_CENTER]) s_motion.yaw=s_motion.pitch=0.0f;
    if (!is_captured) return;
    const float per_pixel=MOUSE_AIM_RAD_PER_PIXEL*mouse_sensitivity;
    aim_screen((float)*p_dx*per_pixel,-(float)*p_dy*per_pixel);
    *p_dx=*p_dy=0; /* not the camera */
    if (is_mouse_left_down) {
        p_pad_data->buttons&=~(uint32_t)BTN_R1; /* the left button's usual attack */
        p_pad_data->buttons|=BTN_R2;
        p_pad_data->r2=255;
    }
}

static void multiply_quaternions(const float a[4], const float b[4], float out[4]) { /* x y z w */
    out[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    out[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    out[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    out[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
/* The world's up (0, 1, 0) in the frame of the controller turned by q (q's inverse applied). */
static void up_in_frame(const float q[4], float out[3]) {
    const float x=q[0], y=q[1], z=q[2], w=q[3];
    out[0]=2.0f*(x*y+w*z);
    out[1]=1.0f-2.0f*(x*x+z*z);
    out[2]=2.0f*(y*z-w*x);
}

void runtime_pad_motion_report(PadData *p_pad_data, float dt) {
    const float s=0.70710678f;
    /* The can: rolled 90 degrees about the controller's front-back axis; then the aim. */
    const float roll[4]={0.0f,0.0f,s_motion.is_sideways ? s : 0.0f,s_motion.is_sideways ? s : 1.0f};
    const float yaw[4]={0.0f,sinf(s_motion.yaw*0.5f),0.0f,cosf(s_motion.yaw*0.5f)};
    const float pitch[4]={sinf(s_motion.pitch*0.5f),0.0f,0.0f,cosf(s_motion.pitch*0.5f)};
    float tilt[4], q[4], up[3];
    multiply_quaternions(yaw,pitch,tilt);
    multiply_quaternions(roll,tilt,q);
    for (int i=0;i<4;++i) p_pad_data->orientation[i]=q[i];
    up_in_frame(q,up);
    for (int i=0;i<3;++i) p_pad_data->acceleration[i]=up[i];
    if (s_motion.is_shaking) {
        /* 2.5 g back and forth along the controller's long axis, 8 times a second. */
        p_pad_data->acceleration[0]+=2.5f*sinf((float)(p_pad_data->timestamp%1000000)*1e-6f*2.0f*3.14159265f*8.0f);
    }
    p_pad_data->angular_velocity[0]=dt>0.0f ? (s_motion.pitch-s_motion.prev_pitch)/dt : 0.0f;
    p_pad_data->angular_velocity[1]=dt>0.0f ? (s_motion.yaw-s_motion.prev_yaw)/dt : 0.0f;
    p_pad_data->angular_velocity[2]=0.0f;
    s_motion.prev_yaw=s_motion.yaw; s_motion.prev_pitch=s_motion.pitch;
}
