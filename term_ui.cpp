#include "term_ui.h"
#include "term_pty.h"
#include "ft_font.h"
#include "gl_renderer.h"
#include "term_color.h"
#include "gl_terminal.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <unistd.h>    // readlink, setsid, execl, _exit, fork
#include <sys/types.h> // pid_t


// ============================================================================
// FIGHT CLUB — background stick figure fighting simulation
// ============================================================================

static bool s_fight_enabled = false;

// ── pose keyframe: angles in radians, body offsets in pixels ────────────────
// All angles: 0 = straight down from joint, positive = forward (toward enemy)
struct Pose {
    // body
    float lean;          // torso lean forward (rad)
    float crouch;        // hip drop (pixels, positive = lower)
    // right arm (attack arm — mirrored for left-facing)
    float rua;           // upper arm angle from shoulder
    float rla;           // lower arm angle from elbow (relative to upper)
    // left arm (guard arm)
    float lua, lla;
    // right leg
    float rul, rll;
    // left leg
    float lul, lll;
    // head tilt
    float head;
};

// lerp two poses
static Pose pose_lerp(const Pose &a, const Pose &b, float t) {
    auto lf = [&](float x, float y){ return x + (y-x)*t; };
    return { lf(a.lean,b.lean), lf(a.crouch,b.crouch),
             lf(a.rua,b.rua), lf(a.rla,b.rla),
             lf(a.lua,b.lua), lf(a.lla,b.lla),
             lf(a.rul,b.rul), lf(a.rll,b.rll),
             lf(a.lul,b.lul), lf(a.lll,b.lll),
             lf(a.head,b.head) };
}

// ── named poses ─────────────────────────────────────────────────────────────
static const Pose P_NEUTRAL  = { 0.0f, 0,   -0.3f, 0.5f,  0.3f, 0.4f,  0.15f,-0.2f,  -0.15f, 0.2f,  0.0f };
static const Pose P_GUARD    = { 0.1f, 4,   -0.8f, 1.1f,  0.7f, 1.0f,  0.2f, -0.3f,  -0.2f,  0.3f,  0.05f};
// jab: wind-up → full extension → recovery
static const Pose P_JAB_WIND = { 0.15f,4,   -1.2f, 0.3f,  0.6f, 1.0f,  0.2f, -0.25f, -0.2f,  0.25f, 0.1f };
static const Pose P_JAB_EXT  = { 0.3f, 2,    0.1f,-0.1f,  0.5f, 0.8f,  0.3f, -0.4f,  -0.3f,  0.4f,  0.15f};
// cross: heavy straight, big lean
static const Pose P_CROSS_W  = { 0.05f,6,   -1.4f, 0.4f,  0.4f, 0.9f,  0.3f, -0.5f,  -0.25f, 0.4f,  0.05f};
static const Pose P_CROSS_E  = { 0.5f, 3,    0.2f,-0.15f, 0.3f, 0.6f,  0.4f, -0.5f,  -0.35f, 0.5f,  0.2f };
// hook: swing around
static const Pose P_HOOK_W   = { 0.1f, 5,    1.5f, 0.8f,  0.6f, 1.0f,  0.2f, -0.3f,  -0.25f, 0.35f, 0.0f };
static const Pose P_HOOK_E   = { 0.2f, 3,   -0.2f, 1.6f,  0.4f, 0.7f,  0.3f, -0.4f,  -0.3f,  0.4f,  0.1f };
// uppercut: dip then launch upward
static const Pose P_UPC_W    = {-0.1f,10,   -0.8f, 1.2f,  0.5f, 0.9f,  0.15f,-0.2f,  -0.2f,  0.3f, -0.1f };
static const Pose P_UPC_E    = { 0.2f, 0,   -1.8f,-0.4f,  0.6f, 0.8f,  0.2f, -0.3f,  -0.15f, 0.25f, 0.2f };
// roundhouse kick
static const Pose P_KICK_W   = { 0.2f, 2,   -0.5f, 0.6f,  0.8f, 1.2f, -0.5f, 0.2f,   0.8f,  -1.2f,  0.1f };
static const Pose P_KICK_E   = { 0.3f, 2,   -0.4f, 0.5f,  0.7f, 1.1f, -0.2f, 0.1f,   1.6f,  -0.5f,  0.15f};
// sweep kick (low)
static const Pose P_SWEEP_W  = { 0.3f, 8,   -0.3f, 0.4f,  0.9f, 1.3f,  0.4f, 0.1f,   2.0f,   0.2f,  0.05f};
static const Pose P_SWEEP_E  = { 0.1f, 6,   -0.2f, 0.3f,  0.8f, 1.2f,  0.2f, 0.0f,   2.4f,  -0.3f,  0.0f };
// block
static const Pose P_BLOCK    = { 0.05f,6,   -0.6f, 1.3f,  0.65f,1.2f,  0.25f,-0.35f, -0.25f, 0.35f, -0.05f};
// hurt recoil
static const Pose P_HURT     = {-0.2f, 2,    0.8f, 0.6f, -0.6f, 0.5f,  0.1f, -0.15f, -0.1f,  0.15f, -0.2f};
// walk cycle keyframes
static const Pose P_WALK_A   = { 0.08f,2,   -0.4f, 0.6f,  0.5f, 0.7f,  0.5f, -0.6f,  -0.5f,  0.6f,  0.05f};
static const Pose P_WALK_B   = { 0.08f,2,   -0.2f, 0.4f,  0.3f, 0.5f, -0.5f,  0.6f,   0.5f,  -0.6f,  0.05f};
// in air
static const Pose P_AIR      = { 0.1f, 0,   -0.7f, 0.9f,  0.6f, 0.9f, -0.6f, 0.5f,   0.5f,  -0.4f,  0.1f };
// death collapse
static const Pose P_DEAD_MID = {-0.4f,14,    1.0f, 1.4f, -0.8f, 0.4f,  0.6f,  0.8f,  -0.3f,  0.5f, -0.3f};
static const Pose P_DEAD_END = {-1.4f,28,    1.5f, 1.8f, -1.2f, 0.6f,  1.2f,  1.6f,   0.4f,  1.0f, -0.5f};

// ── attack sub-types ─────────────────────────────────────────────────────────
enum AttackType { ATK_JAB, ATK_CROSS, ATK_HOOK, ATK_UPPERCUT, ATK_KICK, ATK_SWEEP };

// ── fighter state ─────────────────────────────────────────────────────────────
enum FState { FS_IDLE, FS_WALK, FS_ATTACK, FS_BLOCK, FS_HURT, FS_JUMP, FS_DEAD };

struct FightFighter {
    float x, y, vx, vy, hp;
    bool  facing_right, dead;
    float dead_timer, anim;
    FState state;
    int   state_timer, state_dur;  // timer counts down from dur
    int   atk_cd, special_cd;
    AttackType cur_atk;
    Pose  cur_pose;   // interpolated each frame for smooth rendering
    Pose  pose_target;
    float pose_t;     // 0..1 where we are in pose blend
    float cr, cg, cb;
    // squash/stretch
    float body_scale_y;
};

static FightFighter s_ff[2];
static bool  s_fight_inited = false;
static float s_fight_ww = 800.f;
static float s_fight_wh = 600.f;
static int   s_fight_ticks = 0;

struct BloodParticle { float x,y,vx,vy,life,maxlife,r,cr,cg,cb; };
static std::vector<BloodParticle> s_blood;

static void fight_spawn_blood(float x, float y, float cr, float cg, float cb, int n) {
    for (int i=0;i<n;i++) {
        float angle=((float)rand()/RAND_MAX)*6.2831f;
        float spd=2.f+((float)rand()/RAND_MAX)*5.f;
        s_blood.push_back({x,y,cosf(angle)*spd,sinf(angle)*spd-2.5f,
                           40.f+((float)rand()/RAND_MAX)*25.f,65.f,
                           2.f+((float)rand()/RAND_MAX)*3.f,cr,cg,cb});
    }
}

