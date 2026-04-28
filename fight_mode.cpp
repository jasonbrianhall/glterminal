#include "fight_mode.h"
#include "gl_renderer.h"
#include "crt_audio.h"

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
struct Pose {
    float lean;          // torso lean forward (rad)
    float crouch;        // hip drop (pixels, positive = lower)
    float rua, rla;      // right arm: upper/lower angle
    float lua, lla;      // left arm: upper/lower angle
    float rul, rll;      // right leg: upper/lower angle
    float lul, lll;      // left leg: upper/lower angle
    float head;          // head tilt
};

static Pose pose_lerp(const Pose &a, const Pose &b, float t) {
    auto lf = [&](float x, float y){ return x + (y-x)*t; };
    return { lf(a.lean,b.lean), lf(a.crouch,b.crouch),
             lf(a.rua,b.rua),   lf(a.rla,b.rla),
             lf(a.lua,b.lua),   lf(a.lla,b.lla),
             lf(a.rul,b.rul),   lf(a.rll,b.rll),
             lf(a.lul,b.lul),   lf(a.lll,b.lll),
             lf(a.head,b.head) };
}

// ── named poses ─────────────────────────────────────────────────────────────
static const Pose P_NEUTRAL  = { 0.0f, 0,   -0.3f, 0.5f,  0.3f, 0.4f,  0.15f,-0.2f,  -0.15f, 0.2f,  0.0f };
static const Pose P_GUARD    = { 0.1f, 4,   -0.8f, 1.1f,  0.7f, 1.0f,  0.2f, -0.3f,  -0.2f,  0.3f,  0.05f};
static const Pose P_JAB_WIND = { 0.15f,4,   -1.2f, 0.3f,  0.6f, 1.0f,  0.2f, -0.25f, -0.2f,  0.25f, 0.1f };
static const Pose P_JAB_EXT  = { 0.3f, 2,    0.1f,-0.1f,  0.5f, 0.8f,  0.3f, -0.4f,  -0.3f,  0.4f,  0.15f};
static const Pose P_CROSS_W  = { 0.05f,6,   -1.4f, 0.4f,  0.4f, 0.9f,  0.3f, -0.5f,  -0.25f, 0.4f,  0.05f};
static const Pose P_CROSS_E  = { 0.5f, 3,    0.2f,-0.15f, 0.3f, 0.6f,  0.4f, -0.5f,  -0.35f, 0.5f,  0.2f };
static const Pose P_HOOK_W   = { 0.1f, 5,    1.5f, 0.8f,  0.6f, 1.0f,  0.2f, -0.3f,  -0.25f, 0.35f, 0.0f };
static const Pose P_HOOK_E   = { 0.2f, 3,   -0.2f, 1.6f,  0.4f, 0.7f,  0.3f, -0.4f,  -0.3f,  0.4f,  0.1f };
static const Pose P_UPC_W    = {-0.1f,10,   -0.8f, 1.2f,  0.5f, 0.9f,  0.15f,-0.2f,  -0.2f,  0.3f, -0.1f };
static const Pose P_UPC_E    = { 0.2f, 0,   -1.8f,-0.4f,  0.6f, 0.8f,  0.2f, -0.3f,  -0.15f, 0.25f, 0.2f };
static const Pose P_KICK_W   = { 0.2f, 2,   -0.5f, 0.6f,  0.8f, 1.2f, -0.5f, 0.2f,   0.8f,  -1.2f,  0.1f };
static const Pose P_KICK_E   = { 0.3f, 2,   -0.4f, 0.5f,  0.7f, 1.1f, -0.2f, 0.1f,   1.6f,  -0.5f,  0.15f};
static const Pose P_SWEEP_W  = { 0.3f, 8,   -0.3f, 0.4f,  0.9f, 1.3f,  0.4f, 0.1f,   2.0f,   0.2f,  0.05f};
static const Pose P_SWEEP_E  = { 0.1f, 6,   -0.2f, 0.3f,  0.8f, 1.2f,  0.2f, 0.0f,   2.4f,  -0.3f,  0.0f };
static const Pose P_BLOCK    = { 0.05f,6,   -0.6f, 1.3f,  0.65f,1.2f,  0.25f,-0.35f, -0.25f, 0.35f, -0.05f};
static const Pose P_HURT     = {-0.2f, 2,    0.8f, 0.6f, -0.6f, 0.5f,  0.1f, -0.15f, -0.1f,  0.15f, -0.2f};
static const Pose P_STAGGER  = {-0.35f,3,    1.1f, 0.9f, -0.9f, 0.3f,  0.15f,-0.2f,  -0.2f,  0.25f, -0.35f};
static const Pose P_WALK_A   = { 0.08f,2,   -0.4f, 0.6f,  0.5f, 0.7f,  0.5f, -0.6f,  -0.5f,  0.6f,  0.05f};
static const Pose P_WALK_B   = { 0.08f,2,   -0.2f, 0.4f,  0.3f, 0.5f, -0.5f,  0.6f,   0.5f,  -0.6f,  0.05f};
static const Pose P_AIR      = { 0.1f, 0,   -0.7f, 0.9f,  0.6f, 0.9f, -0.6f, 0.5f,   0.5f,  -0.4f,  0.1f };
static const Pose P_DODGE    = {-0.35f,2,   -0.5f, 0.7f,  0.8f, 1.1f,  0.15f,-0.2f,  -0.2f,  0.3f, -0.15f};
static const Pose P_TAUNT    = { 0.0f, 0,    1.2f, 1.5f, -1.2f, 1.5f,  0.2f, -0.3f,  -0.2f,  0.3f,  0.0f };
static const Pose P_KD_MID   = {-0.6f,18,    1.2f, 1.5f, -1.0f, 0.5f,  0.8f,  1.0f,  -0.4f,  0.6f, -0.4f};
static const Pose P_GETUP_A  = {-0.8f,20,    0.9f, 1.2f, -0.5f, 0.8f,  1.0f,  0.5f,   0.2f,  0.8f, -0.3f};
static const Pose P_DEAD_MID = {-0.4f,14,    1.0f, 1.4f, -0.8f, 0.4f,  0.6f,  0.8f,  -0.3f,  0.5f, -0.3f};
static const Pose P_DEAD_END = {-1.4f,28,    1.5f, 1.8f, -1.2f, 0.6f,  1.2f,  1.6f,   0.4f,  1.0f, -0.5f};

