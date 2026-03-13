#include "fight_mode.h"
#include "gl_renderer.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include <algorithm>

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