static void fight_init(float ww, float wh) {
    s_fight_ww=ww; s_fight_wh=wh;
    float fy=wh*0.82f;
    s_ff[0]={}; s_ff[0].x=ww*.25f; s_ff[0].y=fy; s_ff[0].hp=1.f;
    s_ff[0].facing_right=true; s_ff[0].cr=1.f; s_ff[0].cg=0.25f; s_ff[0].cb=0.25f;
    s_ff[0].cur_pose=P_GUARD; s_ff[0].pose_target=P_GUARD; s_ff[0].body_scale_y=1.f;
    s_ff[1]={}; s_ff[1].x=ww*.75f; s_ff[1].y=fy; s_ff[1].hp=1.f;
    s_ff[1].facing_right=false; s_ff[1].cr=0.25f; s_ff[1].cg=0.5f; s_ff[1].cb=1.f;
    s_ff[1].cur_pose=P_GUARD; s_ff[1].pose_target=P_GUARD; s_ff[1].body_scale_y=1.f;
    s_blood.clear(); s_fight_ticks=0; s_fight_inited=true;
}

// ── drawing primitives ───────────────────────────────────────────────────────
static void fight_line(float x1,float y1,float x2,float y2,
                        float cr,float cg,float cb,float a,float thick=3.f) {
    float dx=x2-x1,dy=y2-y1,len=sqrtf(dx*dx+dy*dy);
    if(len<0.5f) return;
    float nx=-dy/len*thick*.5f, ny=dx/len*thick*.5f;
    Vertex v[6];
    auto mk=[&](Vertex &vv,float vx,float vy){vv={vx,vy,cr,cg,cb,a};};
    mk(v[0],x1+nx,y1+ny);mk(v[1],x1-nx,y1-ny);mk(v[2],x2-nx,y2-ny);
    mk(v[3],x1+nx,y1+ny);mk(v[4],x2-nx,y2-ny);mk(v[5],x2+nx,y2+ny);
    draw_verts(v,6,GL_TRIANGLES);
}
static void fight_circle(float x,float y,float r,float cr,float cg,float cb,float a) {
    const int S=16; Vertex v[S*3];
    for(int i=0;i<S;i++){
        float a0=(i/(float)S)*6.2831f,a1=((i+1)/(float)S)*6.2831f;
        v[i*3]={x,y,cr,cg,cb,a};
        v[i*3+1]={x+cosf(a0)*r,y+sinf(a0)*r,cr,cg,cb,a};
        v[i*3+2]={x+cosf(a1)*r,y+sinf(a1)*r,cr,cg,cb,a};
    }
    draw_verts(v,S*3,GL_TRIANGLES);
}

// 2-joint limb: returns end-effector position
static void fight_2joint(float ox,float oy,
                          float upper_ang, float lower_ang_rel, float upper_len, float lower_len,
                          float cr,float cg,float cb,float a, float thick,
                          float *ex=nullptr,float *ey=nullptr) {
    float kx=ox+sinf(upper_ang)*upper_len;
    float ky=oy+cosf(upper_ang)*upper_len;
    float total_ang=upper_ang+lower_ang_rel;
    float endx=kx+sinf(total_ang)*lower_len;
    float endy=ky+cosf(total_ang)*lower_len;
    fight_line(ox,oy,kx,ky,cr,cg,cb,a,thick);
    fight_line(kx,ky,endx,endy,cr,cg,cb,a,thick);
    if(ex)*ex=endx; if(ey)*ey=endy;
}

static void fight_draw_fighter(const FightFighter &f, float alpha) {
    float flip = f.facing_right ? 1.f : -1.f;
    const Pose &p = f.cur_pose;
    float x = f.x, base_y = f.y;
    float sy = f.body_scale_y;

    float cr=f.cr,cg=f.cg,cb=f.cb;
    float dcr=cr*.65f,dcg=cg*.65f,dcb=cb*.65f; // darker for back limbs

    // body geometry
    float head_r   = 11.f;
    float torso_h  = 34.f * sy;
    float uarm_l   = 21.f;
    float larm_l   = 18.f;
    float uleg_l   = 25.f;
    float lleg_l   = 23.f;

    // hip position (drops with crouch, affected by body scale)
    float hip_y    = base_y - 4.f;
    float neck_y   = hip_y - torso_h - p.crouch * sy;
    float head_cy  = neck_y - head_r;

    // torso with lean: shoulder offset from hip
    float shoulder_x = x + sinf(p.lean) * torso_h * flip;
    float shoulder_y = neck_y;

    // draw torso
    fight_line(x, hip_y, shoulder_x, shoulder_y, cr, cg, cb, alpha, 3.5f);

    // head
    float head_x = shoulder_x + sinf(p.head)*head_r*flip;
    float hcy    = head_cy - p.crouch*0.3f*sy;
    fight_circle(head_x, hcy, head_r, cr, cg, cb, alpha);
    // eyes
    fight_circle(head_x+flip*3.5f, hcy-2.f, 2.f, 0,0,0, alpha);
    fight_circle(head_x+flip*7.5f, hcy-2.f, 2.f, 0,0,0, alpha);

    // BACK arm (darker, drawn first)
    {
        float ua = (p.lua + p.lean) * flip;
        float ex,ey;
        fight_2joint(shoulder_x,shoulder_y, ua, p.lla*flip,
                     uarm_l,larm_l, dcr,dcg,dcb,alpha*.75f,2.5f,&ex,&ey);
        fight_circle(ex,ey,3.5f,dcr,dcg,dcb,alpha*.75f);
    }
    // BACK leg (darker, drawn first)
    {
        float ul = p.lul * flip;
        float ex,ey;
        fight_2joint(x,hip_y, ul, p.lll*flip, uleg_l,lleg_l,
                     dcr,dcg,dcb,alpha*.75f,3.f,&ex,&ey);
        // foot
        fight_line(ex,ey,ex+flip*11.f,ey,dcr,dcg,dcb,alpha*.75f,2.f);
    }
    // FRONT leg
    {
        float ul = p.rul * flip;
        float ex,ey;
        fight_2joint(x,hip_y, ul, p.rll*flip, uleg_l,lleg_l,
                     cr,cg,cb,alpha,3.f,&ex,&ey);
        fight_line(ex,ey,ex+flip*11.f,ey,cr,cg,cb,alpha,2.f);
    }
    // FRONT arm (attack arm — brighter)
    {
        float ua = (p.rua + p.lean) * flip;
        float ex,ey;
        fight_2joint(shoulder_x,shoulder_y, ua, p.rla*flip,
                     uarm_l,larm_l, cr,cg,cb,alpha,3.f,&ex,&ey);
        fight_circle(ex,ey,4.f,cr,cg,cb,alpha);  // fist
    }
}

// ── attack logic ──────────────────────────────────────────────────────────────
static void fight_do_attack(FightFighter &me, FightFighter &enemy, AttackType atk) {
    if (enemy.dead) return;  // don't attack corpses
    float dist=fabsf(me.x-enemy.x);
    float range = (atk==ATK_KICK||atk==ATK_SWEEP) ? 105.f : 85.f;
    if (dist < range) {
        float dmg;
        switch(atk) {
            case ATK_JAB:      dmg=0.05f+((float)rand()/RAND_MAX)*0.04f; break;
            case ATK_CROSS:    dmg=0.09f+((float)rand()/RAND_MAX)*0.06f; break;
            case ATK_HOOK:     dmg=0.10f+((float)rand()/RAND_MAX)*0.06f; break;
            case ATK_UPPERCUT: dmg=0.13f+((float)rand()/RAND_MAX)*0.07f; break;
            case ATK_KICK:     dmg=0.14f+((float)rand()/RAND_MAX)*0.08f; break;
            case ATK_SWEEP:    dmg=0.11f+((float)rand()/RAND_MAX)*0.06f; break;
            default:           dmg=0.07f; break;
        }
        if (enemy.state==FS_BLOCK) dmg*=0.12f;
        enemy.hp=fmaxf(0.f,enemy.hp-dmg);
        enemy.state=FS_HURT; enemy.state_timer=14; enemy.state_dur=14;
        float dir=(enemy.x>me.x)?1.f:-1.f;
        float kb = (atk==ATK_UPPERCUT||atk==ATK_KICK) ? 9.f : 4.f;
        enemy.vx+=dir*kb;
        if (atk==ATK_UPPERCUT) enemy.vy=-7.f;
        if (atk==ATK_SWEEP)    enemy.vy=-3.f;
        fight_spawn_blood(enemy.x,enemy.y-55.f,enemy.cr,enemy.cg,enemy.cb,
                          (atk==ATK_KICK||atk==ATK_UPPERCUT)?10:4);
        if (enemy.hp<=0.f) {
            enemy.dead=true; enemy.state=FS_DEAD; enemy.dead_timer=0.f;
            // launch them backward and up so they fall dramatically
            float kdir=(enemy.x>me.x)?1.f:-1.f;
            enemy.vx=kdir*7.f;
            enemy.vy=-5.f;  // pop up before falling
        }
    }
    int dur;
    switch(atk) {
        case ATK_JAB:   dur=18; me.atk_cd=22; break;
        case ATK_CROSS: dur=26; me.atk_cd=34; break;
        case ATK_HOOK:  dur=24; me.atk_cd=30; break;
        case ATK_UPPERCUT: dur=30; me.atk_cd=38; me.special_cd=100; break;
        case ATK_KICK:  dur=28; me.atk_cd=36; me.special_cd=80;  break;
        case ATK_SWEEP: dur=32; me.atk_cd=40; me.special_cd=90;  break;
        default: dur=20; me.atk_cd=25; break;
    }
    me.cur_atk=atk;
    me.state=FS_ATTACK; me.state_timer=dur; me.state_dur=dur;
}