// hadouken: arms drawn to hips, then both palms thrust forward
static const Pose P_HDKN_CHARGE = {-0.05f,8,  1.3f,1.6f,  1.3f,1.6f,  0.25f,-0.35f, -0.25f,0.35f, -0.05f};
static const Pose P_HDKN_PUSH   = { 0.25f,4,  0.0f,-0.1f, 0.0f,-0.1f, 0.25f,-0.35f, -0.25f,0.35f,  0.15f};

// ── enums ─────────────────────────────────────────────────────────────────────
enum AttackType { ATK_JAB, ATK_CROSS, ATK_HOOK, ATK_UPPERCUT, ATK_KICK, ATK_SWEEP, ATK_HADOUKEN };
enum FState     { FS_IDLE, FS_WALK, FS_ATTACK, FS_BLOCK, FS_HURT, FS_STAGGER,
                  FS_JUMP, FS_DODGE, FS_TAUNT, FS_KNOCKDOWN, FS_GETUP, FS_DEAD };
enum OpponentType { OPP_HUMAN, OPP_CHICKEN, OPP_LION };

struct FightFighter {
    float  x, y, vx, vy, hp;
    bool   facing_right, dead;
    float  dead_timer, anim;
    FState state;
    int    state_timer, state_dur;
    int    atk_cd, special_cd, dodge_cd;
    int    combo_count;
    int    combo_window;
    AttackType cur_atk;
    Pose   cur_pose, pose_target;
    float  pose_t;
    float  cr, cg, cb;
    float  body_scale_y;
    int    rounds_won;
    OpponentType opp_type;
};

// ── rounds ────────────────────────────────────────────────────────────────────
static int   s_round        = 1;
static bool  s_intermission = false;
static int   s_inter_timer  = 0;
static const int ROUNDS_MAX          = 3;
static const int INTERMISSION_TICKS  = 180;

// ── global state ─────────────────────────────────────────────────────────────
static FightFighter s_ff[2];
static bool  s_fight_inited = false;
static float s_fight_ww     = 800.f;
static float s_fight_wh     = 600.f;
static int   s_fight_ticks  = 0;
static float s_screen_shake = 0.f;

// ── particles ─────────────────────────────────────────────────────────────────
struct BloodParticle { float x,y,vx,vy,life,maxlife,r,cr,cg,cb; };
struct SparkParticle { float x,y,vx,vy,life,maxlife,cr,cg,cb; };
struct SweatParticle { float x,y,vx,vy,life,maxlife,r; };
struct Hadouken {
    float x, y, vx;         // position and horizontal velocity
    float cr, cg, cb;       // owner colour
    float angle;             // spin angle for rendering
    int   owner;             // 0 or 1
    bool  dead;
};

static std::vector<BloodParticle> s_blood;
static std::vector<SparkParticle> s_sparks;
static std::vector<SweatParticle> s_sweat;
static std::vector<Hadouken>      s_hadoukens;

static float frand() { return (float)rand() / (float)RAND_MAX; }

static void fight_spawn_blood(float x, float y, float cr, float cg, float cb, int n) {
    for (int i=0;i<n;i++) {
        float angle = frand()*6.2831f;
        float spd   = 2.f + frand()*5.f;
        s_blood.push_back({x, y, cosf(angle)*spd, sinf(angle)*spd-2.5f,
                           40.f+frand()*25.f, 65.f, 2.f+frand()*3.f, cr, cg, cb});
    }
}

static void fight_spawn_sparks(float x, float y, float cr, float cg, float cb, int n) {
    for (int i=0;i<n;i++) {
        float angle = frand()*6.2831f;
        float spd   = 3.f + frand()*6.f;
        s_sparks.push_back({x, y, cosf(angle)*spd, sinf(angle)*spd-1.f,
                            8.f+frand()*10.f, 18.f, cr, cg, cb});
    }
}

static void fight_spawn_sweat(float x, float y, int n) {
    for (int i=0;i<n;i++) {
        float spd = 1.5f + frand()*3.f;
        s_sweat.push_back({x + (frand()-0.5f)*10.f, y,
                           cosf(frand()*6.2831f)*spd*0.5f,
                           -spd,
                           12.f+frand()*8.f, 20.f, 1.5f+frand()*1.5f});
    }
}

static void fight_init_round(float ww, float wh) {
    s_fight_ww = ww; s_fight_wh = wh;
    float fy = wh * 0.82f;
    int r0 = s_ff[0].rounds_won, r1 = s_ff[1].rounds_won;
    s_ff[0] = {}; s_ff[0].x=ww*.25f; s_ff[0].y=fy; s_ff[0].hp=1.f;
    s_ff[0].facing_right=true;  s_ff[0].cr=1.f;   s_ff[0].cg=0.25f; s_ff[0].cb=0.25f;
    s_ff[0].cur_pose=P_GUARD; s_ff[0].pose_target=P_GUARD; s_ff[0].body_scale_y=1.f;
    s_ff[0].rounds_won=r0; s_ff[0].opp_type=OPP_HUMAN;
    s_ff[1] = {}; s_ff[1].x=ww*.75f; s_ff[1].y=fy; s_ff[1].hp=1.f;
    s_ff[1].facing_right=false; s_ff[1].cr=0.25f; s_ff[1].cg=0.5f;  s_ff[1].cb=1.f;
    s_ff[1].cur_pose=P_GUARD; s_ff[1].pose_target=P_GUARD; s_ff[1].body_scale_y=1.f;
    s_ff[1].rounds_won=r1; s_ff[1].opp_type=OPP_HUMAN;
    
    // 25% chance opponent 2 is something special!
    float mystery = frand();
    if (mystery < 0.12f) {  // 12% chicken
        s_ff[1].opp_type = OPP_CHICKEN;
        s_ff[1].cr = 1.f;   s_ff[1].cg = 0.8f;  s_ff[1].cb = 0.2f;   // yellow-orange
        s_ff[1].body_scale_y = 0.7f;  // smaller
        s_ff[1].hp = 0.7f;  // easier
    } else if (mystery < 0.25f) {  // 13% lion
        s_ff[1].opp_type = OPP_LION;
        s_ff[1].cr = 1.f;   s_ff[1].cg = 0.6f;  s_ff[1].cb = 0.2f;   // golden
        s_ff[1].body_scale_y = 1.3f;  // bigger and scarier
        s_ff[1].hp = 1.4f;  // tougher
    }
    
    s_blood.clear(); s_sparks.clear(); s_sweat.clear(); s_hadoukens.clear();
    s_fight_ticks=0; s_screen_shake=0.f; s_fight_inited=true;
}