// ── smooth pose update each tick ─────────────────────────────────────────────
static void fight_update_pose(FightFighter &me) {
    float floor_y=s_fight_wh*0.82f;
    bool on_floor=(me.y>=floor_y-1.f);
    float ph=me.anim;

    Pose target;
    float blend_speed;

    if (me.dead) {
        float dt=fminf(me.dead_timer/40.f,1.f);
        target=pose_lerp(P_DEAD_MID,P_DEAD_END,dt);
        blend_speed=0.12f;
    } else {
        float t=me.state_timer/(float)(me.state_dur>0?me.state_dur:1);
        float progress=1.f-t; // 0=start, 1=end
        switch(me.state) {
        case FS_IDLE:
            // subtle breathing sway
            target=P_GUARD;
            target.lean+=sinf(ph*0.3f)*0.04f;
            target.crouch+=sinf(ph*0.25f+1.f)*1.5f;
            blend_speed=0.08f;
            break;
        case FS_WALK: {
            // oscillate between two walk poses
            float wt=(sinf(ph*2.2f)*0.5f+0.5f);
            target=pose_lerp(P_WALK_A,P_WALK_B,wt);
            blend_speed=0.18f;
            break;
        }
        case FS_JUMP:
            target=P_AIR;
            // tuck legs when rising, extend when falling
            if(me.vy<0.f) {
                target.rul=-0.8f; target.rll=0.8f;
                target.lul=0.7f;  target.lll=-0.6f;
            }
            blend_speed=0.2f;
            break;
        case FS_BLOCK:
            target=P_BLOCK;
            // slight bob
            target.crouch+=sinf(ph*1.5f)*1.f;
            blend_speed=0.25f;
            break;
        case FS_HURT:
            target=P_HURT;
            blend_speed=0.35f;
            break;
        case FS_ATTACK:
            // each attack type has wind-up → extension → recovery phases
            switch(me.cur_atk) {
            case ATK_JAB:
                if(progress<0.3f)      target=pose_lerp(P_GUARD,P_JAB_WIND,progress/0.3f);
                else if(progress<0.55f) target=pose_lerp(P_JAB_WIND,P_JAB_EXT,(progress-0.3f)/0.25f);
                else                   target=pose_lerp(P_JAB_EXT,P_GUARD,(progress-0.55f)/0.45f);
                break;
            case ATK_CROSS:
                if(progress<0.25f)     target=pose_lerp(P_GUARD,P_CROSS_W,progress/0.25f);
                else if(progress<0.55f) target=pose_lerp(P_CROSS_W,P_CROSS_E,(progress-0.25f)/0.3f);
                else                   target=pose_lerp(P_CROSS_E,P_GUARD,(progress-0.55f)/0.45f);
                break;
            case ATK_HOOK:
                if(progress<0.3f)      target=pose_lerp(P_GUARD,P_HOOK_W,progress/0.3f);
                else if(progress<0.6f)  target=pose_lerp(P_HOOK_W,P_HOOK_E,(progress-0.3f)/0.3f);
                else                   target=pose_lerp(P_HOOK_E,P_GUARD,(progress-0.6f)/0.4f);
                break;
            case ATK_UPPERCUT:
                if(progress<0.25f)     target=pose_lerp(P_GUARD,P_UPC_W,progress/0.25f);
                else if(progress<0.55f) target=pose_lerp(P_UPC_W,P_UPC_E,(progress-0.25f)/0.3f);
                else                   target=pose_lerp(P_UPC_E,P_GUARD,(progress-0.55f)/0.45f);
                break;
            case ATK_KICK:
                if(progress<0.3f)      target=pose_lerp(P_GUARD,P_KICK_W,progress/0.3f);
                else if(progress<0.65f) target=pose_lerp(P_KICK_W,P_KICK_E,(progress-0.3f)/0.35f);
                else                   target=pose_lerp(P_KICK_E,P_GUARD,(progress-0.65f)/0.35f);
                break;
            case ATK_SWEEP:
                if(progress<0.25f)     target=pose_lerp(P_GUARD,P_SWEEP_W,progress/0.25f);
                else if(progress<0.6f)  target=pose_lerp(P_SWEEP_W,P_SWEEP_E,(progress-0.25f)/0.35f);
                else                   target=pose_lerp(P_SWEEP_E,P_GUARD,(progress-0.6f)/0.4f);
                break;
            default: target=P_GUARD; break;
            }
            blend_speed=0.3f;
            break;
        default:
            target=P_GUARD; blend_speed=0.1f; break;
        }
    }

    // squash on land
    if (on_floor && me.vy > 3.f) {
        me.body_scale_y = fmaxf(0.75f, 1.f - me.vy*0.04f);
    } else {
        me.body_scale_y += (1.f - me.body_scale_y) * 0.15f;
    }

    // smooth blend toward target
    me.cur_pose = pose_lerp(me.cur_pose, target, blend_speed);
}

// ── AI decision making ────────────────────────────────────────────────────────
static void fight_update_ai(FightFighter &me, FightFighter &enemy) {
    float floor_y=s_fight_wh*0.82f;

    if (me.dead) {
        me.dead_timer+=1.f;
        // full physics — let them arc through the air and land
        me.vy+=0.65f;
        me.y+=me.vy;
        me.x+=me.vx;
        me.vx*=0.80f;
        me.x=fmaxf(20.f,fminf(s_fight_ww-20.f,me.x));
        if(me.y>=floor_y) {
            me.y=floor_y;
            // bounce once then stay
            if(me.vy>3.f) { me.vy*=-0.25f; me.vx*=0.5f; }
            else           { me.vy=0.f; me.vx*=0.3f; }
        }
        fight_update_pose(me);
        return;
    }

    me.anim+=0.1f;
    if(me.atk_cd>0)     me.atk_cd--;
    if(me.special_cd>0) me.special_cd--;
    if(me.state_timer>0){ me.state_timer--; if(!me.state_timer&&me.state!=FS_DEAD) me.state=FS_IDLE; }

    // physics
    bool on_floor=(me.y>=floor_y-1.f);
    me.vy+=0.65f; me.y+=me.vy;
    if(me.y>=floor_y){ me.y=floor_y; me.vy=0.f;
        if(me.state==FS_JUMP) me.state=FS_IDLE; }
    me.x+=me.vx; me.vx*=0.82f;
    me.x=fmaxf(40.f,fminf(s_fight_ww-40.f,me.x));

    // AI only acts when not locked in animation
    if(me.state_timer>0) { fight_update_pose(me); return; }

    float dist=fabsf(me.x-enemy.x);
    float dir=(enemy.x>me.x)?1.f:-1.f;
    me.facing_right=(dir>0.f);
    float aggression=0.45f+(1.f-me.hp)*0.55f;
    float r=(float)rand()/RAND_MAX;

    // in attack range
    if(dist<90.f && me.atk_cd<=0 && !enemy.dead) {
        // react to enemy attacking — block
        if(enemy.state==FS_ATTACK && r<0.28f) {
            me.state=FS_BLOCK; me.state_timer=15+rand()%10; me.state_dur=me.state_timer;
        }
        // special moves (kicks/uppercut) if cooldown allows
        else if(me.special_cd<=0 && r<0.07f*aggression) {
            AttackType specials[]={ATK_KICK,ATK_UPPERCUT,ATK_SWEEP};
            fight_do_attack(me,enemy,specials[rand()%3]);
        }
        // normal attacks
        else if(r<0.2f*aggression) {
            AttackType normals[]={ATK_JAB,ATK_CROSS,ATK_HOOK,ATK_JAB};
            fight_do_attack(me,enemy,normals[rand()%4]);
        }
    }
    // jump in
    else if(dist<140.f && on_floor && r<0.013f) {
        me.vy=-14.f; me.vx=dir*5.f; me.state=FS_JUMP; me.state_timer=35; me.state_dur=35;
    }
    // approach / back off
    else if(dist>=90.f) {
        me.vx=dir*(2.5f+aggression*1.2f);
        me.state=FS_WALK;
    } else if(dist<30.f && r<0.08f) {
        me.vx=-dir*3.f;
    } else {
        me.vx*=0.7f;
        if(me.state==FS_WALK) me.state=FS_IDLE;
    }

    fight_update_pose(me);
}

void fight_tick(float ww, float wh) {
    if(!s_fight_enabled) return;
    if(!s_fight_inited||fabsf(ww-s_fight_ww)>1.f||fabsf(wh-s_fight_wh)>1.f)
        fight_init(ww,wh);
    s_fight_ticks++;
    // Reset after loser has been dead for ~2.5s (use the dead one's timer directly)
    {
        float dt = 0.f;
        if(s_ff[0].dead) dt = s_ff[0].dead_timer;
        if(s_ff[1].dead) dt = fmaxf(dt, s_ff[1].dead_timer);
        if(dt > 160.f) fight_init(ww,wh);
    }
    fight_update_ai(s_ff[0],s_ff[1]);
    fight_update_ai(s_ff[1],s_ff[0]);
    float fy=wh*0.82f;
    for(auto &p:s_blood){ p.x+=p.vx;p.y+=p.vy;p.vy+=0.38f;
        if(p.y>fy){p.y=fy;p.vy*=-0.25f;p.vx*=0.65f;} p.life--; }
    s_blood.erase(std::remove_if(s_blood.begin(),s_blood.end(),
        [](const BloodParticle &p){return p.life<=0.f;}),s_blood.end());
}

void fight_render(float ww, float wh) {
    if(!s_fight_enabled||!s_fight_inited) return;
    float alpha=0.55f;
    float bar_w=160.f,bar_h=6.f,bar_y=wh*0.82f+18.f;
    draw_rect(20.f,bar_y,bar_w,bar_h, 0.08f,0.08f,0.08f,alpha*0.75f);
    draw_rect(20.f,bar_y,bar_w*s_ff[0].hp,bar_h, s_ff[0].cr,s_ff[0].cg,s_ff[0].cb,alpha*0.85f);
    draw_rect(ww-20.f-bar_w,bar_y,bar_w,bar_h, 0.08f,0.08f,0.08f,alpha*0.75f);
    draw_rect(ww-20.f-bar_w,bar_y,bar_w*s_ff[1].hp,bar_h, s_ff[1].cr,s_ff[1].cg,s_ff[1].cb,alpha*0.85f);
    fight_line(0,wh*.82f,ww,wh*.82f, 0.5f,0.3f,0.1f,alpha*.6f,1.f);
    for(const auto &p:s_blood){
        float a=(p.life/p.maxlife)*alpha*0.9f;
        fight_circle(p.x,p.y,p.r,p.cr,p.cg,p.cb,a);
    }
    fight_draw_fighter(s_ff[0],alpha*0.85f);
    fight_draw_fighter(s_ff[1],alpha*0.85f);
    if(s_ff[0].dead&&!s_ff[1].dead){
        float fl=(sinf(s_fight_ticks*.15f)*.5f+.5f)*0.5f;
        draw_rect(0,0,ww,4.f,s_ff[1].cr,s_ff[1].cg,s_ff[1].cb,fl);
        draw_rect(0,wh-4.f,ww,4.f,s_ff[1].cr,s_ff[1].cg,s_ff[1].cb,fl);
    } else if(s_ff[1].dead&&!s_ff[0].dead){
        float fl=(sinf(s_fight_ticks*.15f)*.5f+.5f)*0.5f;
        draw_rect(0,0,ww,4.f,s_ff[0].cr,s_ff[0].cg,s_ff[0].cb,fl);
        draw_rect(0,wh-4.f,ww,4.f,s_ff[0].cr,s_ff[0].cg,s_ff[0].cb,fl);
    }
}

void fight_set_enabled(bool en) {
    s_fight_enabled=en;
    if(en&&!s_fight_inited) fight_init(s_fight_ww,s_fight_wh);
    if(!en) s_blood.clear();
}
bool fight_get_enabled() { return s_fight_enabled; }

// ============================================================================
// URL DETECTION
// ============================================================================

struct UrlSpan {
    int row, col_start, col_end;  // col_end is inclusive
    std::string url;   // display text
    std::string href;  // actual href (may prepend https:// for www. links)
};

static std::vector<UrlSpan> s_urls;
static int s_hovered_url = -1;  // index into s_urls, or -1

static bool is_url_char(char c) {
    // Characters valid inside a URL (not terminal punctuation)
    return (c > ' ') && c != '"' && c != '\'' && c != '<' && c != '>' && c != '`';
}

static bool starts_with(const std::string &s, const char *prefix) {
    size_t plen = strlen(prefix);
    return s.size() >= plen && s.compare(0, plen, prefix) == 0;
}

// Strip trailing punctuation that is likely not part of the URL
static std::string trim_url(const std::string &s) {
    size_t end = s.size();
    // Strip paired closers if their opener isn't in the URL
    while (end > 0) {
        char c = s[end-1];
        if (c == ')' && s.find('(') == std::string::npos) { end--; continue; }
        if (c == ']' && s.find('[') == std::string::npos) { end--; continue; }
        if (c == '}' && s.find('{') == std::string::npos) { end--; continue; }
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') { end--; continue; }
        break;
    }
    return s.substr(0, end);
}

// Scan the visible grid and rebuild s_urls
static void detect_urls(Terminal *t,
                         std::function<Cell*(int row, int col)> resolve_cell) {
    s_urls.clear();
    const char *prefixes[] = { "https://", "http://", "ftp://", "file://", "www.", nullptr };

    for (int row = 0; row < t->rows; row++) {
        // Build a plain-text string for this row
        std::string line;
        line.reserve(t->cols);
        for (int col = 0; col < t->cols; col++) {
            Cell *c = resolve_cell(row, col);
            uint32_t cp = c->cp;
            if (!cp) cp = ' ';
            // Only handle ASCII for URL scanning simplicity
            if (cp < 0x80) line += (char)cp;
            else            line += '?';  // non-ASCII placeholder keeps column alignment
        }

        size_t pos = 0;
        while (pos < (size_t)t->cols) {
            // Find earliest prefix match from pos
            size_t best = std::string::npos;
            for (int pi = 0; prefixes[pi]; pi++) {
                size_t f = line.find(prefixes[pi], pos);
                if (f < best) best = f;
            }
            if (best == std::string::npos) break;

            // Scan forward to end of URL
            size_t end = best;
            while (end < (size_t)t->cols && is_url_char(line[end])) end++;

            std::string raw = line.substr(best, end - best);
            std::string url = trim_url(raw);
            size_t min_len = starts_with(url, "www.") ? 6 : 8; // www.x.com minimum
            if (url.size() > min_len) {
                UrlSpan span;
                span.row       = row;
                span.col_start = (int)best;
                span.col_end   = (int)(best + url.size() - 1);
                span.url       = url;
                span.href      = starts_with(url, "www.") ? "https://" + url : url;
                s_urls.push_back(span);
            }
            pos = end;
        }
    }
}