static void fight_init(float ww, float wh) {
    s_round=1; s_intermission=false; s_inter_timer=0;
    s_ff[0].rounds_won=0; s_ff[1].rounds_won=0;
    fight_init_round(ww, wh);
}

// ── drawing primitives ───────────────────────────────────────────────────────
static void fight_line(float x1,float y1,float x2,float y2,
                        float cr,float cg,float cb,float a,float thick=3.f) {
    float dx=x2-x1, dy=y2-y1, len=sqrtf(dx*dx+dy*dy);
    if (len<0.5f) return;
    float nx=-dy/len*thick*.5f, ny=dx/len*thick*.5f;
    Vertex v[6];
    auto mk=[&](Vertex &vv,float vx,float vy){vv={vx,vy,cr,cg,cb,a};};
    mk(v[0],x1+nx,y1+ny); mk(v[1],x1-nx,y1-ny); mk(v[2],x2-nx,y2-ny);
    mk(v[3],x1+nx,y1+ny); mk(v[4],x2-nx,y2-ny); mk(v[5],x2+nx,y2+ny);
    draw_verts(v,6,GL_TRIANGLES);
}

static void fight_circle(float x,float y,float r,float cr,float cg,float cb,float a) {
    const int S=16; Vertex v[S*3];
    for (int i=0;i<S;i++) {
        float a0=(i/(float)S)*6.2831f, a1=((i+1)/(float)S)*6.2831f;
        v[i*3]  ={x,           y,           cr,cg,cb,a};
        v[i*3+1]={x+cosf(a0)*r,y+sinf(a0)*r,cr,cg,cb,a};
        v[i*3+2]={x+cosf(a1)*r,y+sinf(a1)*r,cr,cg,cb,a};
    }
    draw_verts(v,S*3,GL_TRIANGLES);
}

static void fight_2joint(float ox,float oy,
                          float upper_ang, float lower_ang_rel,
                          float upper_len, float lower_len,
                          float cr,float cg,float cb,float a, float thick,
                          float *ex=nullptr,float *ey=nullptr) {
    float kx=ox+sinf(upper_ang)*upper_len, ky=oy+cosf(upper_ang)*upper_len;
    float total_ang=upper_ang+lower_ang_rel;
    float endx=kx+sinf(total_ang)*lower_len, endy=ky+cosf(total_ang)*lower_len;
    fight_line(ox,oy,kx,ky,cr,cg,cb,a,thick);
    fight_line(kx,ky,endx,endy,cr,cg,cb,a,thick);
    if (ex) *ex=endx; if (ey) *ey=endy;
}

static void fight_draw_fighter(const FightFighter &f, float alpha) {
    float flip = f.facing_right ? 1.f : -1.f;
    const Pose &p = f.cur_pose;
    float x=f.x, base_y=f.y, sy=f.body_scale_y;

    float cr=f.cr, cg=f.cg, cb=f.cb;
    if (f.state==FS_STAGGER) { cr*=0.7f; cg*=0.7f; cb*=0.7f; }
    float dcr=cr*.65f, dcg=cg*.65f, dcb=cb*.65f;

    // Animal-specific size/color tweaks
    float head_r=11.f, torso_h=34.f*sy;
    if (f.opp_type == OPP_CHICKEN) {
        head_r = 8.f;  // smaller chicken head
    } else if (f.opp_type == OPP_LION) {
        head_r = 14.f; // bigger lion head
    }
    
    float uarm_l=21.f, larm_l=18.f, uleg_l=25.f, lleg_l=23.f;

    float hip_y     = base_y-4.f;
    float neck_y    = hip_y - torso_h - p.crouch*sy;
    float head_cy   = neck_y - head_r;
    float shoulder_x= x + sinf(p.lean)*torso_h*flip;
    float shoulder_y= neck_y;

    fight_line(x,hip_y, shoulder_x,shoulder_y, cr,cg,cb, alpha, 3.5f);

    float head_x = shoulder_x + sinf(p.head)*head_r*flip;
    float hcy    = head_cy - p.crouch*0.3f*sy;
    fight_circle(head_x, hcy, head_r, cr,cg,cb, alpha);
    
    // Different eye behavior for animals
    float eye_r = (f.state==FS_STAGGER||f.state==FS_HURT) ? 2.8f : 2.f;
    if (f.opp_type == OPP_CHICKEN) {
        eye_r *= 0.7f;  // tiny chicken eyes
        fight_circle(head_x+flip*4.f, hcy-0.5f, eye_r, 0,0,0, alpha);
        fight_circle(head_x+flip*6.5f, hcy-0.5f, eye_r, 0,0,0, alpha);
    } else if (f.opp_type == OPP_LION) {
        eye_r *= 1.3f;  // fierce lion eyes
        fight_circle(head_x+flip*4.5f, hcy-2.f, eye_r, 1,1,0, alpha*0.6f);  // glowing eyes
        fight_circle(head_x+flip*9.f, hcy-2.f, eye_r, 1,1,0, alpha*0.6f);
    } else {
        fight_circle(head_x+flip*3.5f, hcy-2.f, eye_r, 0,0,0, alpha);
        fight_circle(head_x+flip*7.5f, hcy-2.f, eye_r, 0,0,0, alpha);
    }

    { float ua=(p.lua+p.lean)*flip, ex,ey;
      fight_2joint(shoulder_x,shoulder_y, ua, p.lla*flip, uarm_l,larm_l, dcr,dcg,dcb,alpha*.75f,2.5f,&ex,&ey);
      fight_circle(ex,ey,3.5f,dcr,dcg,dcb,alpha*.75f); }
    { float ul=p.lul*flip, ex,ey;
      fight_2joint(x,hip_y, ul, p.lll*flip, uleg_l,lleg_l, dcr,dcg,dcb,alpha*.75f,3.f,&ex,&ey);
      fight_line(ex,ey,ex+flip*11.f,ey, dcr,dcg,dcb,alpha*.75f,2.f); }
    { float ul=p.rul*flip, ex,ey;
      fight_2joint(x,hip_y, ul, p.rll*flip, uleg_l,lleg_l, cr,cg,cb,alpha,3.f,&ex,&ey);
      fight_line(ex,ey,ex+flip*11.f,ey, cr,cg,cb,alpha,2.f); }
    { float ua=(p.rua+p.lean)*flip, ex,ey;
      fight_2joint(shoulder_x,shoulder_y, ua, p.rla*flip, uarm_l,larm_l, cr,cg,cb,alpha,3.f,&ex,&ey);
      fight_circle(ex,ey,4.f,cr,cg,cb,alpha); }
    
    // Add aura/glow for special opponents
    if (f.opp_type == OPP_CHICKEN) {
        fight_circle(x, hcy, 18.f, 1.0f, 0.8f, 0.2f, alpha*0.15f);  // yellow glow
    } else if (f.opp_type == OPP_LION) {
        fight_circle(x, hcy, 25.f, 1.0f, 0.6f, 0.2f, alpha*0.2f);  // gold glow
        // Draw mane spikes around head
        for (int i = 0; i < 8; i++) {
            float angle = (float)i / 8.f * 6.28318f + (f.facing_right ? 0 : 3.14159f);
            float mx = head_x + cosf(angle)*18.f;
            float my = hcy + sinf(angle)*18.f;
            float mx2 = head_x + cosf(angle)*25.f;
            float my2 = hcy + sinf(angle)*25.f;
            fight_line(mx, my, mx2, my2, cr, cg, cb, alpha*0.6f, 2.2f);
        }
    }
}