static int url_at(int row, int col) {
    for (int i = 0; i < (int)s_urls.size(); i++) {
        const UrlSpan &u = s_urls[i];
        if (u.row == row && col >= u.col_start && col <= u.col_end)
            return i;
    }
    return -1;
}

void open_url(const std::string &url) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
        _exit(1);
    }
}

extern int  g_font_size;
extern bool g_blink_text_on;

// ============================================================================
// CONTEXT MENU DATA
// ============================================================================

static const float OPACITY_LEVELS[] = { 1.0f, 0.85f, 0.7f, 0.5f, 0.3f, 0.1f };
static const char* OPACITY_NAMES[]  = { "100%", "85%", "70%", "50%", "30%", "10%" };
static const int   OPACITY_COUNT    = 6;

static const char* RENDER_MODE_NAMES[] = { "Normal", "CRT", "LCD", "VHS", "Focus", "Commodore 64", "Bad Composite" };
static_assert(sizeof(RENDER_MODE_NAMES)/sizeof(RENDER_MODE_NAMES[0]) == RENDER_MODE_COUNT,
              "RENDER_MODE_NAMES count mismatch");

const MenuItem MENU_ITEMS[] = {
    { "New Terminal",    false },
    { nullptr,           true  },
    { "Copy",            false },
    { "Copy as HTML",    false },
    { "Copy as ANSI",    false },
    { "Paste",           false },
    { nullptr,           true  },
    { "Reset",           false },
    { nullptr,           true  },
    { "Color Theme  >",  false },
    { "Transparency  >", false },
    { "Render Mode  >",  false },
    { "Fight Mode",      false },
    { nullptr,           true  },
    { "Quit",            false },
};
const int MENU_COUNT = (int)(sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0]));

ContextMenu g_menu = {};

extern float g_opacity;

// ============================================================================
// SELECTION HELPERS
// ============================================================================

void pixel_to_cell(Terminal *t, int px, int py, int ox, int oy, int *row, int *col) {
    *col = (int)((px - ox) / t->cell_w);
    *row = (int)((py - oy) / t->cell_h);
    if (*col < 0) *col = 0;
    if (*row < 0) *row = 0;
    if (*col >= t->cols) *col = t->cols - 1;
    if (*row >= t->rows) *row = t->rows - 1;
    *row += t->sb_count - t->sb_offset;
}

bool cell_in_sel(Terminal *t, int r, int c) {
    if (!t->sel_exists && !t->sel_active) return false;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }
    if (r < r0 || r > r1) return false;
    if (r == r0 && c < c0) return false;
    if (r == r1 && c > c1) return false;
    return true;
}

// ============================================================================
// URL HOVER / HIT TEST
// ============================================================================

static int pixel_to_render_row(Terminal *t, int py, int oy) {
    int row = (int)((py - oy) / t->cell_h);
    if (row < 0) row = 0;
    if (row >= t->rows) row = t->rows - 1;
    return row;
}
static int pixel_to_render_col(Terminal *t, int px, int ox) {
    int col = (int)((px - ox) / t->cell_w);
    if (col < 0) col = 0;
    if (col >= t->cols) col = t->cols - 1;
    return col;
}

bool url_update_hover(Terminal *t, int mouse_px, int mouse_py, int ox, int oy) {
    int row = pixel_to_render_row(t, mouse_py, oy);
    int col = pixel_to_render_col(t, mouse_px, ox);
    int uid = url_at(row, col);
    if (uid != s_hovered_url) {
        s_hovered_url = uid;
        return true;
    }
    return false;
}

std::string url_at_pixel(Terminal *t, int mouse_px, int mouse_py, int ox, int oy) {
    int row = pixel_to_render_row(t, mouse_py, oy);
    int col = pixel_to_render_col(t, mouse_px, ox);
    int uid = url_at(row, col);
    if (uid >= 0) return s_urls[uid].href;
    return {};
}

// ============================================================================
// CLIPBOARD
// ============================================================================

void term_copy_selection(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc;
    }
    int bufsize = (r1 - r0 + 1) * (t->cols + 1) + 1;
    char *buf = (char*)malloc(bufsize);
    int pos = 0;
    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (cp && cp != ' ') last_nonspace = c;
        }
        for (int c = cs; c <= last_nonspace; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (!cp) cp = ' ';
            if      (cp < 0x80)    { buf[pos++] = (char)cp; }
            else if (cp < 0x800)   { buf[pos++]=(char)(0xC0|(cp>>6)); buf[pos++]=(char)(0x80|(cp&0x3F)); }
            else if (cp < 0x10000) { buf[pos++]=(char)(0xE0|(cp>>12)); buf[pos++]=(char)(0x80|((cp>>6)&0x3F)); buf[pos++]=(char)(0x80|(cp&0x3F)); }
            else { buf[pos++]=(char)(0xF0|(cp>>18)); buf[pos++]=(char)(0x80|((cp>>12)&0x3F)); buf[pos++]=(char)(0x80|((cp>>6)&0x3F)); buf[pos++]=(char)(0x80|(cp&0x3F)); }
        }
        if (r < r1) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    SDL_SetClipboardText(buf);
    free(buf);
    //SDL_Log("[Term] copied %d chars to clipboard\n", pos);
}

static void append_hex_color(std::string &s, float r, float g, float b) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x",
             (int)(r*255+.5f), (int)(g*255+.5f), (int)(b*255+.5f));
    s += buf;
}

static void append_html_char(std::string &s, uint32_t cp) {
    if      (cp == '<')  s += "&lt;";
    else if (cp == '>')  s += "&gt;";
    else if (cp == '&')  s += "&amp;";
    else if (cp == '"')  s += "&quot;";
    else if (cp < 0x80)  s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0|(cp>>6)); s += (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0|(cp>>12)); s += (char)(0x80|((cp>>6)&0x3F)); s += (char)(0x80|(cp&0x3F)); }
    else { s+=(char)(0xF0|(cp>>18)); s+=(char)(0x80|((cp>>12)&0x3F)); s+=(char)(0x80|((cp>>6)&0x3F)); s+=(char)(0x80|(cp&0x3F)); }
}