// ── attack logic ──────────────────────────────────────────────────────────────
static void fight_fire_hadouken(FightFighter &me, int owner_idx) {
    float dir = me.facing_right ? 1.f : -1.f;
    // spawn from outstretched palms area
    float hx  = me.x + dir * 20.f;
    float hy  = me.y - 52.f;
    s_hadoukens.push_back({hx, hy, dir * 5.5f, me.cr, me.cg, me.cb, 0.f, owner_idx, false});
    // small charge burst at launch point
    fight_spawn_sparks(hx, hy, me.cr, me.cg, me.cb, 8);
    fight_audio_hadouken_launch();
}

static void fight_do_attack(FightFighter &me, FightFighter &enemy, AttackType atk) {
    if (enemy.dead) return;

    // hadouken: fire projectile immediately, skip melee range check entirely
    if (atk == ATK_HADOUKEN) {
        int owner_idx = (&me == &s_ff[0]) ? 0 : 1;
        fight_fire_hadouken(me, owner_idx);
        me.cur_atk=ATK_HADOUKEN; me.state=FS_ATTACK; me.state_timer=40; me.state_dur=40;
        me.atk_cd=50; me.special_cd=140;
        return;
    }

    float dist  = fabsf(me.x-enemy.x);
    float range = (atk==ATK_KICK||atk==ATK_SWEEP) ? 105.f : 85.f;
    if (dist < range) {
        float dmg;
        switch (atk) {
            case ATK_JAB:      dmg=0.05f+frand()*0.04f; break;
            case ATK_CROSS:    dmg=0.09f+frand()*0.06f; break;
            case ATK_HOOK:     dmg=0.10f+frand()*0.06f; break;
            case ATK_UPPERCUT: dmg=0.13f+frand()*0.07f; break;
            case ATK_KICK:     dmg=0.14f+frand()*0.08f; break;
            case ATK_SWEEP:    dmg=0.11f+frand()*0.06f; break;
            case ATK_HADOUKEN: dmg=0.18f+frand()*0.07f; break;
            default:           dmg=0.07f; break;
        }
        if (me.combo_count > 1) dmg *= 1.f + me.combo_count * 0.08f;

        bool blocked = (enemy.state==FS_BLOCK || enemy.state==FS_DODGE);
        if (blocked) dmg *= 0.12f;

        enemy.hp = fmaxf(0.f, enemy.hp-dmg);
        float dir = (enemy.x > me.x) ? 1.f : -1.f;
        float kb  = (atk==ATK_UPPERCUT||atk==ATK_KICK) ? 9.f : 4.f;

        if (!blocked) {
            bool heavy = (atk==ATK_UPPERCUT||atk==ATK_KICK||atk==ATK_HOOK);
            if (heavy && enemy.hp > 0.f) {
                enemy.state=FS_STAGGER; enemy.state_timer=22; enemy.state_dur=22;
            } else {
                enemy.state=FS_HURT; enemy.state_timer=14; enemy.state_dur=14;
            }
            enemy.vx += dir*kb;
            if (atk==ATK_UPPERCUT) enemy.vy=-7.f;
            if (atk==ATK_SWEEP)    enemy.vy=-3.f;

            float hx=enemy.x, hy=enemy.y-55.f;
            fight_spawn_blood(hx,hy, enemy.cr,enemy.cg,enemy.cb,
                              (atk==ATK_KICK||atk==ATK_UPPERCUT)?10:4);
            fight_spawn_sparks(hx,hy, 1.f,0.85f,0.3f, 6);
            fight_spawn_sweat(hx,hy, 3);
            s_screen_shake += (atk==ATK_UPPERCUT||atk==ATK_KICK) ? 5.f : 2.5f;
            if (s_screen_shake > 10.f) s_screen_shake = 10.f;

            // Sound: 0=light(jab), 1=medium(cross/hook), 2=heavy(uppercut/kick/sweep)
            int weight = (atk==ATK_JAB) ? 0 : (atk==ATK_UPPERCUT||atk==ATK_KICK||atk==ATK_SWEEP) ? 2 : 1;
            fight_audio_hit(enemy.x, enemy.y, weight);
        } else {
            fight_spawn_sparks(enemy.x, enemy.y-50.f, 0.7f,0.7f,1.f, 3);
            fight_audio_block();
        }

        me.combo_count++;
        me.combo_window = 35;

        if (enemy.hp <= 0.f) {
            enemy.dead       = true;
            enemy.state      = FS_KNOCKDOWN;
            enemy.dead_timer = 0.f;
            float kdir = (enemy.x > me.x) ? 1.f : -1.f;
            enemy.vx=kdir*7.f; enemy.vy=-5.f;
            fight_audio_ko();
        }
    }

    int dur;
    switch (atk) {
        case ATK_JAB:      dur=18; me.atk_cd=22; break;
        case ATK_CROSS:    dur=26; me.atk_cd=34; break;
        case ATK_HOOK:     dur=24; me.atk_cd=30; break;
        case ATK_UPPERCUT: dur=30; me.atk_cd=38; me.special_cd=100; break;
        case ATK_KICK:     dur=28; me.atk_cd=36; me.special_cd=80;  break;
        case ATK_SWEEP:    dur=32; me.atk_cd=40; me.special_cd=90;  break;
        case ATK_HADOUKEN: dur=40; me.atk_cd=50; me.special_cd=140; break;
        default:           dur=20; me.atk_cd=25; break;
    }
    // hadouken fires projectile at the push frame (40% through animation)
    // We schedule the launch here; actual spawn happens at pose push moment.
    // Mark that the projectile should be fired this attack.
    me.cur_atk=atk; me.state=FS_ATTACK; me.state_timer=dur; me.state_dur=dur;
}