void term_copy_selection_html(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) { int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc; }

    const Theme &th = THEMES[g_theme_idx];
    char bg_hex[8], fg_hex[8];
    snprintf(bg_hex, sizeof(bg_hex), "#%02x%02x%02x",
             (int)(th.bg_r*255+.5f), (int)(th.bg_g*255+.5f), (int)(th.bg_b*255+.5f));
    // Default foreground from palette index 7
    TermColor deffg = tcolor_resolve(TCOLOR_PALETTE(7));
    snprintf(fg_hex, sizeof(fg_hex), "#%02x%02x%02x",
             (int)(deffg.r*255+.5f), (int)(deffg.g*255+.5f), (int)(deffg.b*255+.5f));

    std::string html;
    html.reserve(8192);
    html += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"UTF-8\">\n";
    html += "<title>Terminal — "; html += th.name; html += "</title>\n";
    html += "<style>\n";
    html += "  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }\n";
    html += "  body {\n";
    html += "    background: #111;\n";
    html += "    display: flex;\n";
    html += "    justify-content: center;\n";
    html += "    align-items: flex-start;\n";
    html += "    min-height: 100vh;\n";
    html += "    padding: 2rem;\n";
    html += "  }\n";
    html += "  .terminal {\n";
    html += "    background: "; html += bg_hex; html += ";\n";
    html += "    color: "; html += fg_hex; html += ";\n";
    html += "    font-family: 'DejaVu Sans Mono', 'Cascadia Code', 'Fira Code', 'Consolas', monospace;\n";
    html += "    font-size: 14px;\n";
    html += "    line-height: 1.4;\n";
    html += "    padding: 1.25rem 1.5rem;\n";
    html += "    border-radius: 8px;\n";
    html += "    box-shadow: 0 8px 32px rgba(0,0,0,0.6);\n";
    html += "    white-space: pre;\n";
    html += "    overflow-x: auto;\n";
    html += "    max-width: 100%;\n";
    html += "  }\n";
    html += "  .terminal a {\n";
    html += "    color: #6ab0f5;\n";
    html += "    text-decoration: underline;\n";
    html += "  }\n";
    html += "  .terminal a:hover {\n";
    html += "    color: #9dcfff;\n";
    html += "  }\n";
    html += "</style>\n</head>\n<body>\n<div class=\"terminal\">";

    TermColorVal last_fg = ~(TermColorVal)0, last_bg = ~(TermColorVal)0;
    uint8_t last_attrs = 0xFF;
    bool span_open = false;
    auto close_span = [&]() { if (span_open) { html += "</span>"; span_open = false; } };

    // Build a helper to detect URLs in a row of cells (selection-relative row index)
    auto row_url_spans = [&](int r) -> std::vector<UrlSpan> {
        std::vector<UrlSpan> spans;
        const char *prefixes[] = { "https://", "http://", "ftp://", "file://", "www.", nullptr };
        std::string line;
        line.reserve(t->cols);
        for (int col = 0; col < t->cols; col++) {
            uint32_t cp = vcell(t, r, col)->cp;
            if (!cp) cp = ' ';
            line += (cp < 0x80) ? (char)cp : '?';
        }
        size_t pos = 0;
        while (pos < line.size()) {
            size_t best = std::string::npos;
            for (int pi = 0; prefixes[pi]; pi++) {
                size_t f = line.find(prefixes[pi], pos);
                if (f < best) best = f;
            }
            if (best == std::string::npos) break;
            size_t end = best;
            while (end < line.size() && is_url_char(line[end])) end++;
            std::string raw = line.substr(best, end - best);
            std::string url = trim_url(raw);
            size_t min_len = starts_with(url, "www.") ? 6 : 8;
            if (url.size() > min_len) {
                UrlSpan sp;
                sp.row = r; sp.col_start = (int)best; sp.col_end = (int)(best + url.size() - 1);
                sp.url = url;
                sp.href = starts_with(url, "www.") ? "https://" + url : url;
                spans.push_back(sp);
            }
            pos = end;
        }
        return spans;
    };

    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (cp && cp != ' ') last_nonspace = c;
        }

        std::vector<UrlSpan> row_urls = row_url_spans(r);
        bool link_open = false;
        auto close_link = [&]() { if (link_open) { html += "</a>"; link_open = false; } };

        for (int c = cs; c <= last_nonspace; c++) {
            Cell *cellp = vcell(t,r,c);
            uint32_t cp = cellp->cp ? cellp->cp : ' ';
            TermColorVal fg = cellp->fg, bg = cellp->bg;
            uint8_t attrs = cellp->attrs;
            if (attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }

            // Check if we're entering or leaving a URL span
            int uid = -1;
            for (int ui = 0; ui < (int)row_urls.size(); ui++) {
                if (c >= row_urls[ui].col_start && c <= row_urls[ui].col_end) { uid = ui; break; }
            }
            bool in_url = (uid >= 0);
            bool at_url_start = in_url && (c == row_urls[uid].col_start);
            bool past_url     = link_open && !in_url;
            if (past_url) { close_span(); close_link(); }
            if (at_url_start) {
                close_span();
                close_link();
                html += "<a href=\"";
                // HTML-escape the href
                for (char ch : row_urls[uid].href) {
                    if (ch == '"') html += "&quot;";
                    else           html += ch;
                }
                html += "\" >";
                link_open = true;
                // Reset span state so colors are re-emitted inside the link
                last_fg = ~(TermColorVal)0; last_bg = ~(TermColorVal)0; last_attrs = 0xFF;
            }

            if (fg != last_fg || bg != last_bg || attrs != last_attrs) {
                close_span();
                TermColor fc = tcolor_resolve(fg);
                TermColor bc = tcolor_resolve(bg);
                html += "<span style=\"color:";
                append_hex_color(html, fc.r, fc.g, fc.b);
                html += ";background:";
                append_hex_color(html, bc.r, bc.g, bc.b);
                if (attrs & ATTR_BOLD)      html += ";font-weight:bold";
                if (attrs & ATTR_ITALIC)    html += ";font-style:italic";
                if (attrs & ATTR_UNDERLINE) html += ";text-decoration:underline";
                html += "\">";
                span_open = true;
                last_fg = fg; last_bg = bg; last_attrs = attrs;
            }
            append_html_char(html, cp);
        }
        close_span();
        close_link();
        last_fg = ~(TermColorVal)0;
        if (r < r1) html += '\n';
    }
    html += "</div>\n</body>\n</html>\n";
    SDL_SetClipboardText(html.c_str());
}

void term_copy_selection_ansi(Terminal *t) {
    if (!t->sel_exists && !t->sel_active) return;
    int r0 = t->sel_start_row, c0 = t->sel_start_col;
    int r1 = t->sel_end_row,   c1 = t->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) { int tr=r0,tc=c0; r0=r1;c0=c1;r1=tr;c1=tc; }

    std::string out;
    out.reserve(4096);
    TermColorVal last_fg = ~(TermColorVal)0, last_bg = ~(TermColorVal)0;
    uint8_t last_attrs = 0xFF;

    auto emit_sgr = [&](TermColorVal fg, TermColorVal bg, uint8_t attrs) {
        out += "\x1b[0";
        if (attrs & ATTR_BOLD)      out += ";1";
        if (attrs & ATTR_UNDERLINE) out += ";4";
        if (attrs & ATTR_BLINK)     out += ";5";
        if (TCOLOR_IS_RGB(fg)) {
            char buf[32]; snprintf(buf,sizeof(buf),";38;2;%d;%d;%d",(int)TCOLOR_R(fg),(int)TCOLOR_G(fg),(int)TCOLOR_B(fg)); out+=buf;
        } else {
            int idx=(int)TCOLOR_IDX(fg);
            if (idx<8)       { char b[8]; snprintf(b,sizeof(b),";3%d",idx);     out+=b; }
            else if (idx<16) { char b[8]; snprintf(b,sizeof(b),";9%d",idx-8);   out+=b; }
            else             { char b[16];snprintf(b,sizeof(b),";38;5;%d",idx); out+=b; }
        }
        if (TCOLOR_IS_RGB(bg)) {
            char buf[32]; snprintf(buf,sizeof(buf),";48;2;%d;%d;%d",(int)TCOLOR_R(bg),(int)TCOLOR_G(bg),(int)TCOLOR_B(bg)); out+=buf;
        } else {
            int idx=(int)TCOLOR_IDX(bg);
            if (idx<8)       { char b[8]; snprintf(b,sizeof(b),";4%d",idx);      out+=b; }
            else if (idx<16) { char b[8]; snprintf(b,sizeof(b),";10%d",idx-8);   out+=b; }
            else             { char b[16];snprintf(b,sizeof(b),";48;5;%d",idx);  out+=b; }
        }
        out += 'm';
        last_fg = fg; last_bg = bg; last_attrs = attrs;
    };

    for (int r = r0; r <= r1; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        int last_nonspace = cs - 1;
        for (int c = cs; c <= ce; c++) {
            uint32_t cp = vcell(t,r,c)->cp;
            if (cp && cp != ' ') last_nonspace = c;
        }
        for (int c = cs; c <= last_nonspace; c++) {
            Cell *cellp = vcell(t,r,c);
            uint32_t cp = cellp->cp ? cellp->cp : ' ';
            TermColorVal fg = cellp->fg, bg = cellp->bg;
            uint8_t attrs = cellp->attrs;
            if (attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            if (fg != last_fg || bg != last_bg || attrs != last_attrs)
                emit_sgr(fg, bg, attrs);
            if      (cp < 0x80)    out += (char)cp;
            else if (cp < 0x800)   { out+=(char)(0xC0|(cp>>6)); out+=(char)(0x80|(cp&0x3F)); }
            else if (cp < 0x10000) { out+=(char)(0xE0|(cp>>12)); out+=(char)(0x80|((cp>>6)&0x3F)); out+=(char)(0x80|(cp&0x3F)); }
            else { out+=(char)(0xF0|(cp>>18)); out+=(char)(0x80|((cp>>12)&0x3F)); out+=(char)(0x80|((cp>>6)&0x3F)); out+=(char)(0x80|(cp&0x3F)); }
        }
        out += "\x1b[0m";
        last_fg = ~(TermColorVal)0;
        if (r < r1) out += '\n';
    }
    SDL_SetClipboardText(out.c_str());
    //SDL_Log("[Term] copied %zu bytes of ANSI to clipboard\n", out.size());
}