// ── pose update ───────────────────────────────────────────────────────────────
static void fight_update_pose(FightFighter &me) {
    float floor_y  = s_fight_wh * 0.82f;
    bool  on_floor = (me.y >= floor_y - 1.f);
    float ph       = me.anim;
    Pose  target; float blend_speed;

    if (me.dead || me.state==FS_KNOCKDOWN) {
        float dt = fminf(me.dead_timer/40.f, 1.f);
        target=pose_lerp(P_DEAD_MID,P_DEAD_END,dt); blend_speed=0.12f;
    } else if (me.state==FS_GETUP) {
        float t = me.state_timer/(float)(me.state_dur>0?me.state_dur:1);
        target=pose_lerp(P_GETUP_A,P_GUARD,1.f-t); blend_speed=0.15f;
    } else {
        float t=me.state_timer/(float)(me.state_dur>0?me.state_dur:1);
        float progress=1.f-t;
        switch (me.state) {
        case FS_IDLE:
            target=P_GUARD;
            target.lean  +=sinf(ph*0.3f)*0.04f;
            target.crouch+=sinf(ph*0.25f+1.f)*1.5f;
            blend_speed=0.08f; break;
        case FS_WALK: {
            float wt=(sinf(ph*2.2f)*0.5f+0.5f);
            target=pose_lerp(P_WALK_A,P_WALK_B,wt); blend_speed=0.18f; break; }
        case FS_JUMP:
            target=P_AIR;
            if (me.vy<0.f){target.rul=-0.8f;target.rll=0.8f;target.lul=0.7f;target.lll=-0.6f;}
            blend_speed=0.2f; break;
        case FS_DODGE:   target=P_DODGE; blend_speed=0.35f; break;
        case FS_BLOCK:
            target=P_BLOCK; target.crouch+=sinf(ph*1.5f)*1.f; blend_speed=0.25f; break;
        case FS_HURT:    target=P_HURT; blend_speed=0.35f; break;
        case FS_STAGGER: {
            float wob=sinf(ph*4.f)*0.5f+0.5f;
            target=pose_lerp(P_STAGGER,P_NEUTRAL,wob); blend_speed=0.25f; break; }
        case FS_TAUNT:
            target=P_TAUNT; target.lean+=sinf(ph*0.6f)*0.06f; blend_speed=0.12f; break;
        case FS_ATTACK:
            switch (me.cur_atk) {
            case ATK_JAB:
                if      (progress<0.30f) target=pose_lerp(P_GUARD,P_JAB_WIND,progress/0.3f);
                else if (progress<0.55f) target=pose_lerp(P_JAB_WIND,P_JAB_EXT,(progress-0.3f)/0.25f);
                else                     target=pose_lerp(P_JAB_EXT,P_GUARD,(progress-0.55f)/0.45f);
                break;
            case ATK_CROSS:
                if      (progress<0.25f) target=pose_lerp(P_GUARD,P_CROSS_W,progress/0.25f);
                else if (progress<0.55f) target=pose_lerp(P_CROSS_W,P_CROSS_E,(progress-0.25f)/0.3f);
                else                     target=pose_lerp(P_CROSS_E,P_GUARD,(progress-0.55f)/0.45f);
                break;
            case ATK_HOOK:
                if      (progress<0.30f) target=pose_lerp(P_GUARD,P_HOOK_W,progress/0.3f);
                else if (progress<0.60f) target=pose_lerp(P_HOOK_W,P_HOOK_E,(progress-0.3f)/0.3f);
                else                     target=pose_lerp(P_HOOK_E,P_GUARD,(progress-0.6f)/0.4f);
                break;
            case ATK_UPPERCUT:
                if      (progress<0.25f) target=pose_lerp(P_GUARD,P_UPC_W,progress/0.25f);
                else if (progress<0.55f) target=pose_lerp(P_UPC_W,P_UPC_E,(progress-0.25f)/0.3f);
                else                     target=pose_lerp(P_UPC_E,P_GUARD,(progress-0.55f)/0.45f);
                break;
            case ATK_KICK:
                if      (progress<0.30f) target=pose_lerp(P_GUARD,P_KICK_W,progress/0.3f);
                else if (progress<0.65f) target=pose_lerp(P_KICK_W,P_KICK_E,(progress-0.3f)/0.35f);
                else                     target=pose_lerp(P_KICK_E,P_GUARD,(progress-0.65f)/0.35f);
                break;
            case ATK_SWEEP:
                if      (progress<0.25f) target=pose_lerp(P_GUARD,P_SWEEP_W,progress/0.25f);
                else if (progress<0.60f) target=pose_lerp(P_SWEEP_W,P_SWEEP_E,(progress-0.25f)/0.35f);
                else                     target=pose_lerp(P_SWEEP_E,P_GUARD,(progress-0.6f)/0.4f);
                break;
            case ATK_HADOUKEN:
                if      (progress<0.40f) target=pose_lerp(P_GUARD,P_HDKN_CHARGE,progress/0.4f);
                else if (progress<0.65f) target=pose_lerp(P_HDKN_CHARGE,P_HDKN_PUSH,(progress-0.4f)/0.25f);
                else                     target=pose_lerp(P_HDKN_PUSH,P_GUARD,(progress-0.65f)/0.35f);
                break;
            default: target=P_GUARD; break;
            }
            blend_speed=0.3f; break;
        default: target=P_GUARD; blend_speed=0.1f; break;
        }
    }

    if (on_floor && me.vy > 3.f)
        me.body_scale_y = fmaxf(0.75f, 1.f - me.vy*0.04f);
    else
        me.body_scale_y += (1.f - me.body_scale_y) * 0.15f;

    me.cur_pose = pose_lerp(me.cur_pose, target, blend_speed);
}

// ── AI ────────────────────────────────────────────────────────────────────────
static void fight_update_ai(FightFighter &me, FightFighter &enemy) {
    float floor_y = s_fight_wh * 0.82f;

    if (me.combo_window > 0 && --me.combo_window == 0) me.combo_count = 0;

    // knocked down — physics only, then maybe get up
    if (me.state == FS_KNOCKDOWN) {
        me.dead_timer += 1.f;
        me.vy+=0.65f; me.y+=me.vy; me.x+=me.vx; me.vx*=0.80f;
        me.x=fmaxf(20.f,fminf(s_fight_ww-20.f,me.x));
        if (me.y>=floor_y) {
            me.y=floor_y;
            if (me.vy>3.f){me.vy*=-0.25f;me.vx*=0.5f;} else {me.vy=0.f;me.vx*=0.3f;}
        }
        // get up after a beat if not truly dead
        if (!me.dead && me.dead_timer > 80.f) {
            me.state=FS_GETUP; me.state_timer=50; me.state_dur=50;
            me.hp=0.15f; me.dead_timer=0.f;
        }
        fight_update_pose(me); return;
    }

    if (me.dead) {
        me.dead_timer+=1.f;
        me.vy+=0.65f; me.y+=me.vy; me.x+=me.vx; me.vx*=0.80f;
        me.x=fmaxf(20.f,fminf(s_fight_ww-20.f,me.x));
        if (me.y>=floor_y) {
            me.y=floor_y;
            if (me.vy>3.f){me.vy*=-0.25f;me.vx*=0.5f;} else {me.vy=0.f;me.vx*=0.3f;}
        }
        fight_update_pose(me); return;
    }

    me.anim+=0.1f;
    if (me.atk_cd>0)    me.atk_cd--;
    if (me.special_cd>0)me.special_cd--;
    if (me.dodge_cd>0)  me.dodge_cd--;
    if (me.state_timer>0){me.state_timer--;if(!me.state_timer&&me.state!=FS_DEAD)me.state=FS_IDLE;}

    bool on_floor=(me.y>=floor_y-1.f);
    me.vy+=0.65f; me.y+=me.vy;
    if (me.y>=floor_y){me.y=floor_y;me.vy=0.f;if(me.state==FS_JUMP)me.state=FS_IDLE;}
    me.x+=me.vx; me.vx*=0.82f;
    me.x=fmaxf(40.f,fminf(s_fight_ww-40.f,me.x));

    // periodic sweat when hurt
    if ((s_fight_ticks%40)==0 && me.hp<0.7f)
        fight_spawn_sweat(me.x, me.y-60.f, 1);

    if (me.state_timer>0){fight_update_pose(me);return;}

    float dist       = fabsf(me.x-enemy.x);
    float dir        = (enemy.x>me.x)?1.f:-1.f;
    me.facing_right  = (dir>0.f);
    float aggression = 0.45f+(1.f-me.hp)*0.55f;
    
    // Animals are more aggressive!
    if (me.opp_type == OPP_CHICKEN) {
        aggression = fmaxf(aggression, 0.6f);  // at least 60% aggression
    } else if (me.opp_type == OPP_LION) {
        aggression = fmaxf(aggression, 0.85f);  // at least 85% aggression for lions
    }
    
    bool  retreating = (me.hp<0.30f);
    float r = frand();

    if (retreating) {
        // ── retreat mode: back away and throw hadoukens ───────────────────────
        me.facing_right = (dir>0.f);  // always face enemy while retreating

        if (enemy.state==FS_ATTACK && me.dodge_cd<=0 && r<0.50f) {
            me.vx=-dir*6.f; me.state=FS_DODGE;
            me.state_timer=16; me.state_dur=16; me.dodge_cd=30;
        }
        else if (enemy.state==FS_ATTACK && r<0.45f) {
            me.state=FS_BLOCK; me.state_timer=18+rand()%8; me.state_dur=me.state_timer;
        }
        else if (me.special_cd<=0 && dist>80.f && dist<400.f && r<0.06f) {
            fight_do_attack(me,enemy,ATK_HADOUKEN);
        }
        else if (dist<55.f && me.atk_cd<=0 && r<0.18f) {
            fight_do_attack(me,enemy,ATK_JAB);  // desperate jab if cornered
        }
        else if (dist < 180.f) {
            me.vx = -dir * 3.5f;  // back away
            me.state = FS_WALK;
        }
        else {
            me.vx *= 0.6f;  // hold ground at safe range
            if (me.state==FS_WALK) me.state=FS_IDLE;
        }
    }
    else {
        // ── normal aggression mode ────────────────────────────────────────────
        if (dist<90.f && me.atk_cd<=0 && !enemy.dead) {
            if (enemy.state==FS_ATTACK && me.dodge_cd<=0 && r<0.22f) {
                me.vx=-dir*5.f; me.state=FS_DODGE;
                me.state_timer=16; me.state_dur=16; me.dodge_cd=40;
            }
            else if (enemy.state==FS_ATTACK && r<0.28f) {
                me.state=FS_BLOCK; me.state_timer=15+rand()%10; me.state_dur=me.state_timer;
            }
            else if (me.combo_count>0 && me.combo_window>0 && r<0.55f*aggression) {
                AttackType combo[]={ATK_JAB,ATK_CROSS,ATK_HOOK,ATK_JAB,ATK_CROSS};
                fight_do_attack(me,enemy,combo[rand()%5]);
            }
            else if (me.special_cd<=0 && r<0.07f*aggression) {
                if (dist > 110.f && dist < 350.f) {
                    fight_do_attack(me,enemy,ATK_HADOUKEN);
                } else {
                    AttackType specials[]={ATK_KICK,ATK_UPPERCUT,ATK_SWEEP};
                    fight_do_attack(me,enemy,specials[rand()%3]);
                }
            }
            else if (r<0.2f*aggression) {
                AttackType normals[]={ATK_JAB,ATK_CROSS,ATK_HOOK,ATK_JAB};
                fight_do_attack(me,enemy,normals[rand()%4]);
            }
            else if (me.hp>0.6f && enemy.hp<0.3f && r<0.005f) {
                me.state=FS_TAUNT; me.state_timer=40; me.state_dur=40;
            }
        }
        else if (dist<140.f && on_floor && r<0.013f) {
            me.vy=-14.f; me.vx=dir*5.f; me.state=FS_JUMP; me.state_timer=35; me.state_dur=35;
        }
        else if (dist>=90.f) {
            me.vx=dir*(2.5f+aggression*1.2f);
            me.state=FS_WALK;
        }
        else if (dist<30.f && r<0.08f) { me.vx=-dir*3.f; }
        else {
            me.vx*=0.7f;
            if (me.state==FS_WALK) me.state=FS_IDLE;
        }
    }

    fight_update_pose(me);
}