void term_paste(Terminal *t) {
    if (!SDL_HasClipboardText()) return;
    char *text = SDL_GetClipboardText();
    if (text && text[0]) {
        if (t->bracketed_paste) term_write(t, "\x1b[200~", 6);
        term_write(t, text, (int)strlen(text));
        if (t->bracketed_paste) term_write(t, "\x1b[201~", 6);
    }
    SDL_free(text);
}

// ============================================================================
// RENDERING
// ============================================================================

void term_render(Terminal *t, int ox, int oy) {
    float cw = t->cell_w, ch = t->cell_h;
    bool scrolled = (t->sb_offset > 0);

    Cell blank = {' ', TCOLOR_PALETTE(7), TCOLOR_PALETTE(0), 0, {0,0,0}};
    auto resolve_cell = [&](int row, int col) -> Cell* {
        if (scrolled) {
            int sb_row_idx = t->sb_count - t->sb_offset + row;
            if (sb_row_idx < 0) return &blank;
            if (sb_row_idx < t->sb_count) return sb_row(t, sb_row_idx) + col;
            int live_row = sb_row_idx - t->sb_count;
            return (live_row < t->rows) ? &CELL(t, live_row, col) : &blank;
        }
        return &CELL(t, row, col);
    };

    // Detect URLs in visible content
    detect_urls(t, resolve_cell);

    // Pass 1: backgrounds
    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor bc = tcolor_resolve(bg);
            int vrow = row + t->sb_count - t->sb_offset;
            if (cell_in_sel(t, vrow, col)) {
                draw_rect(px, py, cw, ch, 0.3f, 0.5f, 1.0f, 0.5f);
            } else {
                float bg_alpha = 1.f;
                draw_rect(px, py, cw, ch, bc.r, bc.g, bc.b, bg_alpha);
            }
        }
    }

    // Pass 2: glyphs and decorations
    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols; col++) {
            float px = ox + col*cw, py = oy + row*ch;
            Cell *c = resolve_cell(row, col);
            TermColorVal fg = c->fg, bg = c->bg;
            if (c->attrs & ATTR_REVERSE) { TermColorVal tmp=fg; fg=bg; bg=tmp; }
            TermColor fc = tcolor_resolve(fg);
            if ((c->attrs & ATTR_BOLD) && !TCOLOR_IS_RGB(fg) && TCOLOR_IDX(fg) < 8)
                fc = tcolor_resolve(TCOLOR_PALETTE(TCOLOR_IDX(fg)+8));
            if (c->attrs & ATTR_DIM) { fc.r *= 0.5f; fc.g *= 0.5f; fc.b *= 0.5f; }

            uint32_t cp = c->cp;
            bool blink_hidden = (c->attrs & ATTR_BLINK) && !g_blink_text_on;
            if (cp && cp != ' ' && !blink_hidden) {
                char tmp[5] = {};
                cp_to_utf8(cp, tmp);
                float baseline = py + ch * 0.82f;
                draw_text(tmp, px, baseline, g_font_size, (int)ch, fc.r, fc.g, fc.b, 1.f, c->attrs);
            }
            if ((c->attrs & ATTR_UNDERLINE) && !blink_hidden)
                draw_rect(px, py+ch-2, cw, 2, fc.r, fc.g, fc.b, 1.f);
            if ((c->attrs & ATTR_STRIKE) && !blink_hidden)
                draw_rect(px, py+ch*0.45f, cw, 1, fc.r, fc.g, fc.b, 1.f);
            if ((c->attrs & ATTR_OVERLINE) && !blink_hidden)
                draw_rect(px, py+1, cw, 1, fc.r, fc.g, fc.b, 1.f);

            // URL underline
            int uid = url_at(row, col);
            if (uid >= 0) {
                bool hovered = (uid == s_hovered_url);
                float ur = hovered ? 0.4f : 0.35f;
                float ug = hovered ? 0.8f : 0.6f;
                float ub = hovered ? 1.0f : 0.9f;
                float uh = hovered ? 2.f : 1.f;
                draw_rect(px, py + ch - uh - 1, cw, uh, ur, ug, ub, 1.f);
            }
        }
    }

    // Cursor
    if (!scrolled && t->cursor_on) {
        float cx = ox + t->cur_col * cw;
        float cy = oy + t->cur_row * ch;
        switch (t->cursor_shape) {
        case 0: draw_rect(cx, cy, cw, ch, 1,1,1, 0.3f); break;
        case 2: draw_rect(cx, cy, 2, ch, 1,1,1, 0.85f); break;
        default: draw_rect(cx, cy+ch-3, cw, 3, 1,1,1, 0.85f); break;
        }
    }

    // Scrollbar
    if (scrolled && t->sb_count > 0) {
        float win_h   = t->rows * ch;
        int   total_rows = t->sb_count + t->rows;
        float bar_h   = win_h * t->rows / total_rows;
        if (bar_h < 8) bar_h = 8;
        float bar_y   = oy + (win_h - bar_h) * (float)(total_rows - t->rows - t->sb_offset) / (total_rows - t->rows);
        float bar_x   = ox + t->cols * cw - 4;
        draw_rect(bar_x, oy, 4, win_h, 0,0,0, 0.3f);
        draw_rect(bar_x, bar_y, 4, bar_h, 0.6f, 0.6f, 0.7f, 0.8f);
    }
}

// ============================================================================
// KEYBOARD
// ============================================================================

void handle_key(Terminal *t, SDL_Keysym ks, const char *text) {
    SDL_Keymod mod = (SDL_Keymod)ks.mod;
    bool shift = (mod & KMOD_SHIFT) != 0;
    bool ctrl  = (mod & KMOD_CTRL)  != 0;
    bool alt   = (mod & KMOD_ALT)   != 0;

    if (ctrl && shift) {
        if (ks.sym == SDLK_v) { term_paste(t); return; }
        if (ks.sym == SDLK_c) { return; }
    }

    auto arrow = [&](const char *normal, const char *app, char letter) {
        if (!shift && !ctrl && !alt) {
            term_write(t, t->app_cursor_keys ? app : normal, 3);
        } else {
            int m = 1 + (shift?1:0) + (alt?2:0) + (ctrl?4:0);
            char seq[16];
            int n = snprintf(seq, sizeof(seq), "\x1b[1;%d%c", m, letter);
            term_write(t, seq, n);
        }
    };

    switch (ks.sym) {
    case SDLK_UP:    arrow("\x1b[A", "\x1bOA", 'A'); return;
    case SDLK_DOWN:  arrow("\x1b[B", "\x1bOB", 'B'); return;
    case SDLK_RIGHT: arrow("\x1b[C", "\x1bOC", 'C'); return;
    case SDLK_LEFT:  arrow("\x1b[D", "\x1bOD", 'D'); return;
    case SDLK_HOME:  term_write(t, t->app_cursor_keys ? "\x1bOH" : "\x1b[H", 3); return;
    case SDLK_END:   term_write(t, t->app_cursor_keys ? "\x1bOF" : "\x1b[F", 3); return;
    default: break;
    }

    switch (ks.sym) {
    case SDLK_RETURN:    term_write(t, "\r",      1); break;
    case SDLK_BACKSPACE: term_write(t, "\x7f",    1); break;
    case SDLK_TAB:
        if (shift) term_write(t, "\x1b[Z", 3);
        else       term_write(t, "\t",     1);
        break;
    case SDLK_ESCAPE:    term_write(t, "\x1b",    1); break;
    case SDLK_INSERT:    term_write(t, "\x1b[2~", 4); break;
    case SDLK_DELETE:    term_write(t, "\x1b[3~", 4); break;
    case SDLK_PAGEUP:    term_write(t, "\x1b[5~", 4); break;
    case SDLK_PAGEDOWN:  term_write(t, "\x1b[6~", 4); break;
    case SDLK_F1:   term_write(t, "\x1bOP",   3); break;
    case SDLK_F2:   term_write(t, "\x1bOQ",   3); break;
    case SDLK_F3:   term_write(t, "\x1bOR",   3); break;
    case SDLK_F4:   term_write(t, "\x1bOS",   3); break;
    case SDLK_F5:   term_write(t, "\x1b[15~", 5); break;
    case SDLK_F6:   term_write(t, "\x1b[17~", 5); break;
    case SDLK_F7:   term_write(t, "\x1b[18~", 5); break;
    case SDLK_F8:   term_write(t, "\x1b[19~", 5); break;
    case SDLK_F9:   term_write(t, "\x1b[20~", 5); break;
    case SDLK_F10:  term_write(t, "\x1b[21~", 5); break;
    case SDLK_F11:  term_write(t, "\x1b[23~", 5); break;
    case SDLK_F12:  term_write(t, "\x1b[24~", 5); break;
    default:
        if (ctrl && ks.sym >= SDLK_a && ks.sym <= SDLK_z) {
            char c = (char)(ks.sym - SDLK_a + 1);
            if (alt) term_write(t, "\x1b", 1);
            term_write(t, &c, 1);
        } else if (alt && text && text[0]) {
            term_write(t, "\x1b", 1);
            term_write(t, text, (int)strlen(text));
        } else if (text && text[0]) {
            term_write(t, text, (int)strlen(text));
        }
        break;
    }
}