// ── tick ─────────────────────────────────────────────────────────────────────
void fight_tick(float ww, float wh) {
    if (!s_fight_enabled) return;
    if (!s_fight_inited||fabsf(ww-s_fight_ww)>1.f||fabsf(wh-s_fight_wh)>1.f)
        fight_init(ww,wh);

    if (s_intermission) {
        if (++s_inter_timer >= INTERMISSION_TICKS) {
            s_intermission=false; s_inter_timer=0;
            if (++s_round > ROUNDS_MAX) fight_init(ww,wh);
            else                        fight_init_round(ww,wh);
        }
        return;
    }

    s_fight_ticks++;
    s_screen_shake *= 0.80f;
    if (s_screen_shake<0.05f) s_screen_shake=0.f;

    fight_update_ai(s_ff[0],s_ff[1]);
    fight_update_ai(s_ff[1],s_ff[0]);

    float fy=wh*0.82f;
    for (auto &p:s_blood){p.x+=p.vx;p.y+=p.vy;p.vy+=0.38f;
        if(p.y>fy){p.y=fy;p.vy*=-0.25f;p.vx*=0.65f;}p.life--;}
    s_blood.erase(std::remove_if(s_blood.begin(),s_blood.end(),
        [](const BloodParticle &p){return p.life<=0.f;}),s_blood.end());

    for (auto &p:s_sparks){p.x+=p.vx;p.y+=p.vy;p.vy+=0.2f;p.life--;}
    s_sparks.erase(std::remove_if(s_sparks.begin(),s_sparks.end(),
        [](const SparkParticle &p){return p.life<=0.f;}),s_sparks.end());

    for (auto &p:s_sweat){p.x+=p.vx;p.y+=p.vy;p.vy+=0.25f;p.life--;}
    s_sweat.erase(std::remove_if(s_sweat.begin(),s_sweat.end(),
        [](const SweatParticle &p){return p.life<=0.f;}),s_sweat.end());

    // hadouken physics + collision
    for (auto &h : s_hadoukens) {
        if (h.dead) continue;
        h.x     += h.vx;
        h.angle += 0.12f;
        // off-screen
        if (h.x < -60.f || h.x > ww+60.f) { h.dead=true; continue; }
        // hit opponent
        FightFighter &target = s_ff[h.owner == 0 ? 1 : 0];
        float dx = fabsf(h.x - target.x), dy = fabsf(h.y - (target.y-50.f));
        if (dx < 22.f && dy < 30.f && !target.dead) {
            h.dead = true;
            bool blocked = (target.state==FS_BLOCK || target.state==FS_DODGE);
            float dmg = blocked ? 0.02f : (0.18f + frand()*0.07f);
            target.hp = fmaxf(0.f, target.hp - dmg);
            float dir = (target.x > h.x) ? 1.f : -1.f;
            if (!blocked) {
                target.vx += dir * 5.f;
                target.state=FS_STAGGER; target.state_timer=25; target.state_dur=25;
                fight_spawn_blood(target.x, target.y-55.f, target.cr,target.cg,target.cb, 6);
                s_screen_shake += 4.f;
                fight_audio_hit(target.x, target.y, 2);
            } else {
                fight_audio_block();
            }
            // big explosion of sparks at impact
            fight_spawn_sparks(h.x, h.y, h.cr, h.cg, h.cb, 14);
            if (target.hp <= 0.f) {
                target.dead=true; target.state=FS_KNOCKDOWN; target.dead_timer=0.f;
                float kdir=(target.x>h.x)?1.f:-1.f;
                target.vx=kdir*8.f; target.vy=-6.f;
                fight_audio_ko();
            }
        }
    }
    s_hadoukens.erase(std::remove_if(s_hadoukens.begin(),s_hadoukens.end(),
        [](const Hadouken &h){return h.dead;}),s_hadoukens.end());

    // round-end check
    bool f0d=s_ff[0].dead, f1d=s_ff[1].dead;
    bool f0kd=(s_ff[0].state==FS_KNOCKDOWN), f1kd=(s_ff[1].state==FS_KNOCKDOWN);
    bool f0s=(f0d&&s_ff[0].dead_timer>60.f)||(!f0d&&!f0kd);
    bool f1s=(f1d&&s_ff[1].dead_timer>60.f)||(!f1d&&!f1kd);
    if ((f0d||f1d) && f0s && f1s && !s_intermission) {
        if (f1d&&!f0d)      s_ff[0].rounds_won++;
        else if (f0d&&!f1d) s_ff[1].rounds_won++;
        s_intermission=true; s_inter_timer=0;
        fight_audio_cheer();
    }
}

// ── render ────────────────────────────────────────────────────────────────────
void fight_render(float ww, float wh) {
    if (!s_fight_enabled||!s_fight_inited) return;

    float alpha=0.55f;
    float sx=0.f, sy_off=0.f;
    if (s_screen_shake>0.f) {
        sx    =(frand()-0.5f)*2.f*s_screen_shake;
        sy_off=(frand()-0.5f)*2.f*s_screen_shake*0.5f;
    }

    // floor
    fight_line(0,wh*.82f+sy_off,ww,wh*.82f+sy_off, 0.5f,0.3f,0.1f,alpha*.6f,1.f);

    // HP bars (thicker than before)
    float bar_w=160.f, bar_h=8.f, bar_y=wh*0.82f+18.f;
    draw_rect(20.f,      bar_y,bar_w,bar_h, 0.08f,0.08f,0.08f,alpha*0.75f);
    draw_rect(20.f,      bar_y,bar_w*s_ff[0].hp,bar_h, s_ff[0].cr,s_ff[0].cg,s_ff[0].cb,alpha*0.85f);
    draw_rect(ww-20.f-bar_w,bar_y,bar_w,bar_h, 0.08f,0.08f,0.08f,alpha*0.75f);
    draw_rect(ww-20.f-bar_w,bar_y,bar_w*s_ff[1].hp,bar_h, s_ff[1].cr,s_ff[1].cg,s_ff[1].cb,alpha*0.85f);

    // round pips
    for (int i=0;i<ROUNDS_MAX;i++) {
        float px0=20.f+i*14.f, px1=ww-20.f-(ROUNDS_MAX-1)*14.f+i*14.f, py=bar_y-12.f;
        fight_circle(px0,py,4.f, 0.15f,0.15f,0.15f,alpha);
        fight_circle(px1,py,4.f, 0.15f,0.15f,0.15f,alpha);
        if (i<s_ff[0].rounds_won) fight_circle(px0,py,3.5f, s_ff[0].cr,s_ff[0].cg,s_ff[0].cb,alpha);
        if (i<s_ff[1].rounds_won) fight_circle(px1,py,3.5f, s_ff[1].cr,s_ff[1].cg,s_ff[1].cb,alpha);
    }

    // blood
    for (const auto &p:s_blood){
        float a=(p.life/p.maxlife)*alpha*0.9f;
        fight_circle(p.x+sx,p.y+sy_off,p.r,p.cr,p.cg,p.cb,a);}

    // sparks (bright streaks)
    for (const auto &p:s_sparks){
        float a=fminf((p.life/p.maxlife)*alpha*1.2f,1.f);
        fight_line(p.x+sx,p.y+sy_off,p.x+sx-p.vx*1.5f,p.y+sy_off-p.vy*1.5f,
                   p.cr,p.cg,p.cb,a,1.5f);}

    // sweat (blue-white drops)
    for (const auto &p:s_sweat){
        float a=(p.life/p.maxlife)*alpha*0.6f;
        fight_circle(p.x+sx,p.y+sy_off,p.r,0.7f,0.85f,1.f,a);}

    // hadoukens — spinning energy orb with concentric rings
    for (const auto &h : s_hadoukens) {
        if (h.dead) continue;
        float hx=h.x+sx, hy=h.y+sy_off;
        float ha=alpha*0.9f;
        // soft outer glow (large translucent disc)
        fight_circle(hx, hy, 20.f, h.cr,h.cg,h.cb, ha*0.18f);
        // mid glow
        fight_circle(hx, hy, 14.f, h.cr,h.cg,h.cb, ha*0.35f);
        // bright core
        fight_circle(hx, hy,  7.f, 1.f, 1.f, 1.f,  ha*0.85f);
        fight_circle(hx, hy,  5.f, h.cr,h.cg,h.cb, ha);
        // two spinning rings drawn as 8-spoke crosses
        for (int ring=0; ring<2; ring++) {
            float ra   = h.angle + ring * 1.5708f;  // offset second ring 90deg
            float rrad = 12.f + ring*4.f;
            float rthk = 1.8f - ring*0.4f;
            for (int spoke=0; spoke<4; spoke++) {
                float sa = ra + spoke * 1.5708f;
                float x1 = hx + cosf(sa)*4.f,  y1 = hy + sinf(sa)*4.f;
                float x2 = hx + cosf(sa)*rrad,  y2 = hy + sinf(sa)*rrad;
                fight_line(x1,y1,x2,y2, h.cr,h.cg,h.cb, ha*0.75f, rthk);
            }
        }
        // trailing sparks
        fight_spawn_sparks(hx - h.vx*0.5f, hy, h.cr,h.cg,h.cb, 1);
    }

    fight_draw_fighter(s_ff[0],alpha*0.85f);
    fight_draw_fighter(s_ff[1],alpha*0.85f);

    // intermission overlay
    if (s_intermission) {
        float t  = fminf((float)s_inter_timer/(float)INTERMISSION_TICKS,1.f);
        float fl = (sinf(s_fight_ticks*0.25f)*0.5f+0.5f)*0.4f*(1.f-t*0.5f);
        int   w  = (s_ff[0].rounds_won>=s_ff[1].rounds_won)?0:1;
        draw_rect(0,0,     ww,5.f, s_ff[w].cr,s_ff[w].cg,s_ff[w].cb,fl);
        draw_rect(0,wh-5.f,ww,5.f, s_ff[w].cr,s_ff[w].cg,s_ff[w].cb,fl);
        if (t>0.75f) draw_rect(0,0,ww,wh, 0.f,0.f,0.f,(t-0.75f)/0.25f*0.6f);
    } else {
        bool f0d=s_ff[0].dead, f1d=s_ff[1].dead;
        if (f0d&&!f1d){float fl=(sinf(s_fight_ticks*.15f)*.5f+.5f)*0.5f;
            draw_rect(0,0,ww,4.f,s_ff[1].cr,s_ff[1].cg,s_ff[1].cb,fl);
            draw_rect(0,wh-4.f,ww,4.f,s_ff[1].cr,s_ff[1].cg,s_ff[1].cb,fl);}
        else if(f1d&&!f0d){float fl=(sinf(s_fight_ticks*.15f)*.5f+.5f)*0.5f;
            draw_rect(0,0,ww,4.f,s_ff[0].cr,s_ff[0].cg,s_ff[0].cb,fl);
            draw_rect(0,wh-4.f,ww,4.f,s_ff[0].cr,s_ff[0].cg,s_ff[0].cb,fl);}
    }
}

void fight_set_enabled(bool en) {
    s_fight_enabled=en;
    if (en&&!s_fight_inited) fight_init(s_fight_ww,s_fight_wh);
    if (!en){s_blood.clear();s_sparks.clear();s_sweat.clear();s_hadoukens.clear();}
}
bool fight_get_enabled() { return s_fight_enabled; }