// ============================================================================
// CONTEXT MENU
// ============================================================================

static void menu_layout(ContextMenu *m, int font_size) {
    m->item_h = (int)(font_size * 1.8f);
    m->sep_h  = 8;
    m->pad_x  = (int)(font_size * 0.8f);
    m->width  = (int)(font_size * 14.0f);
}

static int menu_total_height(ContextMenu *m) {
    int h = 8;
    for (int i = 0; i < MENU_COUNT; i++)
        h += MENU_ITEMS[i].separator ? m->sep_h : m->item_h;
    return h;
}

void menu_open(ContextMenu *m, int x, int y, int win_w, int win_h) {
    menu_layout(m, g_font_size);
    m->visible = true; m->hovered = -1; m->sub_open = -1; m->sub_hovered = -1;
    int th = menu_total_height(m);
    m->x = SDL_min(x, win_w - m->width - 2);
    m->y = SDL_min(y, win_h - th - 2);
    if (m->x < 0) m->x = 0;
    if (m->y < 0) m->y = 0;
}

int menu_hit(ContextMenu *m, int px, int py) {
    if (!m->visible) return -1;
    if (px < m->x || px > m->x + m->width) return -1;
    int y = m->y + 4;
    for (int i = 0; i < MENU_COUNT; i++) {
        int h = MENU_ITEMS[i].separator ? m->sep_h : m->item_h;
        if (!MENU_ITEMS[i].separator && py >= y && py < y + h) return i;
        y += h;
    }
    return -1;
}

int submenu_hit(ContextMenu *m, int px, int py) {
    if (m->sub_open < 0) return -1;
    if (px < m->sub_x || px > m->sub_x + m->sub_w) return -1;
    if (py < m->sub_y || py > m->sub_y + m->sub_h) return -1;
    int count = (m->sub_open == MENU_ID_THEMES)      ? THEME_COUNT :
                (m->sub_open == MENU_ID_RENDER_MODE)  ? RENDER_MODE_COUNT : OPACITY_COUNT;
    int idx = (py - m->sub_y) / m->item_h;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

static void draw_menu_panel(float mx, float my, float mw, float mh) {
    draw_rect(mx+3, my+3, mw, mh, 0,0,0, 0.35f);
    draw_rect(mx, my, mw, mh, 0.13f, 0.13f, 0.16f, 0.96f);
    draw_rect(mx, my, mw, 1, 0.35f,0.35f,0.45f, 1.f);
    draw_rect(mx, my+mh-1, mw, 1, 0.35f,0.35f,0.45f, 1.f);
    draw_rect(mx, my, 1, mh, 0.35f,0.35f,0.45f, 1.f);
    draw_rect(mx+mw-1, my, 1, mh, 0.35f,0.35f,0.45f, 1.f);
}

void menu_render(ContextMenu *m) {
    if (!m->visible) return;
    menu_layout(m, g_font_size);
    int th = menu_total_height(m);
    float mx = (float)m->x, my = (float)m->y, mw = (float)m->width;

    draw_menu_panel(mx, my, mw, (float)th);

    float y = my + 4;
    for (int i = 0; i < MENU_COUNT; i++) {
        if (MENU_ITEMS[i].separator) {
            draw_rect(mx+4, y + m->sep_h*0.5f, mw-8, 1, 0.35f,0.35f,0.45f, 1.f);
            y += m->sep_h; continue;
        }
        float ih = (float)m->item_h;
        bool hov = (i == m->hovered);
        bool sub_open = (m->sub_open == i);
        if (hov || sub_open) draw_rect(mx+2, y, mw-4, ih, 0.25f, 0.45f, 0.85f, 0.85f);
        float tr = (hov||sub_open)?1.f:0.88f, tg=tr, tb=(hov||sub_open)?1.f:0.92f;
        // Fight Mode shows a check indicator when enabled
        if (i == MENU_ID_FIGHT_MODE && fight_get_enabled()) {
            static char fight_lbl[32];
            snprintf(fight_lbl, sizeof(fight_lbl), "* Fight Mode");
            draw_text(fight_lbl, mx + m->pad_x, y + ih*0.72f, g_font_size, g_font_size, tr,tg,tb,1.f);
        } else {
            draw_text(MENU_ITEMS[i].label, mx + m->pad_x, y + ih*0.72f, g_font_size, g_font_size, tr,tg,tb,1.f);
        }
        y += ih;
    }

    if (m->sub_open == MENU_ID_THEMES || m->sub_open == MENU_ID_OPACITY || m->sub_open == MENU_ID_RENDER_MODE) {
        int count = (m->sub_open == MENU_ID_THEMES)      ? THEME_COUNT :
                    (m->sub_open == MENU_ID_RENDER_MODE)  ? RENDER_MODE_COUNT : OPACITY_COUNT;
        float sw = (float)(m->width + (int)(g_font_size * 2));
        float sh = (float)(count * m->item_h + 8);
        float sx = (float)m->sub_x, sy = (float)m->sub_y;
        m->sub_w = (int)sw; m->sub_h = (int)sh;
        draw_menu_panel(sx, sy, sw, sh);
        for (int j = 0; j < count; j++) {
            const char *lbl = (m->sub_open == MENU_ID_THEMES)     ? THEMES[j].name :
                              (m->sub_open == MENU_ID_RENDER_MODE) ? RENDER_MODE_NAMES[j] :
                                                                     OPACITY_NAMES[j];
            float iy = sy + 4 + j * m->item_h, ih = (float)m->item_h;
            bool hov = (j == m->sub_hovered);
            bool active = (m->sub_open == MENU_ID_THEMES)      ? (j == g_theme_idx) :
                          (m->sub_open == MENU_ID_RENDER_MODE)  ? (j == g_render_mode) :
                                                                   (fabsf(OPACITY_LEVELS[j]-g_opacity)<0.01f);
            if (hov)          draw_rect(sx+2,iy,sw-4,ih,0.25f,0.45f,0.85f,0.85f);
            if (active&&!hov) draw_rect(sx+2,iy,sw-4,ih,0.2f,0.35f,0.6f,0.6f);
            float tr=hov?1.f:(active?0.7f:0.88f), tg=hov?1.f:(active?0.9f:0.88f), tb=hov?1.f:(active?1.0f:0.92f);
            if (active) draw_text("\xe2\x9c\x93", sx+4, iy+ih*0.72f, g_font_size, g_font_size, 0.4f,0.8f,0.4f,1.f);
            draw_text(lbl, sx + m->pad_x + g_font_size, iy+ih*0.72f, g_font_size, g_font_size, tr,tg,tb,1.f);
        }
    }

    // Menu is drawn after gl_end_frame (to stay outside post-process), so we
    // must flush the accumulator ourselves — there's no automatic flush after this.
    gl_flush_verts();
}

// ============================================================================
// ACTIONS
// ============================================================================

void action_new_terminal() {
    char self[512] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0) return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid == 0) { setsid(); execl(self, self, nullptr); _exit(1); }
}
