//========================================================================
//  OpenGL version (no EasyX dependency) -- C11 port
//  The original program drew with EasyX (GDI based). This file uses a 2D
//  batch renderer plus a GDI glyph atlas to replicate the EasyX drawing
//  API; the game logic below is kept line for line.
//
//  Rewritten from C++11 to strict C11 for MinGW GCC 4.9.2.
//  Depends only on user32 / gdi32 / opengl32 (system DLLs).
//
//  MinGW :  gcc -std=c11 -O2 main.c -o main.exe -lopengl32 -lgdi32 -luser32
//  Needs an OpenGL 3.0+ driver (FBO is used).
//
//  easygl.h is itself strict C11 (no STL, no references, no overloading,
//  no default arguments), so it is included unchanged.
//
//  How the port works: a small "C++ compatibility layer" at the top of
//  this file emulates the C++ features the original used -- vector / pair
//  / string / map / constructors / member functions / min-max / range-for
//  -- with macros and a few static functions, so the game logic and the
//  drawing code below stay almost untouched.
//========================================================================

#include <setjmp.h>
#include "easygl.h" 

#include <conio.h>

/* ---- C standard headers (were <cmath> <cstring> <cstdlib> <cstdio> <ctime>) ---- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdbool.h>
#include <stddef.h>

/* =========================================================================
 *                   C++ compatibility layer
 * ========================================================================= */

/* 1) std::min / std::max
 *    Implemented as a GCC statement expression: each argument is
 *    evaluated exactly once; (__typeof__ and ({ ... }) are GCC extensions). */
#undef min
#undef max
#define max(a, b) ({ __typeof__(a) _cx_a = (a); __typeof__(b) _cx_b = (b); _cx_a > _cx_b ? _cx_a : _cx_b; })
#define min(a, b) ({ __typeof__(a) _cx_a = (a); __typeof__(b) _cx_b = (b); _cx_a < _cx_b ? _cx_a : _cx_b; })

/* 2) std::string
 *    The original only stored literals into a string and then printed
 *    or compared them, so a const char* is enough; compare with String_equals(). */
typedef const char* String;
#define String_equals(a, b) (strcmp((a), (b)) == 0)

/* 3) std::pair<double, double> */
typedef struct PairDD {
    double first;
    double second;
} PairDD;

/* 4) std::vector<T>
 *    DECLARE_VEC(T) defines Vec_T; afterwards you can write:
 *      vec_size(v) / vec_empty(v) / vec_at(v,i) / vec_ptr(v,i)
 *      vec_push(v,x) / vec_resize(v,n) / vec_clear(v) / vec_erase(v,i)
 *    Same three-pointer layout as std::vector; member calls become macros. */
typedef struct VecRaw { void* data; int size; int cap; } VecRaw;

#define DECLARE_VEC(T) typedef struct Vec_##T { T* data; int size; int cap; } Vec_##T

typedef struct Mob Mob;              /* forward declaration, needed by DECLARE_VEC(Mob) */
typedef struct FloatingText FloatingText;
typedef struct BOSSBAR BOSSBAR;

DECLARE_VEC(int);
DECLARE_VEC(Mob);
DECLARE_VEC(FloatingText);
DECLARE_VEC(BOSSBAR);
DECLARE_VEC(PairDD);

#define vec_size(v)      ((v).size)
#define vec_empty(v)     ((v).size == 0)
#define vec_at(v, i)     ((v).data[i])
#define vec_ptr(v, i)    (&(v).data[i])
#define vec_begin(v)     ((v).data)
#define vec_end(v)       ((v).data + (v).size)
#define vec_push(v, x)   cx_vec_push(&(v), &(x), sizeof((v).data[0]))
#define vec_resize(v, n) cx_vec_resize(&(v), (n), sizeof((v).data[0]))
#define vec_clear(v)     cx_vec_clear(&(v))
#define vec_erase(v, i)  cx_vec_erase(&(v), (i), sizeof((v).data[0]))

static void cx_vec_push(void* vp, const void* elem, size_t esz) {
    VecRaw* v = (VecRaw*)vp;
    if (v->size >= v->cap) {
        int ncap = v->cap ? v->cap * 2 : 8;
        void* p = realloc(v->data, (size_t)ncap * esz);
        if (!p) abort();              /* out of memory: same level as std::bad_alloc */
        v->data = p;
        v->cap = ncap;
    }
    memcpy((char*)v->data + (size_t)v->size * esz, elem, esz);
    v->size++;
}

/* Same as std::vector::resize: newly added elements are zero initialized */
static void cx_vec_resize(void* vp, int n, size_t esz) {
    VecRaw* v = (VecRaw*)vp;
    if (n > v->cap) {
        int ncap = v->cap ? v->cap : 8;
        while (ncap < n) ncap *= 2;
        void* p = realloc(v->data, (size_t)ncap * esz);
        if (!p) abort();
        v->data = p;
        v->cap = ncap;
    }
    if (n > v->size) memset((char*)v->data + (size_t)v->size * esz, 0, (size_t)(n - v->size) * esz);
    v->size = n;
}

/* Same as std::vector::clear: only the count is reset, capacity is kept */
static void cx_vec_clear(void* vp) {
    VecRaw* v = (VecRaw*)vp;
    v->size = 0;
}

static void cx_vec_erase(void* vp, int idx, size_t esz) {
    VecRaw* v = (VecRaw*)vp;
    if (idx < 0 || idx >= v->size) return;
    memmove((char*)v->data + (size_t)idx * esz,
            (char*)v->data + (size_t)(idx + 1) * esz,
            (size_t)(v->size - idx - 1) * esz);
    v->size--;
}

/* Same as v.erase(remove(v.begin(), v.end(), val), v.end()): drops every element equal to val */
static void vec_remove_int(Vec_int* v, int val) {
    int r;
    int w = 0;
    for (r = 0; r < v->size; r++)
        if (v->data[r] != val) v->data[w++] = v->data[r];
    v->size = w;
}

/* 5) std::map<Mob*, bool>
 *    Open addressing hash table on pointer keys: count(), operator[] and clear(). */
typedef struct PtrMapSlot { void* key; int used; int val; } PtrMapSlot;
typedef struct PtrMap { PtrMapSlot* slots; int cap; int count; } PtrMap;

static unsigned ptr_hash(void* key, int cap) {
    return (unsigned)(((size_t)key >> 4) * 2654435761u) % (unsigned)cap;
}

static void map_rehash(PtrMap* m, int ncap) {
    int i;
    PtrMapSlot* ns = (PtrMapSlot*)calloc((size_t)ncap, sizeof(PtrMapSlot));
    if (!ns) abort();
    for (i = 0; i < m->cap; i++) {
        if (!m->slots[i].used) continue;
        unsigned h = ptr_hash(m->slots[i].key, ncap);
        while (ns[h].used) h = (h + 1) % (unsigned)ncap;
        ns[h] = m->slots[i];
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
}

static void map_clear(PtrMap* m) {          /* empties it only; the bucket array is kept for reuse */
    if (!m->slots) return;
    memset(m->slots, 0, (size_t)m->cap * sizeof(PtrMapSlot));
    m->count = 0;
}

static void map_set(PtrMap* m, void* key, int val) {   /* m[key] = val */
    if (!m->slots) map_rehash(m, 256);
    if (m->count * 10 >= m->cap * 7) map_rehash(m, m->cap * 2);
    unsigned h = ptr_hash(key, m->cap);
    while (m->slots[h].used) {
        if (m->slots[h].key == key) { m->slots[h].val = val; return; }
        h = (h + 1) % (unsigned)m->cap;
    }
    m->slots[h].used = 1;
    m->slots[h].key = key;
    m->slots[h].val = val;
    m->count++;
}

static int map_count(PtrMap* m, void* key) {           /* m.count(key) */
    if (!m->slots) return 0;
    unsigned h = ptr_hash(key, m->cap);
    while (m->slots[h].used) {
        if (m->slots[h].key == key) return 1;
        h = (h + 1) % (unsigned)m->cap;
    }
    return 0;
}

static void map_free(PtrMap* m)          /* not inlined: C89 has no inline keyword */ { free(m->slots); m->slots = NULL; m->cap = m->count = 0; }
jmp_buf g_exceptionJmp;
bool g_isCrashReset = false;
LONG WINAPI CrashExceptionFilter(EXCEPTION_POINTERS* ep) {
    (void)ep;
    longjmp(g_exceptionJmp, 1);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* In C a const int is not a compile time constant (it cannot size an array), so enum constants are used */
enum{
	MAP_SIZE = 128, CHUNK_SIZE = 8
};
int SCREEN_WIDTH = 800, SCREEN_HEIGHT = 600;

// Set DPI handling to "application" (per monitor aware)


HWND hwnd;
float g_fps = 0;
double g_waveT = 0.0;    /* seconds; drives the water swell */

bool IsWindowActive(){
    HWND curHwnd=hwnd;
    if(curHwnd==NULL||!IsWindow(curHwnd)){
        curHwnd=GetHWnd();
        if(curHwnd==NULL||!IsWindow(curHwnd)){
            curHwnd=GetConsoleWindow();
        }
    }
    if(curHwnd==NULL||!IsWindow(curHwnd))return false;
    return GetForegroundWindow() == curHwnd;
}
#define ENDBLOCKTYPE 5
typedef enum BlockType {
    AIR = 0,
    SOIL = 1,
    GRASS = 2,
    SAND = 3,
    WATER = 4,
    ICE = 5
} BlockType;
#define ENDMOBTYPE 16
typedef enum MobType {
    BABY_ANT = 0,      // Baby ant
    WORKER_ANT = 1,     // Worker ant
    SOLDIER_ANT = 2,    // Soldier ant
    BUSH = 3,           // Bush
    SHELL = 4,           // Shell
    ROCK = 5,           // Rock
    STARFISH = 6,        // Starfish
    LEECH = 7,            // Leech
    LADYBUG = 8,            // LadyBug
    SANDSTORM = 9,           // SS
    SANDSTORM_SUMMON = 10,     // sandstorm summoned by the player
    CORAL = 11,     // Coral
    BEE = 12,     // Bee
    DARK_LADYBUG = 13,     // LadyBug
    SHINY_LADYBUG = 14,     // LadyBug
    CACTUS = 15,     // Cactus
    PEARL = 16     // Pearl
} MobType;

bool IsProjectile(MobType mobb){
	if(mobb==PEARL)return 1;
	else return 0;
}
bool IsObstacle(MobType mobb){
	if(mobb == BUSH || mobb == SHELL || mobb == ROCK || mobb == CORAL || mobb == CACTUS || mobb==PEARL)return 1;
	else return 0;
}

typedef struct Chunk {
    BlockType type;
    int seed;
    Vec_int mobIds;

    /* 4x4 sub-cell masks */
    unsigned char collisionPattern[4][4];  /* solid sub cells, used for SOIL */
    unsigned char texturePattern[4][4];    /* texture sub cells, GRASS / SAND */
} Chunk;

Chunk worldMap[MAP_SIZE][MAP_SIZE];

typedef struct FloatingText {
    double x, y;
    int value;
    int life;
    COLORREF color;
} FloatingText;

/* Emulates the C++ constructor FloatingText(x, y, value, color) */
static FloatingText FloatingText_new(double x, double y, int value, COLORREF color) {
    FloatingText ft;
    ft.x = x; ft.y = y; ft.value = value; ft.life = 30; ft.color = color;
    return ft;
}

typedef struct Player {
    double x, y;
    double vx, vy;
    double radius;
    double baseSpeed;
    COLORREF color;
    String name;
    double health;
    double maxHealth;
    double oldHealth;
    double bodyDamage;
    double healp;
    int damageFlashTimer;


    bool canSummonSandstorm;
    int sandstormSummonTimer;
    int activeSandstorms;
    double summonSandstormHealth;
    double summonSandstormDamage;
    double summonSandstormRadius;
} Player;

/* Emulates the C++ default constructor Player() */
static void Player_init(Player* self) {
    memset(self, 0, sizeof(*self));
    self->x = MAP_SIZE / 2;
    self->y = MAP_SIZE / 2;
    self->vx = self->vy = 0;
    self->radius = 0.18;
    self->baseSpeed = 0.1;
    self->color = YELLOW;
    self->name = "Player";
    self->maxHealth = 1000;
    self->health = self->maxHealth;
    self->oldHealth = self->maxHealth;
    self->bodyDamage = 25;
    self->healp = 5;
    self->damageFlashTimer = 0;
    self->canSummonSandstorm = false;
    self->sandstormSummonTimer = 0;
    self->activeSandstorms = 0;
    self->summonSandstormHealth = 100;
    self->summonSandstormDamage = 2.5;
    self->summonSandstormRadius = 0;
}

/* Emulates the member function Player::takeDamage(double) */
static void Player_takeDamage(Player* self, double damage) {
    self->health -= damage;
    self->damageFlashTimer = 10;
    if (self->health <= 0) {
        self->health = 0;
    }
}

/* Emulates the member function Player::heal() */
static void Player_heal(Player* self) {
    self->summonSandstormRadius = max(20.0, min(self->summonSandstormHealth / 20, 250.0));
    if (self->health + self->healp <= self->maxHealth) self->health += self->healp;
    else if (self->health <= self->maxHealth) self->health = self->maxHealth;
    if (self->health > self->oldHealth) self->oldHealth = self->health;
}

/* Emulates the member function Player::isDead() */
static bool Player_isDead(const Player* self) {
    return self->health <= 0;
}

/* Emulates the member function Player::respawn() */
static void Player_respawn(Player* self) {
    self->x = MAP_SIZE / 2;
    self->y = MAP_SIZE / 2;
    self->health = self->maxHealth;
    self->oldHealth = self->health;
    self->damageFlashTimer = 0;
}

Player player;

/* Edge triggered key input.
 * The original did while(GetAsyncKeyState(k)); after each toggle, which
 * froze the whole frame - no drawing, no logic - until the key came
 * back up.  A held R (reload the world) locked it for as long as the
 * user felt like holding. */
#define KDOWN(k) ((GetAsyncKeyState(k) & 0x8000) != 0)
static int s_prevKey[256];
typedef struct Mob {
    int id;
    MobType type;
    double x, y;
    double vx, vy;       // Velocity
    int rarity;
    int chunkX, chunkY;
    int moveTimer;
    int moveDelay;
    double targetAngle;  // Target direction
    double dir;         // Current facing direction (for head rotation)
    Vec_PairDD bodySegments; // For leech body segments

    // New attributes
    double health;
    double maxHealth;
    double oldHealth;
    double armor;
    double experienceValue;
    double bodyDamage;
    int damageFlashTimer;
    int lastDamageTaken;
    int summonLifetime;

    //for pearl
    double shellx;
    double shelly;
    double pearlx;
    double pearly;
    int shellTimer;
} Mob;

/* Emulates the C++ constructor
   Mob(id, type, x, y, rarity, isSummoned = false, shellx = 0, shelly = 0).
   C has no default arguments, so every call site passes all 8 arguments. */
static Mob Mob_new(int id, MobType type, double x, double y, int rarity, bool isSummoned, double shellx, double shelly) {
    Mob m;
    memset(&m, 0, sizeof(m));

    m.id = id;
    m.type = type;
    m.x = x;
    m.y = y;
    m.rarity = rarity;
    m.shellx = shellx;
    m.shelly = shelly;

    m.vx = 0;
    m.vy = 0;
    m.chunkX = (int)(x);
    m.chunkY = (int)(y);
    m.moveTimer = 0;
    m.moveDelay = 30 + rand() % 60;
    m.targetAngle = rand() / (double)(RAND_MAX) * 2 * 3.14159;
    m.dir = rand() / (double)(RAND_MAX) * 2 * 3.14159;  // Random initial direction
    m.damageFlashTimer = 0;
    m.lastDamageTaken = 0;
    m.summonLifetime = 300;

    m.shellTimer = 0;
    m.pearlx = x;
    m.pearly = y;

    double baseHealth[] = {25, 60, 100, 175, 50, 200, 50, 100, 60, 75, 90,300,35,85,85,50,5};
    double baseExp[] = {10, 8, 14, 10, 9, 10, 12, 12, 12, 24 , 0,18,18,16,72,10,5};
    double baseArmor[] = {0.1,0.2, 0.2, 0.8, 1.0, 0.8, 0.3, 0.3, 0.3, 0.2, 0.1,0.8,0.2,0.5,0.3,0.2,0.5};
    if (isSummoned && type == SANDSTORM_SUMMON) {
        m.maxHealth = player.summonSandstormHealth;
        m.oldHealth = m.maxHealth;
        m.health = m.maxHealth;
        m.armor = baseArmor[type] * 3.0;
        m.experienceValue = 0;
        m.bodyDamage = player.summonSandstormDamage;
    } else {
        int i;
        // Health multiplier by rarity (1x, 3.75x, 13.5x, 54x, 324x, 3159x, 145800x, 4374000x, 26244000x)
        double healthMultipliers[] = {0,1.0, 3.75, 13.5, 54.0, 324.0, 3159.0, 145800.0, 4374000.0, 26244000.0, 2361960000.0};

        // Experience multiplier by rarity
        double expMultipliers[] = {0,1.0, 3.0, 27.0, 81.0, 180.0, 810.0, 2430.0, 65610.0, 1968300.0, 39366000.0};

        // Calculate body damage: 10x3^n for most, LEECH: 20x3^n, BUSH/ROCK: 10x3^(n-1)
        double baseBodyDamage = 10.0;
        if (type == LEECH || type == STARFISH) baseBodyDamage = 20.0;
        if (type == SANDSTORM || type == PEARL) baseBodyDamage = 30.0;
        if (type == BEE) baseBodyDamage = 50.0;
        if (type == CACTUS) baseBodyDamage = 35.0;
        if (type == BUSH) baseBodyDamage = 8.0;
        if (type == ROCK) baseBodyDamage = 5.0;

        double bodyDamageMultiplier = 1.0;
        for (i = 0; i < rarity; i++) {
            bodyDamageMultiplier *= 3.0;
        }

        m.maxHealth = baseHealth[type] * healthMultipliers[rarity];
        m.oldHealth = m.maxHealth;
        m.health = m.maxHealth;
        if (m.type == SHELL) m.health *= 2;
        m.armor = baseArmor[type] * bodyDamageMultiplier;
        m.experienceValue = (int)(baseExp[type] * expMultipliers[rarity]);
        m.bodyDamage = baseBodyDamage * bodyDamageMultiplier;

        // Initialize leech body segments
        if (type == LEECH) {
            int i;
            int numSegments = 5 * rarity + rand() % 3 * rarity; // 5-10 segments
            vec_resize(m.bodySegments, numSegments);
            for (i = 0; i < numSegments; i++) {
                vec_at(m.bodySegments, i).first = x/* - (i + 1) * 0.15*/;
                vec_at(m.bodySegments, i).second = y;
            }
        }
    }
    return m;
}

/* Emulates the member function Mob::takeDamage(double) */
static void Mob_takeDamage(Mob* self, double damage) {
    if (damage > self->armor) {
        double actualDamage = damage - self->armor;
        self->health -= actualDamage;
        self->lastDamageTaken = (int)(actualDamage);
        self->damageFlashTimer = 5;
        self->armor = max(0.0, self->armor - 1); // Reduce armor when hit
    } else {
        self->lastDamageTaken = 0;
        self->oldHealth = self->health;
    }
}

/* Emulates the member function Mob::giveExperience(Player*) */
static void Mob_giveExperience(Mob* self, Player* p) {
    double experienceValue = self->experienceValue;
    MobType type = self->type;
    if(type==BABY_ANT){
        p->bodyDamage+=experienceValue*0.05;
        p->maxHealth+=experienceValue*0.5;
    }
    if(type==WORKER_ANT){
        p->bodyDamage+=experienceValue*0.07;
        p->maxHealth+=experienceValue*1;
    }
    if(type==SOLDIER_ANT){
        p->bodyDamage+=experienceValue*0.07;
        p->maxHealth+=experienceValue*2;
    }
    if(type==BUSH){
        p->bodyDamage+=experienceValue*0.01;
        p->maxHealth+=experienceValue*2;
    }
    if(type==SHELL){
        p->bodyDamage+=experienceValue*0.03;
        p->health+=experienceValue*20;
        p->health=min(p->maxHealth*2,p->health);
    }
    if(type==ROCK){
        p->bodyDamage+=experienceValue*0.01;
        p->maxHealth+=experienceValue*3;
    }
    if(type==STARFISH){
        p->bodyDamage+=experienceValue*0.01;
        p->maxHealth+=experienceValue*0.5;
        p->baseSpeed=min(0.3,p->baseSpeed+experienceValue*0.0001);
    }
    if(type==LEECH){
        p->bodyDamage+=experienceValue*0.15;
        p->maxHealth+=experienceValue*0.5;
        p->baseSpeed=min(0.3,p->baseSpeed+experienceValue*0.00005);
    }
    if(type==LADYBUG){
        p->bodyDamage+=experienceValue*0.05;
        p->maxHealth+=experienceValue*1;
        p->healp+=experienceValue*0.04;
    }
    if(type==SANDSTORM){
        p->summonSandstormDamage+=experienceValue*0.1;
        p->summonSandstormHealth+=experienceValue*0.3;
    }
    if(type==CORAL){
        p->bodyDamage+=experienceValue*0.007;
        p->maxHealth+=experienceValue*0.5;
    }
    if(type==BEE){
        p->bodyDamage+=experienceValue*0.5;
        p->maxHealth+=experienceValue*0.1;
        p->baseSpeed=min(0.3,p->baseSpeed+experienceValue*0.00005);
    }
    if(type==DARK_LADYBUG){
        p->bodyDamage+=experienceValue*0.07;
        p->maxHealth+=experienceValue*1.5;
        p->healp+=experienceValue*0.06;
    }
    if(type==SHINY_LADYBUG){
        p->bodyDamage+=experienceValue*0.3;
        p->maxHealth+=experienceValue*3;
        p->healp+=experienceValue*0.3;
    }
    if(type==CACTUS){
        p->bodyDamage+=experienceValue*0.4;
        p->maxHealth+=experienceValue*0.8;
    }
    if(type==PEARL){
        p->bodyDamage+=experienceValue*0.05;
        p->maxHealth+=experienceValue*0.1;
    }
}

/* Emulates the member function Mob::isDead() const */
static bool Mob_isDead(const Mob* self) {
    return self->health <= 0;
}

Vec_Mob mobs;
Vec_FloatingText floatingTexts;
int nextMobId = 0;

/* mobs.clear(): the C++ vector<Mob>::clear() destroys every element
   (which frees the leech body segments); in C that must be done by hand
   or the repeated clears leak memory. */
static void mobs_clear(void) {
    int i;
    for (i = 0; i < vec_size(mobs); i++) free(vec_at(mobs, i).bodySegments.data);
    vec_clear(mobs);
}

/* Equivalent of mobs.erase(it): free that element's bodySegments, then remove it */
static void mobs_erase_at(int idx) {
    free(vec_at(mobs, idx).bodySegments.data);
    vec_erase(mobs, idx);
}

const unsigned int FIXED_SEED = 1222256789;

// Pre-calculated colors for performance
COLORREF baseColors[6];
COLORREF textureColors[4];
COLORREF mobColors[17];
COLORREF rarityColors[11]; 

/* ---------------------------------------------------------------------
 * florr palette.  Every terrain / mob colour the game draws lives here so
 * the look can be retuned in one place.  These are *target* colours: they
 * are used as-is, not pushed through lift() like the few the palette does
 * not specify (AIR and ICE).
 *
 * SOIL and GRASS and SAND get more than one speckle tone so a field is no
 * longer a wall of a single green - texturePattern holds 0..5 now, 0..2 is
 * empty and 3..5 selects one of the extra tones, which keeps the old 50%
 * coverage exactly.
 * ------------------------------------------------------------------- */
#define C_SOIL        RGB(0x5e, 0x42, 0x2f)
#define C_SOIL_SPEC   RGB(0x49, 0x34, 0x25)
#define C_GRASS       RGB(0x1c, 0x9e, 0x5b)
#define C_GRASS_SPEC1 RGB(0x1e, 0xa7, 0x61)
#define C_GRASS_SPEC2 RGB(0x1f, 0xad, 0x64)
#define C_SAND        RGB(0xec, 0xdc, 0xb8)
#define C_SAND_SPEC1  RGB(0xe0, 0xd1, 0xaf)
#define C_SAND_SPEC2  RGB(0xf3, 0xe2, 0xbe)
#define C_SAND_SPEC3  RGB(0xd7, 0xc9, 0xa8)
#define C_WATER       RGB(0x56, 0x7a, 0xa1)
#define C_WAVE        RGB(0x6d, 0x97, 0xbf)
#define C_SHELL       RGB(252, 221, 134)
#define C_ROCK_FILL   RGB(0x77, 0x77, 0x77)
#define C_ROCK_LINE   RGB(0x60, 0x60, 0x60)
#define C_BUSH_FILL   RGB(0x32, 0xa8, 0x52)
#define C_BUSH_LINE   RGB(0x21, 0x85, 0x3c)
#define C_HP_FULL     RGB(0x69, 0xd1, 0x39)

/* Mob trim colours, read off the official florr mob SVGs.  These are the
 * fixed accents - rim, shade, spots, stripes, antennae, ribs - while the
 * body itself still comes from mobColors[] so every rarity keeps its own
 * tint and none of the existing colour tables changes. */
#define C_BEE_RIM     RGB(0xd3, 0xbd, 0x46)   /* #d3bd46 */
#define C_BEE_STRIPE  RGB(0x33, 0x33, 0x33)   /* #333    */
#define C_LB_RIM      RGB(0x0e, 0x0e, 0x0e)   /* #0e0e0e */
#define C_LB_SPOT     RGB(0x11, 0x11, 0x11)   /* ladybug #111    */
#define C_LB_SHADE    RGB(0xbe, 0x34, 0x2a)   /* ladybug #be342a */
#define C_DLB_SPOT    RGB(0xbe, 0x34, 0x2a)   /* dark    #be342a */
#define C_DLB_SHADE   RGB(0x79, 0x21, 0x1b)   /* dark    #79211b */
#define C_SLB_SPOT    RGB(0x11, 0x11, 0x11)   /* shiny   #111    */
#define C_SLB_SHADE   RGB(0xbe, 0xbe, 0x2a)   /* shiny   #bebe2a */
#define C_SHELL_RIM   RGB(204, 179, 109)   /* #ccb36d */

/* Extra speckle tones per terrain index (0 SOIL, 1 GRASS, 2 SAND, 3 ICE).
 * Index 3 of a pattern cell maps to [0], 4 to [1], 5 to [2].  SOIL and ICE
 * only declare what they use. */
COLORREF speckleColors[4][3];

/* How far towards white every terrain colour is pushed.  florr reads as a
 * bright, high-key scene; the palette this port started from was noticeably
 * darker and flatter.  Lifting each channel by the same percentage keeps the
 * contrast between grass / sand / soil / water exactly as it was, it just
 * moves the whole set up.  Mobs and rarity colours are deliberately left
 * alone - they have to stay readable against brighter ground, and saturated
 * entity colours are what give florr its look. */
#define TERRAIN_LIFT 18

static COLORREF lift(COLORREF c, int pct) {
    int r = GetRValue(c) + (255 - GetRValue(c)) * pct / 100;
    int g = GetGValue(c) + (255 - GetGValue(c)) * pct / 100;
    int b = GetBValue(c) + (255 - GetBValue(c)) * pct / 100;
    return RGB(r, g, b);
}

/* The water swell highlight.  Derived from baseColors[WATER] rather than
 * hardcoded so it always stays a lighter shade of the sea it sits on -
 * a near-white band on blue water was the "too harsh" look.  The high byte
 * is transparency in easygl's ARGB (0 = solid), so 190 is about 25% cover. */
static COLORREF g_waterHi = 0;

void initColors() {
    /* AIR and ICE are the two the florr palette does not pin down, so they
     * still get the global lift; everything else is a target colour. */
    baseColors[AIR] = lift(RGB(135, 206, 235), TERRAIN_LIFT);
    baseColors[SOIL] = C_SOIL;
    baseColors[GRASS] = C_GRASS;
    baseColors[SAND] = C_SAND;
    baseColors[WATER] = C_WATER;
    baseColors[ICE] = lift(RGB(180, 210, 255), TERRAIN_LIFT);  // Light ice blue
    
    textureColors[0] = C_SOIL_SPEC;    // SOIL
    textureColors[1] = C_GRASS_SPEC1;  // GRASS
    textureColors[2] = C_SAND_SPEC1;   // SAND
    textureColors[3] = lift(RGB(150, 195, 245), TERRAIN_LIFT); // ICE

    memset(speckleColors, 0, sizeof(speckleColors));
    speckleColors[0][0] = C_SOIL_SPEC;                 /* SOIL mask is 0/1, */
    speckleColors[0][1] = C_SOIL_SPEC;                 /* so only [0] is ever  */
    speckleColors[0][2] = C_SOIL_SPEC;                 /* read - filled anyway */
    speckleColors[1][0] = C_GRASS_SPEC1;               /* GRASS: two tones */
    speckleColors[1][1] = C_GRASS_SPEC2;
    speckleColors[1][2] = C_GRASS_SPEC1;               /* wraps, never black */
    speckleColors[2][0] = C_SAND_SPEC1;                /* SAND: three tones */
    speckleColors[2][1] = C_SAND_SPEC2;
    speckleColors[2][2] = C_SAND_SPEC3;
    speckleColors[3][0] = textureColors[3];            /* ICE: one tone */
    speckleColors[3][1] = textureColors[3];
    speckleColors[3][2] = textureColors[3];

    {   /* swell highlight: the wave tone straight from the palette, so it is
         * always a lighter shade of the sea instead of near-white.  The high
         * byte is transparency in easygl's ARGB (0 = solid), 190 ~= 25%. */
        g_waterHi = ARGB(190, GetRValue(C_WAVE), GetGValue(C_WAVE), GetBValue(C_WAVE));
    }
    
    rarityColors[1] = RGB(126, 239, 109);        // Green - Common
    rarityColors[2] = RGB(255, 230, 93);      // Yellow - Unusual
    rarityColors[3] = RGB(77, 82, 227);        // Dark Blue - Rare
    rarityColors[4] = RGB(134, 31, 222);      // Purple - Epic
    rarityColors[5] = RGB(222, 31, 31);        // Red - Legendary
    rarityColors[6] = RGB(31, 219, 222);      // Cyan/Aqua - Mythic
    rarityColors[7] = RGB(255, 43, 117);      // Magenta - Ultra
    rarityColors[8] = RGB(43, 255, 163);      // Light Green - Super
    rarityColors[9] = RGB(238, 238, 238);    // White - Eternal
    rarityColors[10] = RGB(85, 85, 85);    // Black - Hyper
    
    mobColors[BABY_ANT] = RGB(85, 85, 85);      // Black
    mobColors[WORKER_ANT] = RGB(85, 85, 85);     // Black
    mobColors[SOLDIER_ANT] = RGB(85, 85, 85);    // Black
    mobColors[BUSH] = C_BUSH_FILL;      // florr green
    mobColors[SHELL] = C_SHELL;         // Light yellow
    mobColors[ROCK] = C_ROCK_FILL;      // florr grey
    mobColors[STARFISH] = RGB(209, 79, 77);   
    mobColors[LEECH] = RGB(25,25,25);  // Pure black
    mobColors[LADYBUG] = RGB(235, 64, 52); 
    mobColors[SANDSTORM] = RGB(240, 240, 195); 
    mobColors[SANDSTORM_SUMMON] = RGB(255, 255, 130);
    mobColors[CORAL] = RGB(200,100,230);
    mobColors[BEE] = RGB(240,230,50);
    mobColors[DARK_LADYBUG] = RGB(150, 41, 33);
    mobColors[SHINY_LADYBUG] = RGB(235, 235, 52);
    mobColors[CACTUS] = RGB(50,150,50);
    mobColors[PEARL] = RGB(255,255,240);
}
double getDistance(double x1, double y1, double x2, double y2) {
    return sqrt((x1-x2)*(x1-x2) + (y1-y2)*(y1-y2));
}
void generateChunkPattern(int cx, int cy, bool isCollision) {
    Chunk* ck = &worldMap[cx][cy];   /* the C++ Chunk& reference */
    srand(cx * 7919 + cy * 7907 + FIXED_SEED);

    if (isCollision) {
        int i;
        int j;
        // SOIL
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                ck->collisionPattern[i][j] = rand() % 2;
    } else {
        int i;
        int j;
        /* GRASS / SAND / ICE.  0..2 leaves the sub-cell as bare terrain and
         * 3..5 picks one of the extra tones, so coverage stays at 3/6 = 50%
         * exactly while a field gets more than one shade.
         * collisionPattern stays 0/1 on purpose - the collision code reads
         * the same bytes, so widening it would change how the player walks. */
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                ck->texturePattern[i][j] = rand() % 6;
    }
}

int calculateRarity(int x, int y) {
    double distFromCenter = sqrt(pow(x - MAP_SIZE/2, 2) + pow(y - MAP_SIZE/2, 2));
    return min(10, max(1, (int)(distFromCenter / 9.0)));
}
bool isWalkable(double px, double py, bool tsummon) {
    int chunkX = (int)(px);
    int chunkY = (int)(py);
    
    if (chunkX < 0 || chunkX >= MAP_SIZE || chunkY < 0 || chunkY >= MAP_SIZE) {
        return false;
    }
    
    BlockType type = worldMap[chunkX][chunkY].type;
    if(!tsummon)return (type == GRASS || type == SAND || type == ICE || type == AIR);
    else return (type == GRASS || type == SAND || type == ICE || type == AIR || type == WATER);
}

void generateMobs() {
    int x;
    
    for (x = 0; x < MAP_SIZE; x++) {
        int y;
        for (y = 0; y < MAP_SIZE; y++) {
            BlockType type = worldMap[x][y].type;
            
            if (type == GRASS || type == SAND || type == ICE || type == WATER) {
                int rarity = calculateRarity(x, y);
                rarity=min(10,rarity+1+(type == SAND));
                int rdn=rand()%130;
                if(rdn<1)rarity=max(1,rarity-4);
                else if(rdn<3)rarity=max(1,rarity-3);
                else if(rdn<5)rarity=max(1,rarity-2);
                else if(rdn<15)rarity=max(1,rarity-1);
                else if(rdn<115);
                else if(rdn<125)rarity=min(10,rarity+1);
                else if(rdn<127)rarity=min(10,rarity+2);
                else if(rdn<129)rarity=min(10,rarity+3);
                else rarity=min(10,rarity+4);
                
                
                // Much fewer mobs - only 30% chance per chunk
                if (rand() % 100 < 30) {
                    // Only 1 mob per chunk
                    double mobX = x + 0.2 + rand() / (double)(RAND_MAX) * 0.6;
                    double mobY = y + 0.2 + rand() / (double)(RAND_MAX) * 0.6;
                    
                    MobType mobType;
                    double density = 1.0;
                    if (type == GRASS) {
                    	density = 0.4;
                        int roll = rand() % (int)min(2000 / density,max(200 / density,(10/density)));
                        if (roll < 5) mobType = BABY_ANT;
                        else if (roll < 10) mobType = WORKER_ANT;
                        else if (roll < 30) mobType = SOLDIER_ANT;
                        else if (roll < 45) mobType = ROCK;
                        else if(roll < 55) mobType = BUSH;
                        else if(roll < 80) mobType = LADYBUG;
                        else if(roll < 90) mobType = DARK_LADYBUG;
                        else if(roll < 100) mobType = BEE;
                        else continue;
                    } else if (type == SAND) {
                    	density = 0.2;
                        int roll = rand() % (int)min(2000 / density,max(200 / density,(10/density)));
                        if (roll < 25) mobType = SHELL;
                        else if (roll < 50) mobType = STARFISH;
                        else if (roll < 70) mobType = SANDSTORM;
                        else if(roll < 99) mobType = CACTUS;
                        else if(roll < 100) mobType = SHINY_LADYBUG;
                        else continue;
                    } else if (type == WATER || type == ICE) {
                    	density = 0.02;
                        int roll = rand() % (int)min(2000 / density,max(200 / density,(10/density)));
                        if (roll < 70) mobType = LEECH;
                        else if (roll < 100) mobType = CORAL;
                        else continue;
                    } else {
                        continue;
                    }
                    
                    Mob mob = Mob_new(nextMobId++, mobType, mobX, mobY, rarity, false, 0.0, 0.0);
                    vec_push(mobs, mob);
                    vec_push(worldMap[x][y].mobIds, mob.id);
                    if ((rarity >= 2 || mobType == SHELL) && (mobType == ROCK || mobType == BUSH || mobType == SANDSTORM || mobType == CORAL || mobType == SHELL)) {
                        int i;
                        int numSurrounding = 4 + rand() % 6; // 3 to 8
                        if(mobType == SANDSTORM)numSurrounding = 2 + rand() % 3;
                        int surroundingRarity;
                        if(mobType==SHELL)surroundingRarity = rarity;
						else surroundingRarity = max(1, rarity - 1);
                        
                        // Check 8 directions around the chunk
                        for (i = 0; i < numSurrounding; i++) {
                            int dx;
                            int dy;
                            if(mobType==SHELL) dx = (rand() % 7) -3,dy = (rand() % 7) -3;
							else dx = (rand() % 5) -2,dy = (rand() % 5) -2;
                            
                            if (dx == 0 && dy == 0) continue;
                            
                            int nx = x + dx;
                            int ny = y + dy;
                            
                            if (nx < 0 || nx >= MAP_SIZE || ny < 0 || ny >= MAP_SIZE) continue;
                            if (sqrt(pow(nx - MAP_SIZE/2, 2) + pow(ny - MAP_SIZE/2, 2))>55) continue;
                            if (!isWalkable(nx,  ny, 0) && mobType!= CORAL) continue;
                            if (worldMap[nx][ny].type!=WATER && mobType== CORAL) continue;
                            if (worldMap[nx][ny].type!=GRASS && mobType== BUSH) continue;
                            if (worldMap[nx][ny].type!=GRASS && mobType== ROCK) continue;
                            if (worldMap[nx][ny].type!=SAND && mobType== SANDSTORM) continue;
                            if ((worldMap[nx][ny].type==SOIL || worldMap[nx][ny].type==WATER) && mobType== SHELL) continue;
                            if (!vec_empty(worldMap[nx][ny].mobIds) && mobType!= SHELL) continue;
                            
                            double dist = getDistance(mobX, mobY, nx + 0.5, ny + 0.5);
                            if (dist < 0.8) continue;
                            
                            double surroundX = nx + 0.2 + rand() / (double)(RAND_MAX) * 0.6;
                            double surroundY = ny + 0.2 + rand() / (double)(RAND_MAX) * 0.6;
                            
                            if (surroundX < 0 || surroundX >= MAP_SIZE || surroundY < 0 || surroundY >= MAP_SIZE) continue;
                            
                            double Shellx=0,Shelly=0;if(mobType==SHELL)mobType=PEARL,Shellx=mob.x,Shelly=mob.y;
                            Mob surroundingMob = Mob_new(nextMobId++, mobType, surroundX, surroundY, surroundingRarity, false, Shellx, Shelly);
                            if(mobType==PEARL)mobType=SHELL;
							vec_push(mobs, surroundingMob);
                            vec_push(worldMap[nx][ny].mobIds, surroundingMob.id);
                            if (rarity >= 7 && mobType != CORAL && mobType!= SHELL) {
                                int i;
		                        int nnumSurrounding = 5 + rand() % 10; // 3 to 8
		                        int nsurroundingRarity = max(1, rarity - 2);
		                        
		                        // Check 8 directions around the chunk
		                        for (i = 0; i < nnumSurrounding; i++) {
		                            int ndx = (rand() % 3) -1;
                            		int ndy = (rand() % 3) - 1;
		                            
		                            if (ndx == 0 && ndy == 0) continue;
		                            
		                            int nnx = nx + ndx;
		                            int nny = ny + ndy;
		                            
		                            if (nnx < 0 || nnx >= MAP_SIZE || nny < 0 || nny >= MAP_SIZE) continue;
		                            if (sqrt(pow(nnx - MAP_SIZE/2, 2) + pow(nny - MAP_SIZE/2, 2))>55) continue;
		                            if (!isWalkable(nnx,  nny, 0)) continue;
		                            if (worldMap[nnx][nny].type!=GRASS && mobType== BUSH) continue;
		                            if (worldMap[nnx][nny].type!=GRASS && mobType== ROCK) continue;
		                            if (worldMap[nnx][nny].type!=SAND && mobType== SANDSTORM) continue;
		                            if (!vec_empty(worldMap[nnx][nny].mobIds)) continue;
		                            
		                            double dist = getDistance(mobX, mobY, nnx + 0.5, nny + 0.5);
		                            if (dist < 0.8) continue;
		                            
		                            double nsurroundX = nnx + 0.2 + rand() / (double)(RAND_MAX) * 0.6;
		                            double nsurroundY = nny + 0.2 + rand() / (double)(RAND_MAX) * 0.6;
		                            
		                            if (nsurroundX < 0 || nsurroundX >= MAP_SIZE || nsurroundY < 0 || nsurroundY >= MAP_SIZE) continue;
		                            
		                            Mob nsurroundingMob = Mob_new(nextMobId++, mobType, nsurroundX, nsurroundY, nsurroundingRarity, false, 0.0, 0.0);
		                            vec_push(mobs, nsurroundingMob);
		                            vec_push(worldMap[nnx][nny].mobIds, nsurroundingMob.id);
		                        }
		                    }
                        }
                    }
                }
            }
        }
    }
}

void initWorld() {
    int x;
    int i;
    

    // Create connected path from center to edges
    for (x = 0; x < MAP_SIZE; x++) {
        int y;
        for (y = 0; y < MAP_SIZE; y++) {
            double distFromCenter = sqrt(pow(x - MAP_SIZE/2, 2) + pow(y - MAP_SIZE/2, 2));

            if (distFromCenter < 15) {
                worldMap[x][y].type = GRASS;
                generateChunkPattern(x, y, false);
            } 
            else if (distFromCenter < 35) {
                worldMap[x][y].type = SOIL;
                generateChunkPattern(x, y, true);
            }
            else if (distFromCenter < 45) {
                worldMap[x][y].type = SAND;
                generateChunkPattern(x, y, false);
            }else{
            	worldMap[x][y].type = WATER;
			}
            if ((x^y^112^rand())%8 == 0) {
                worldMap[x][y].type = (BlockType)((worldMap[x][y].type + 1) % 6);
                if (worldMap[x][y].type == SOIL) {
                    generateChunkPattern(x, y, true);
                } else if (worldMap[x][y].type == GRASS || worldMap[x][y].type == SAND || worldMap[x][y].type == ICE) {
                    generateChunkPattern(x, y, false);
                }
        	}
        }
    }

    // Ensure connectivity by creating paths
    for (i = 0; i < 200; i++) {
        int step;
        int startX = MAP_SIZE/2;
        int startY = MAP_SIZE/2;
        int endX = rand() % MAP_SIZE;
        int endY = rand() % MAP_SIZE;

        int steps = max(abs(endX - startX), abs(endY - startY));
        for (step = 0; step <= steps; step++) {
            int x = startX + (endX - startX) * step / steps;
            int y = startY + (endY - startY) * step / steps;

            if (x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE) {
                if (worldMap[x][y].type == SOIL || worldMap[x][y].type == WATER) {
                    worldMap[x][y].type = GRASS;
                    generateChunkPattern(x, y, false);
                }
            }
        }
    }

    // Add lakes but ensure they don't block paths
    for (i = 0; i < 3; i++) {
        int dx;
        int lakeX = 20 + rand() % (MAP_SIZE - 40);
        int lakeY = 20 + rand() % (MAP_SIZE - 40);
        int lakeSize = 8 + rand() % 12;

        for (dx = -lakeSize; dx <= lakeSize; dx++) {
            int dy;
            for (dy = -lakeSize; dy <= lakeSize; dy++) {
                int x = lakeX + dx;
                int y = lakeY + dy;
                if (x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE) {
                    double dist = sqrt(dx*dx + dy*dy);
                    if (dist < lakeSize * (0.8 + 0.4 * rand() / RAND_MAX)) {
                        if (sqrt(pow(x - MAP_SIZE/2, 2) + pow(y - MAP_SIZE/2, 2)) > 20) {
                            worldMap[x][y].type = WATER;
                        }
                    }
                }
            }
        }
    }

    // Air walls at boundaries
    for (x = 0; x < MAP_SIZE; x++) {
        int y;
        for (y = 0; y < MAP_SIZE; y++) {
            if (x <=5 || x >= MAP_SIZE-6 || y <=5 || y >= MAP_SIZE-6) {
                worldMap[x][y].type = SOIL;
                generateChunkPattern(x, y, true);
            }
        }
    }

    generateMobs();
}


bool isflowerWalkable(double px, double py) {
    int chunkX = (int)(px);
    int chunkY = (int)(py);
    
    if (chunkX < 0 || chunkX >= MAP_SIZE || chunkY < 0 || chunkY >= MAP_SIZE) {
        return false;
    }
    
    BlockType type = worldMap[chunkX][chunkY].type;
    return (type == GRASS || type == SAND || type == ICE || type == AIR || type == WATER);
}

bool checkCollision(double px, double py) {
    int chunkX = (int)(px);
    int chunkY = (int)(py);

    if (chunkX < 0 || chunkX >= MAP_SIZE || chunkY < 0 || chunkY >= MAP_SIZE)
        return true;

    if (worldMap[chunkX][chunkY].type == SOIL) {
        double localX = (px - chunkX) * 4;
        double localY = (py - chunkY) * 4;

        int lx = (int)(localX);
        int ly = (int)(localY);

        if (lx >= 0 && lx < 4 && ly >= 0 && ly < 4)
            return worldMap[chunkX][chunkY].collisionPattern[lx][ly];
    }

    return false;
}


int getMobSize(Mob* mob, bool showMode) {
	
	if(showMode)return 70;
	
    double baseSize = 1;
    int size = 2 * max(5, (int)(baseSize + mob->rarity * mob->rarity));
    
    
    if (mob->type <= 2) size /= 1.2;
    if (mob->type == BUSH) size *= 1.5;
    if (mob->type == ROCK) size *= 1;
    if (mob->type == SHELL) size *= 1.5;
    if (mob->type == LEECH) size *= 0.8;
    if (mob->type == LADYBUG) size *= 1.5;
    if (mob->type == DARK_LADYBUG) size *= 1.5;
    if (mob->type == SHINY_LADYBUG) size *= 1.5;
    if (mob->type == STARFISH) size *= 2;
    if (mob->type == SANDSTORM) size *= 1.5;
    if (mob->type == CORAL) size *=2;
    if (mob->type == CACTUS) size *=2;
    if (mob->type == PEARL) size /=2;
    
    if (mob->type == SANDSTORM_SUMMON) {
        if(!showMode)size = player.summonSandstormRadius;
        else size *= 1.5;
    }
    
    return size;
}

void handleMobCollisions() {
    int mobIdx;
    int summonIdx;
    int _i2;
    // Player vs all non-summon mobs
    static PtrMap f1;            /* emulates map<Mob*,bool>; static so it is not reallocated every frame */
    map_clear(&f1);
    bool f2 = false;         // Mob Player
    for (mobIdx = 0; mobIdx < vec_size(mobs); mobIdx++) {
        Mob* mob = vec_ptr(mobs, mobIdx);
        if (Mob_isDead(mob) || mob->type == SANDSTORM_SUMMON) continue;
        
        bool collided = false;
        double collisionDistance = max(32.0/50, player.radius + getMobSize(mob, 0)/50);
        
        double distanceToHead = getDistance(player.x, player.y, mob->x, mob->y);
        if (distanceToHead < collisionDistance) {
            collided = true;
        }
        
        if (mob->type == LEECH && !vec_empty(mob->bodySegments)) {
            int segIdx;
            double segmentRadius = getMobSize(mob, 0) / 100.0;
            for (segIdx = 0; segIdx < vec_size(mob->bodySegments); segIdx++) {
                PairDD segment = vec_at(mob->bodySegments, segIdx);
                double distanceToSegment = getDistance(player.x, player.y, segment.first, segment.second);
                if (distanceToSegment < player.radius + segmentRadius) {
                    collided = true;
                    break;
                }
            }
        }
        
        if (collided) {
        	map_set(&f1, mob, 1); f2 = true;
            double angle = atan2(player.y - mob->y, player.x - mob->x);
            player.x += cos(angle) * 0.05;
            player.y += sin(angle) * 0.05;
            
            Mob_takeDamage(mob, player.bodyDamage);
            Player_takeDamage(&player, mob->bodyDamage);
            
            player.damageFlashTimer = 5;
            mob->damageFlashTimer = 5;
            
            if (mob->lastDamageTaken > 0) {
                { FloatingText _ft = FloatingText_new(mob->x, mob->y - 0.3, mob->lastDamageTaken, RED); vec_push(floatingTexts, _ft); }
            }
            
            if (Mob_isDead(mob)) {
                int tsIdx;
                Mob_giveExperience(mob, &player);
                if(mob->type==SHELL)for (tsIdx = 0; tsIdx < vec_size(mobs); tsIdx++) {
                    Mob* ts = vec_ptr(mobs, tsIdx);
                    if (fabs(ts->shellx - mob->x) < 0.5 && fabs(ts->shelly - mob->y) < 0.5)
                        ts->shellx = ts->pearlx, ts->shelly = ts->pearly;
                }
                { FloatingText _ft = FloatingText_new(mob->x, mob->y - 0.5, mob->experienceValue, GREEN); vec_push(floatingTexts, _ft); }
            }
        }
    }
    
    // Summoned sandstorm vs all non-summon mobs
    for (summonIdx = 0; summonIdx < vec_size(mobs); summonIdx++) {
        int mobIdx2;
        Mob* summon = vec_ptr(mobs, summonIdx);
    	
        if (summon->type != SANDSTORM_SUMMON || Mob_isDead(summon)) continue;
        
        for (mobIdx2 = 0; mobIdx2 < vec_size(mobs); mobIdx2++) {
        Mob* mob = vec_ptr(mobs, mobIdx2);
        	
        	bool collided = false;
            if (mob->type == SANDSTORM_SUMMON || Mob_isDead(mob)) continue;
            
            double distance = getDistance(summon->x, summon->y, mob->x, mob->y);
            double collisionDistance =  max(32.0/50, player.summonSandstormRadius/100 + getMobSize(mob, 0)/50);
            
            if (distance < collisionDistance) {
            	collided = true;
            }
            
            if (mob->type == LEECH && !vec_empty(mob->bodySegments)) {
                int segIdx2;
	            double segmentRadius = getMobSize(mob, 0) / 100.0;
	            for (segIdx2 = 0; segIdx2 < vec_size(mob->bodySegments); segIdx2++) {
                PairDD segment = vec_at(mob->bodySegments, segIdx2);
	                double distanceToSegment = getDistance(summon->x, summon->y, segment.first, segment.second);
	                if (distanceToSegment < player.summonSandstormRadius/100 + segmentRadius) {
	                    collided = true;
	                    break;
	                }
	            }
	        }
            if(collided){
            	map_set(&f1, mob, 1); map_set(&f1, summon, 1);
                // Summon damages mob
                Mob_takeDamage(mob, summon->bodyDamage);
                
                // Mob damages summon (reduced damage)
                Mob_takeDamage(summon, mob->bodyDamage);
                
                summon->damageFlashTimer = 5;
                mob->damageFlashTimer = 5;
                
                if (mob->lastDamageTaken > 0) {
                    { FloatingText _ft = FloatingText_new(mob->x, mob->y - 0.3, mob->lastDamageTaken, RED); vec_push(floatingTexts, _ft); }
                }
                
                if (Mob_isDead(mob)) {
                    int tsIdx2;
                    Mob_giveExperience(mob, &player);
                    if(mob->type==SHELL)for (tsIdx2 = 0; tsIdx2 < vec_size(mobs); tsIdx2++) {
                    Mob* ts = vec_ptr(mobs, tsIdx2);
                    if (fabs(ts->shellx - mob->x) < 0.5 && fabs(ts->shelly - mob->y) < 0.5)
                        ts->shellx = ts->pearlx, ts->shelly = ts->pearly;
                }
                    { FloatingText _ft = FloatingText_new(mob->x, mob->y - 0.5, mob->experienceValue, GREEN); vec_push(floatingTexts, _ft); }
                }
                
                if (Mob_isDead(summon)) {
                    player.activeSandstorms--;
                }
			}
        }
    }
    
    // Update player health display
    bool playerHit = false;
    for (_i2 = 0; _i2 < vec_size(mobs); _i2++) {
        Mob* mob = vec_ptr(mobs, _i2);
        if (Mob_isDead(mob)) continue;
        
        double distance = getDistance(player.x, player.y, mob->x, mob->y);
        double collisionDistance = max(16.0/50, player.radius + getMobSize(mob, 0)/50);
        
        if (distance < collisionDistance) {
            f2 = true;
        }
        if (!map_count(&f1, mob)) {
        	mob->oldHealth = mob->health;
		}
    }
    
    if (!f2) {
        player.oldHealth = player.health;
    }
}

void movePlayer() {
    double speedMultiplier = 1.0;
    
    int chunkX = (int)(player.x);
    int chunkY = (int)(player.y);
    
    if (chunkX >= 0 && chunkX < MAP_SIZE && chunkY >= 0 && chunkY < MAP_SIZE) {
        if (worldMap[chunkX][chunkY].type == SAND) {
            speedMultiplier = 0.7;
        }
    }
    
    double newX = player.x + player.vx * speedMultiplier;
    double newY = player.y + player.vy * speedMultiplier;
    
    bool canMoveX = isflowerWalkable(newX, player.y);
    bool canMoveY = isflowerWalkable(player.x, newY);
    
    if (canMoveX && !checkCollision(newX, player.y)) {
        player.x = newX;
    } else {
        player.vx *= -0.3;
    }
    
    if (canMoveY && !checkCollision(player.x, newY)) {
        player.y = newY;
    } else {
        player.vy *= -0.3;
    }
    
    if (player.x < 1.0) {
        player.x = 1.0;
        player.vx *= -0.3;
    }
    if (player.x > MAP_SIZE - 2.0) {
        player.x = MAP_SIZE - 2.0;
        player.vx *= -0.3;
    }
    if (player.y < 1.0) {
        player.y = 1.0;
        player.vy *= -0.3;
    }
    if (player.y > MAP_SIZE - 2.0) {
        player.y = MAP_SIZE - 2.0;
        player.vy *= -0.3;
    }
    
    player.vx *= 0.95;
    player.vy *= 0.95;
    
    // Handle collision with mobs
    handleMobCollisions();
}

void moveMobsInView(int startChunkX, int endChunkX, int startChunkY, int endChunkY) {
    int _i3;
    int _i4;
    // First pass: update movement timers and directions
    for (_i3 = 0; _i3 < vec_size(mobs); _i3++) {
        Mob* mob = vec_ptr(mobs, _i3);
        if (mob->chunkX < startChunkX || mob->chunkX > endChunkX ||
            mob->chunkY < startChunkY || mob->chunkY > endChunkY) {
            continue;
        }
        
        // Skip stationary mobs
        if (IsObstacle(mob->type)) {
            continue;
        }
        
        mob->moveTimer++;
        
        
        // Regular mob behavior
        if (mob->moveTimer < mob->moveDelay) continue;
        
        mob->moveTimer = 0;
        mob->moveDelay = 30 + rand() % 60;
        
        // Change target direction randomly
        mob->targetAngle = rand() / (double)(RAND_MAX) * 2 * 3.14159;
        
        // Update dir for head rotation
        if (mob->type == STARFISH || mob->type == SANDSTORM || mob->type == SANDSTORM_SUMMON) {
            mob->dir += (rand() / (double)(RAND_MAX) - 0.5) * 1.5;
        } else if (mob->type == LEECH) {
            mob->dir = mob->targetAngle;
        } else {
            mob->dir += (rand() / (double)(RAND_MAX) - 0.5) * 0.5;
        }
    }
    
    // Second pass: apply movement
    for (_i4 = 0; _i4 < vec_size(mobs); _i4++) {
        Mob* mob = vec_ptr(mobs, _i4);
        if (mob->chunkX < startChunkX || mob->chunkX > endChunkX ||
            mob->chunkY < startChunkY || mob->chunkY > endChunkY) {
            continue;
        }
        
        // Skip stationary mobs
        if (IsObstacle(mob->type)) {
        	if(mob->type==PEARL){
        		mob->shellTimer ++;
        		if(mob->shellTimer>300){
        			mob->shellTimer=0;
				}else if(mob->shellTimer>240&&mob->shellTimer<=300){
					
					double newX=(mob->x*20+mob->pearlx)/21,newY=(mob->y*20+mob->pearly)/21;
					
					bool canMovex = false,canMovey = false;
		            int newChunkX = (int)(newX);
		            int newChunkY = (int)(newY);
		            if (newChunkX >= 0 && newChunkX < MAP_SIZE && newChunkY >= 0 && newChunkY < MAP_SIZE) {
		                if(worldMap[(int)(mob->x)][newChunkY].type!=SOIL)canMovey = true;
		                if(worldMap[newChunkX][(int)(mob->y)].type!=SOIL)canMovex = true;
		            }
		            if(canMovex)mob->x=newX;
					if(canMovey)mob->y=newY;
				}else if(mob->shellTimer>90&&mob->shellTimer<=150){
					
					double newX=(mob->x*20+mob->shellx)/21,newY=(mob->y*20+mob->shelly)/21;
					
					bool canMovex = false,canMovey = false;
		            int newChunkX = (int)(newX);
		            int newChunkY = (int)(newY);
		            if (newChunkX >= 0 && newChunkX < MAP_SIZE && newChunkY >= 0 && newChunkY < MAP_SIZE) {
		                if(worldMap[(int)(mob->x)][newChunkY].type!=SOIL)canMovey = true;
		                if(worldMap[newChunkX][(int)(mob->y)].type!=SOIL)canMovex = true;
		            }
		            if(canMovex)mob->x=newX;
					if(canMovey)mob->y=newY;
		    	}
			}
            continue;
        }
        
        // Calculate speed
        double speed = 0.01 + mob->rarity * 0.003;
        
        if (mob->type == STARFISH) {
            speed *= 2.0;
        } else if (mob->type == LEECH) {
            speed *= 3.0;
        } else if (mob->type == SANDSTORM || mob->type == SANDSTORM_SUMMON) {
            speed *= 2.5;
        } else if (mob->type == BEE) {
            speed *= 5.0;
        } else if (mob->type == BABY_ANT) {
            speed *= 0.5;
        }
        
        mob->vx = cos(mob->targetAngle) * speed;
        mob->vy = sin(mob->targetAngle) * speed;
        
        // Update direction to match movement
        if (mob->type != STARFISH && mob->type != SANDSTORM && mob->type != SANDSTORM_SUMMON) {
            mob->dir = mob->targetAngle;
        } else{
            mob->dir += 0.1;
        }
        
        double newX = mob->x - mob->vx;
        double newY = mob->y - mob->vy;
        
        // Check if new position is walkable
        bool canMove = false;
        if (mob->type == LEECH) {
            int newChunkX = (int)(newX);
            int newChunkY = (int)(newY);
            if (newChunkX >= 0 && newChunkX < MAP_SIZE && newChunkY >= 0 && newChunkY < MAP_SIZE) {
                BlockType newType = worldMap[newChunkX][newChunkY].type;
                if ((newType == WATER || newType == ICE) && !checkCollision(newX, newY)) {
                    canMove = true;
                }
            }
        } else if (mob->type == SANDSTORM_SUMMON) {
            // Summoned sandstorm can move anywhere walkable
            canMove = isWalkable(newX, newY ,1) && !checkCollision(newX, newY);
        } else {
            canMove = isWalkable(newX,  mob->y, 0) && isWalkable(mob->x,  newY, 0) && 
                      isWalkable(newX,  newY, 0) && !checkCollision(newX, mob->y) && 
                      !checkCollision(mob->x, newY);
        }
        
        if (canMove) {
            // Update body segments for leech
            if (mob->type == LEECH && !vec_empty(mob->bodySegments)) {
                int i;
                double oldHeadX = mob->x;
                double oldHeadY = mob->y;
                
                int l=vec_size(mob->bodySegments);
                for (i = l - 1; i > 0; i--) {
                    vec_at(mob->bodySegments, i).first = vec_at(mob->bodySegments, i-1).first;
                    vec_at(mob->bodySegments, i).second = vec_at(mob->bodySegments, i-1).second;
                }
                
                if (!vec_empty(mob->bodySegments)) {
                    vec_at(mob->bodySegments, 0).first = oldHeadX;
                    vec_at(mob->bodySegments, 0).second = oldHeadY;
                }
            }
            
            // Update chunk tracking
            int newChunkX = (int)(newX);
            int newChunkY = (int)(newY);
            
            if (newChunkX != mob->chunkX || newChunkY != mob->chunkY) {
                Vec_int* oldChunkMobs = &worldMap[mob->chunkX][mob->chunkY].mobIds;
                vec_remove_int(oldChunkMobs, mob->id);
                
                vec_push(worldMap[newChunkX][newChunkY].mobIds, mob->id);
                
                mob->chunkX = newChunkX;
                mob->chunkY = newChunkY;
            }
            
            mob->x = newX;
            mob->y = newY;
        } else {
            // Bounce off obstacles
            mob->vx *= -0.3;
            mob->vy *= -0.3;
            mob->targetAngle += 3.14159;
            
            if (mob->type != STARFISH && mob->type != LEECH && mob->type != SANDSTORM_SUMMON) {
                mob->dir = mob->targetAngle;
            }
        }
    }
}

int getUIFontSize(int renderSize) {
    return 16;
}

int getMobLevelFontSize(int renderSize) {
    return max(8, renderSize/6);
}

/* Green -> yellow -> orange as the bar empties, so "nearly dead" reads as
 * nearly dead at a glance instead of needing the numbers.  It stops at
 * orange rather than red because pure red is what the damage ghost behind
 * it is drawn in, and the two would become indistinguishable. */
/* florr reads the bar as green at full health and only warms up as it
 * drains, so the top of the ramp is 69d139 instead of pure yellow.  It
 * still stops at orange rather than red because pure red is what the
 * damage ghost behind it is drawn in, and the two would blur together. */
static COLORREF healthBarColor(double pct) {
    double a = 0, r = 105, g = 209, b = 57;
    if (pct < 0.1){
        a = (0.1-pct) * 2560.0;
    }
    if (a < 0) a = 0; if (a > 255) a = 255;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ARGB(a, r, g, b);
}
static COLORREF healthShellColor(double pct) {
    double a = 0, r = 255, g = 255, b = 255;
    if (pct < 0.1){
        a = (0.1-pct) * 2560.0;
    }
    if (a < 0) a = 0; if (a > 255) a = 255;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ARGB(a, r, g, b);
}
/* Outline thickness for rock / bush and for the health-bar border.
 * Deliberately CONSTANT: it used to be borderWidth/2, which made the
 * outline on a big rock several times thicker than on a small one, and
 * renderSize/16 for the bar, which grew with the zoom.  Now every mob
 * gets the same crisp edge regardless of size. */
#define BORDER_STROKE  2

/* One rounded slab.  The corner radius is capped by the *fill* width as
 * well as the height, otherwise a nearly empty bar renders as a rectangle
 * with a circle stuck on its left edge. */
static void hbSlab(double l, double t, double w, double h, COLORREF c) {
    double r = h / 2.0;
    if (w <= 0.0) return;
    if (r > w / 2.0) r = w / 2.0;
    setfillcolor(c);
    solidroundrect(l, t, l + w, t + h, r, r);
}

/* The HUD is drawn straight onto the terrain, so white-on-sand and
 * white-on-ice was simply invisible.  A translucent strip behind it fixes
 * that without hiding the world. */
static void hudPanel(double l, double t, double r, double b) {
    setfillcolor(ARGB(100, 0, 0, 0));
    solidrectangle(l, t, r, b);
}

/* Boss rows used to sit on a translucent black panel so their labels stayed
 * readable, but the panel hid a chunk of the world behind every bar.  They
 * are drawn straight onto the terrain now, so the labels carry their own
 * 1px outline instead - same legibility, nothing covered up.  Only ever
 * four of them, so the extra draws cost nothing. */
static void outlinedText(int x, int y, COLORREF face, const char* s) {
    settextcolor(BLACK);
    outtextxy(x - 1, y, s);
    outtextxy(x + 1, y, s);
    outtextxy(x, y - 1, s);
    outtextxy(x, y + 1, s);
    settextcolor(face);
    outtextxy(x, y, s);
}

void drawHealthBar(double x, double y, double width, double height, double healthPercent, double oldhealthPercent, int renderSize) {
    int screenX = (int)(x * renderSize);
    int screenY = (int)(y * renderSize);
    int barWidth = (int)(width * renderSize);
    int barHeight = (int)(height * renderSize);
    
    /* florr style: fully rounded ends and a thick outline.  The border is
     * drawn last so it frames the fill instead of being half covered by it. */
    int borderW = BORDER_STROKE;
    double r = barHeight / 2.0;
    int healthWidth;

    // Background (black)
    setfillcolor(BLACK);
    solidroundrect((double)screenX, (double)screenY,
                   (double)(screenX + barWidth), (double)(screenY + barHeight), r, r);

     // Health (red ghost)
    if(healthPercent<=1){
    	oldhealthPercent=min(oldhealthPercent,1.0);
    	healthWidth = (int)(barWidth * oldhealthPercent);
    	hbSlab((double)screenX, (double)screenY, (double)healthWidth, (double)barHeight, RED);
	}


    // Health (green)
    if(healthPercent<=1){
    	healthWidth = (int)(barWidth * healthPercent);
    	hbSlab((double)screenX, (double)screenY, (double)healthWidth, (double)barHeight,
    	       healthBarColor(healthPercent));
	}else{
		hbSlab((double)screenX, (double)screenY, (double)barWidth, (double)barHeight,
		       healthBarColor(1.0));
    	healthWidth = (int)(barWidth * (healthPercent-1));
    	hbSlab((double)screenX + barWidth/40.0, (double)screenY + barHeight/4.0, (double)healthWidth - healthWidth/20.0,
    	       (double)(barHeight/2.0), healthShellColor(healthPercent - 1.0));
	}


    // Border
    setlinecolor(BLACK);
    setlinestyle(PS_SOLID, borderW);
    roundrect((double)screenX, (double)screenY,
              (double)(screenX + barWidth), (double)(screenY + barHeight), r, r);
    setlinestyle(PS_SOLID, 1);
}

typedef struct BOSSBAR{
	double hp;
	double ohp;
	String type;
	String rarity;
	COLORREF color;
} BOSSBAR;
Vec_BOSSBAR boss;

/* Enough for the longest leech: numSegments = 5*rarity + rand()%3*rarity,
 * rarity maxes out at 10, so 70 segments plus the head. */
#define LEECH_MAX_PTS 80

/* Flat colour blocks read as a wall of one colour.  Every chunk gets a
 * +-6% brightness nudge derived from its coordinates - deterministic, so
 * the same chunk looks the same every frame instead of shimmering.
 * Multiplicative rather than additive so dark soil does not go grey.
 */
static COLORREF chunkShade(COLORREF c, int cx, int cy) {
    unsigned h = (unsigned)((unsigned)cx * 73856093u ^ (unsigned)cy * 19349663u);
    int m = 256 + (int)(h % 33) - 16;        /* 240 .. 272, i.e. +-6% */
    int r = GetRValue(c) * m / 256;
    int g = GetGValue(c) * m / 256;
    int b = GetBValue(c) * m / 256;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return RGB(r, g, b);
}

/* ---- mob labels ------------------------------------------------------
 * These were local arrays rebuilt on the stack for every mob of every
 * frame, and their strings were then sprintf()'d into a scratch buffer
 * even though a label is a literal that needs no formatting at all.
 * Now they are file scope and drawMob() just points at them.
 */
static const char* g_rarityNames[] = {"Summon", "Common", "Unusual", "Rare", "Epic", "Legendary", "Mythic", "Ultra", "Super", "Eternal", "Hyper"};
static const char* g_mobNames[] = {"Baby Ant", "Worker Ant", "Soldier Ant", "Bush", "Shell", "Rock", "Starfish", "Leech", "Ladybug", "Sandstorm", "Sandstorm","Coral","Bee", "Ladybug", "Ladybug","Cactus","Pearl"};

/* textwidth() walks the string glyph by glyph.  drawMob() measured the
 * same 28 labels for every mob of every frame - 28 measurements per mob
 * that always returned the same number.  They are literals, so the
 * widths only change when the font does: cache them per font height. */
static int g_rarityW[11];
static int g_mobNameW[17];
static int g_labelFontH = -1;

static void cacheLabelWidths(int fontH) {
    int k;
    if (g_labelFontH == fontH) return;
    for (k = 0; k < 11; k++) g_rarityW[k]  = textwidth(g_rarityNames[k]);
    for (k = 0; k < 17; k++) g_mobNameW[k] = textwidth(g_mobNames[k]);
    g_labelFontH = fontH;
}

/* The body radius drawMob() ends up with, with none of the drawing.  It
 * was inline arithmetic before; extracting it lets renderGame() cull with
 * the exact radius a mob will be drawn at instead of guessing a margin,
 * and keeps the two places from drifting apart. */
static int mobDrawSize(const Mob* mob, int renderSize, bool showMode) {
    int size = getMobSize((Mob*)mob, showMode) * renderSize / 50;
    size /= 2;
    if (mob->type == SHELL) size *= 1.5;
    if (mob->type == BUSH) size *= 2;
    if (mob->type == STARFISH) size *= 2;
    if (mob->type == ROCK) size *= 2;
    if (mob->type <= 2) size *= 1.2;
    if (mob->type == LADYBUG) size *= 1.7;
    if (mob->type == SANDSTORM) size *= 2;
    if (mob->type == BEE) size *= 3;
    if (mob->type == CORAL) size /= 0.7;
    if (mob->type == CACTUS) size /= 0.7;
    if (mob->type == DARK_LADYBUG) size *= 1.7;
    if (mob->type == SHINY_LADYBUG) size *= 1.7;
    if (mob->type == PEARL) size *= 1.6;
    return size;
}

/* All three ladybug species are the same drawing with a different palette,
 * so it lives here once instead of being pasted three times.
 *
 * Layered outside-in the way the official SVG is: a dark rim, the shell,
 * a crescent of shade along the trailing edge, the wing spots, then the
 * head.  The crescent is not clipped - it is a shell-sized disc of shade
 * with a slightly smaller shell disc nudged the other way painted over it,
 * so what stays visible is the crescent.  Same shape, no clip path.
 *
 * cx/cy/r are doubles on purpose: the caller already has untruncated
 * screen coordinates, and rounding here is what makes a walking mob
 * stutter by a pixel. */
/* All three ladybug species are the same drawing with a different palette,
 * so it lives here once instead of being pasted three times.
 *
 * Read off the official ladybug SVG, outside in: a shade disc that survives
 * as a rim, the shell on top of it, a crescent of shade across the trailing
 * edge, then three spots.
 *
 * The old version invented a head disc and a wing seam that are not in the
 * SVG at all, and placed four spots at positions it made up.  The layout
 * below is measured off the rasterised SVG: the shell is 25.5 of the 29
 * unit silhouette, and the spots sit at (-14.6,-14.3), (-3.6,+2.4) and
 * (+1.7,+21.5) from its centre in those same units.
 *
 * lx/ly are in shell radii with the head along +x, so the whole spot set
 * rotates with dir as one rigid piece.
 *
 * cx/cy/r are doubles on purpose: the caller already has untruncated screen
 * coordinates, and rounding here is what makes a walking mob stutter. */
static void drawLadybugShell(double cx, double cy, double r, double dir,
                             COLORREF body, COLORREF spot, COLORREF shade)
{
    static const double spots[3][3] = {   /* lx,    ly,   radius (x shell r) */
        { -0.573, -0.561, 0.184 },        /* upper left, the big one        */
        { -0.142,  0.093, 0.184 },        /* just below the middle          */
        {  0.065,  0.841, 0.083 },        /* small one, low down            */
    };
    double dx = cos(dir), dy = sin(dir);
    double sr = r * 0.88;            /* shell radius - 25.5 of 29 units   */
    int i, k, n;
    POINTF cres[2 * 13];
    
    int headSize = r * 0.5;
    int headX = cx - (int)(cos(dir) * r * 0.8);
    int headY = cy - (int)(sin(dir) * r * 0.8);
            
    setfillcolor(BLACK);
    solidcircle(headX, headY, headSize);

    setfillcolor(shade);
    solidcircle((int)cx, (int)cy, (int)r);

    setfillcolor(body);
    solidcircle((int)cx, (int)cy, (int)sr);

    /* Crescent over the trailing edge.  The outer arc rides the shell rim,
     * the inner one bows back in, and sin() tapers both ends to a point so
     * it reads as a crescent rather than a second ring.  Measured off the
     * SVG it is about 0.39 of the shell radius deep where it is thickest. */
    n = 0;
    for (k = 0; k <= 12; k++) {
        double t = (double)k / 12.0;
        double a = dir + 3.14159265 + (t - 0.5) * 1.51;   /* ~86 deg wide */
        cres[n].x = (float)(cx + cos(a) * sr);
        cres[n].y = (float)(cy + sin(a) * sr);
        n++;
    }
    for (k = 12; k >= 0; k--) {
        double t  = (double)k / 12.0;
        double a  = dir + 3.14159265 + (t - 0.5) * 1.51;
        double rr = sr * (1.0 - 0.39 * sin(3.14159265 * t));
        cres[n].x = (float)(cx + cos(a) * rr);
        cres[n].y = (float)(cy + sin(a) * rr);
        n++;
    }
    solidpolygonf(cres, n);

    setfillcolor(spot);
    for (i = 0; i < 3; i++) {
        double lx   = spots[i][0] * sr;
        double ly   = spots[i][1] * sr;
        double srad = spots[i][2] * sr;
        solidcircle((int)(cx + lx * dx - ly * dy),
                    (int)(cy + lx * dy + ly * dx), (int)srad + 1);
    }
    setlinestyle(PS_SOLID, 1);
}

void drawMob(Mob* mob, int centerX, int centerY, int renderSize, bool showMode) {
	if (Mob_isDead(mob)){
		return;
	}
	int renderSizez;
	if(showMode)renderSizez=1;
	else renderSizez=renderSize;
	
	if(showMode){
		mob->dir=0;
	}
	
    double screenX = centerX + (mob->x - player.x) * renderSizez;
    double screenY = centerY + (mob->y - player.y) * renderSizez;
    
    int size = mobDrawSize(mob, renderSize, showMode);

    /* Zoomed out, a mob is 2-4 px across.  Everything below draws ten
     * or more primitives plus two labelled strings, none of which is
     * readable at that size - and with a thousand mobs on screen it was
     * the single most expensive thing in the frame.  One blob in the
     * mob's own colour carries the same information at that zoom. */
    if (!showMode && size < 5) {
        /* solidcircle(), not fillcircle(): in easygl fillcircle() is
         * solidcircle() PLUS circle(), so it strokes an outline with
         * whatever line style the previous mob left behind - a
         * ladybug's thick red border around a 2 px pebble.  The blob
         * only wants the fill, and skipping the outline is one less
         * ring of vertices per mob. */
        setfillcolor(mobColors[mob->type]);
        solidcircle(screenX, screenY, max(1.0, (double)size));
        return;
    }
    
    int AntborderWidth = size;
    int BushRockborderWidth = size * 3 / 5;
    int BeeborderWidth = size * 3 / 5;
    int LeechborderWidth = size * 3 / 5;
    int PearlborderWidth = size * 3 / 5;
    
    if(showMode){
		vec_clear(mob->bodySegments);
	}
    
    /* Two labels, each with a 1px drop shadow: four outtextxy() per
     * mob, and every outtextxy() formats a 352 byte key string and
     * hashes it twice.  Below renderSize 32 the mob is a few pixels
     * across and the text is an unreadable smear, so drop the lot -
     * labels, health bar and all. */
    bool showLabel = showMode || renderSize >= 32;
    LOGFONT oldFont;
    getfont(&oldFont);
    
    COLORREF antColor = (mob->rarity >= 1 && mob->rarity <= 10) ? rarityColors[mob->rarity] : YELLOW;
    
    if (showLabel) {
    LOGFONT levelFont = oldFont;
    levelFont.lfHeight = getMobLevelFontSize(renderSize);
    levelFont.lfWidth = 0;
    strcpy(levelFont.lfFaceName, "Arial");
    setfont(&levelFont);
    
    /* Labels are literals: point at them instead of sprintf()ing a copy,
     * and take the width from the cache instead of measuring per mob. */
    int rIdx = (mob->type == SANDSTORM_SUMMON) ? 0 : mob->rarity;
    const char* rname = g_rarityNames[rIdx];
    const char* tname = g_mobNames[mob->type];
    cacheLabelWidths(levelFont.lfHeight);
    int textWidth = g_rarityW[rIdx];
    double textX = screenX - textWidth/2;
    double textY = screenY + (mob->type==BEE?size*0.8:size*1.2) + 10;
    
    setbkmode(TRANSPARENT);
    /* One pixel drop shadow.  Labels sit on grass, sand, water and ice,
     * and a flat colour is unreadable on at least one of them. */
    if (showMode) {
        settextcolor(antColor);
        outtextxy(textX, textY, rname);
    } else {
        settextcolor(BLACK);
        outtextxy(textX + 1, textY + 31, rname);
        settextcolor(antColor);
        outtextxy(textX, textY + 30, rname);
    }
    settextcolor(BLACK);
    if (showMode) outtextxy(textX + 1, textY - 9,  tname);
    else          outtextxy(textX + 1, textY + 6,  tname);
    settextcolor(WHITE);
    if (showMode) outtextxy(textX,     textY - 10, tname);
    else          outtextxy(textX,     textY + 5,  tname);
    
    if(showMode){
    	char Text[20];
	    sprintf(Text, "%.f", mob->maxHealth);
	    
	    int textWidth = textwidth(Text);
	    int textX = screenX - textWidth/2;
	    int textY = screenY + (mob->type==BEE?size*0.8:size*1.2) + 10;
	    
	    settextcolor(WHITE);
	    setbkmode(TRANSPARENT);
	    outtextxy(textX, textY+10, Text);
	    settextcolor(WHITE);
	    sprintf(Text, "%.f", mob->bodyDamage);
		outtextxy(textX, textY+20, Text);
	}
    
    
    settextcolor(WHITE);
    
    
    
    
    // Draw health bar below rarity text
    if (!Mob_isDead(mob) && !showMode) {
        double healthPercent = mob->health / mob->maxHealth;
        double oldhealthPercent = mob->oldHealth / mob->maxHealth;
        drawHealthBar(textX - 10, textY + 20, 40, 5, healthPercent, oldhealthPercent, 1);
    }
    /* The boss bar used to be pushed here, gated on the same wide box
     * renderGame() used to cull with.  renderGame() collects it now, so a
     * boss that is off screen still gets a bar even though drawMob() is
     * skipped for it. */
    
    setfont(&oldFont);
    }
    
    setlinestyle(PS_SOLID, 1);
     
    // Flash effect when taking damage
    COLORREF mobColor = mobColors[mob->type];
    if (mob->damageFlashTimer > 0) {
        // Make brighter based on damage ratio
        double brightness = 1.0 + (mob->lastDamageTaken / mob->maxHealth) * 2.0;
        mobColor = RGB(min(255, (int)(GetRValue(mobColor) * brightness)),
                      min(255, (int)(GetGValue(mobColor) * brightness)),
                      min(255, (int)(GetBValue(mobColor) * brightness)));
    }
    
    switch (mob->type) {
        case BABY_ANT: {
            setlinecolor(RGB(69, 69, 69));
            setlinestyle(PS_SOLID, AntborderWidth);
            circle(screenX, screenY, size);
            setfillcolor(mobColor);
            solidcircle(screenX, screenY, size);
            break;
        }
        case WORKER_ANT: {
            int headSize = size * 0.7;
            int headX = screenX + (int)(cos(mob->dir) * size * 1.2);
            int headY = screenY + (int)(sin(mob->dir) * size * 1.2);
            
            setfillcolor(mobColor);
            setlinecolor(RGB(69, 69, 69));
            setlinestyle(PS_SOLID, AntborderWidth);
            circle(headX, headY, headSize);
            solidcircle(headX, headY, headSize);
            
            setlinecolor(RGB(69, 69, 69));
            setlinestyle(PS_SOLID, AntborderWidth);
            circle(screenX, screenY, size);
            solidcircle(screenX, screenY, size);
            break;
        }
        case SOLDIER_ANT: {
            int headSize = size * 0.7;
            int headX = screenX + (int)(cos(mob->dir) * size * 1.2);
            int headY = screenY + (int)(sin(mob->dir) * size * 1.2);
            
            setfillcolor(mobColor);
            setlinecolor(RGB(69, 69, 69));
            setlinestyle(PS_SOLID, AntborderWidth);
            circle(headX, headY, headSize);
            solidcircle(headX, headY, headSize);
            
            setfillcolor(RGBA(255, 255, 255, 105));
            
            double leftWingDir = mob->dir + 3.14159/2;
            double rightWingDir = mob->dir - 3.14159/2;
            
            float wingScale = 3.0f;
            
            POINT leftWing[3];
            leftWing[0].x = headX + (int)(cos(leftWingDir) * headSize * 0.9);
            leftWing[0].y = headY + (int)(sin(leftWingDir) * headSize * 0.9);
            leftWing[1].x = headX + (int)(cos(leftWingDir) * headSize * wingScale);
            leftWing[1].y = headY + (int)(sin(leftWingDir) * headSize * wingScale);
            leftWing[2].x = headX + (int)(cos(leftWingDir + 3.14159/6) * headSize * wingScale * 0.8);
            leftWing[2].y = headY + (int)(sin(leftWingDir + 3.14159/6) * headSize * wingScale * 0.8);
            solidpolygon(leftWing, 3);
            
            POINT rightWing[3];
            rightWing[0].x = headX + (int)(cos(rightWingDir) * headSize * 0.9);
            rightWing[0].y = headY + (int)(sin(rightWingDir) * headSize * 0.9);
            rightWing[1].x = headX + (int)(cos(rightWingDir) * headSize * wingScale);
            rightWing[1].y = headY + (int)(sin(rightWingDir) * headSize * wingScale);
            rightWing[2].x = headX + (int)(cos(rightWingDir - 3.14159/6) * headSize * wingScale * 0.8);
            rightWing[2].y = headY + (int)(sin(rightWingDir - 3.14159/6) * headSize * wingScale * 0.8);
            solidpolygon(rightWing, 3);
            
            setfillcolor(mobColor); 
            setlinecolor(RGB(69, 69, 69));
            setlinestyle(PS_SOLID, AntborderWidth);
            circle(screenX, screenY, size);
            solidcircle(screenX, screenY, size);
            break;
        } 
        case BUSH: {
            int i;
            POINT bus[10];
            int points = 5;
            for (i = 0; i < points * 2; i++) {
                double angle = i * 3.14159 / points + mob->dir;
                int radius = (i % 2 == 0) ? size : size * 0.6;
                bus[i].x = screenX + (int)(cos(angle) * radius);
                bus[i].y = screenY + (int)(sin(angle) * radius);
            }
            setlinecolor(C_BUSH_LINE);
            setlinestyle(PS_SOLID, BORDER_STROKE * size / 20.0);
            setfillcolor(mobColor);
			setstrokecap(GX_CAP_ROUND);
            setstrokejoin(GX_JOIN_ROUND);
            fillstrokepolygon(bus, 10);
            setlinestyle(PS_SOLID, BushRockborderWidth);
            break;
        }
        case SHELL: {
            /* Off the official shell SVG: a scalloped fan hinged at the back,
             * a darker tan rim and four ribs radiating from the hinge.  It used
             * to be a plain disc, which reads as a coin rather than a shell. */
            double r = (double)size;
            int n, i;
            POINT fan[28];
            int   fanN = 0;
            /* The fan spans about 200 degrees, split into 12 lobes; every other
             * vertex is pulled in so the rim reads as scallops. */
            for (i = 0; i <= 24; i++) {
                double t    = (double)i / 24.0;
                double a    = mob->dir + (t - 0.5) * 3.49;      /* ~200 deg */
                double lobe = 0.90;
                double rr   = r * lobe;
                fan[fanN].x = (long)(screenX + cos(a) * rr);
                fan[fanN].y = (long)(screenY + sin(a) * rr);
                fanN++;
            }
            /* close through the hinge, which sits behind the mob */
            {
                double hx = screenX - cos(mob->dir) * r * 0.62;
                double hy = screenY - sin(mob->dir) * r * 0.62;
                fan[fanN].x = (long)hx; fan[fanN].y = (long)hy; fanN++;
            }
            setlinecolor(C_SHELL_RIM);
            setlinestyle(PS_SOLID, (int)(r * 0.09) + 1);
            setfillcolor(mobColor);
            fillpolygon(fan, fanN);
            /* four ribs from the hinge out towards the rim */
            for (n = 0; n < 4; n++) {
                double a = mob->dir - 0.90 + (double)n * 0.60;
                double c = cos(a), s = sin(a);
                line((int)(screenX - cos(mob->dir) * r * 0.50),
                     (int)(screenY - sin(mob->dir) * r * 0.50),
                     (int)(screenX + c * r * 0.74),
                     (int)(screenY + s * r * 0.74));
            }
            setlinestyle(PS_SOLID, 1);
            /* the hinge knob itself */
            setfillcolor(C_SHELL_RIM);
            solidcircle((int)(screenX - cos(mob->dir) * r * 0.62),
                        (int)(screenY - sin(mob->dir) * r * 0.62),
                        (int)(r * 0.16) + 1);
            break;
        }
        case ROCK: {
            int i;
            POINT rock[32];
            int points;
			if(!showMode)points = ((int)mob->x^(int)mob->y)%3+3+mob->rarity;
			else points=5;
            for (i = 0; i < points * 2; i++) {
                double angle = i * 3.14159 / points + mob->dir;
                double radius;
				if(!showMode)radius = (abs(((int)(mob->x*10000)) ^ ((int)(mob->y*10000)) ^ i ^ (10-i) ^ 122343)%8/40.0+0.7) * size;
                else radius = (i^1145 % 2 == 0) ? size * 0.7 : size * 0.9;
				rock[i].x = screenX + (int)(cos(angle) * radius);
                rock[i].y = screenY + (int)(sin(angle) * radius);
            }
            setlinecolor(C_ROCK_LINE);
            setlinestyle(PS_SOLID, BORDER_STROKE);
            setfillcolor(mobColor);
            fillpolygon(rock, points*2);
            setlinestyle(PS_SOLID, BushRockborderWidth);
            break;
        }
        case STARFISH: {
            int i;
            POINT star[10];
            int points = 5;
            for (i = 0; i < points * 2; i++) {
                double angle = i * 3.14159 / points + mob->dir;
                int radius = (i % 2 == 0) ? size : size * 0.4;
                star[i].x = screenX + (int)(cos(angle) * radius);
                star[i].y = screenY + (int)(sin(angle) * radius);
            }
            
            setfillcolor(mobColor);
			setlinecolor(RGB(169, 64, 62));
            setlinestyle(PS_SOLID,BORDER_STROKE * size / 10.0);
            setstrokecap(GX_CAP_ROUND);
            setstrokejoin(GX_JOIN_ROUND);
            fillstrokepolygon(star,10);
            break;
        }
        case LEECH: {
            /* The body used to be a run of thick lines, which is one quad per
             * segment, so consecutive segments overlapped at the joints and a
             * translucent colour got blended twice there.
             *
             * It is now a strip of quads: quad i spans cross sections i and
             * i+1, so neighbours share an entire edge and every pixel of the
             * body is covered exactly once.  The alpha below is therefore
             * applied once and the body reads as one even tone.
             *
             * The whole run goes in as one outline polygon, not one quad per
             * segment.  When the body curves back on itself - and it does,
             * because the segments are past positions - the outline self
             * intersects, and the scanline fill runs under the WINDING rule:
             * the folded over part has winding 2 and is still filled exactly
             * once.  Splitting it into one quad per segment looks safer but
             * is not: quads that turn back on each other overlap, and those
             * pixels get the translucent colour blended twice, which shows up
             * as a dark lump at every tight turn.
             *
             * Both ends get a half disc so the body does not end in a square
             * cut.  The cap starts and ends on the end cross section, so it
             * shares that edge with the outline instead of overlapping it. */
            if (!vec_empty(mob->bodySegments)) {
                int  nseg = vec_size(mob->bodySegments);
                int  total, i;
                POINTF spine[LEECH_MAX_PTS];

                if (nseg > LEECH_MAX_PTS - 1) nseg = LEECH_MAX_PTS - 1;
                total = nseg + 1;              /* head + one point per segment */
 
                /* Head first, then segment 0..n-1 - segment 0 is the one that
                 * trails the head, so this walks head -> tail. */
                spine[0].x = (float)screenX;
                spine[0].y = (float)screenY;
                for (i = 1; i < total; i++) {
                    spine[i].x = (float)(centerX + (vec_at(mob->bodySegments, i - 1).first  - player.x) * renderSizez);
                    spine[i].y = (float)(centerY + (vec_at(mob->bodySegments, i - 1).second - player.y) * renderSizez);
                }
 
                setlinecolor(ARGB(80, GetRValue(mobColor), GetGValue(mobColor), GetBValue(mobColor)));
                /* One ribbon, round caps and round joins, filled once: every
                 * pixel gets the translucent colour exactly once even where
                 * the body folds back over itself. */
                setstrokecap(GX_CAP_ROUND);
                setstrokejoin(GX_JOIN_ROUND);
                setlinestyle(PS_SOLID,size * 2);
                strokepolylinef(spine, total);
            }

            // Draw head
            setfillcolor(mobColor);
            setlinecolor(mobColor);
            setlinestyle(PS_SOLID, LeechborderWidth);
            circle(screenX, screenY, size);
            solidcircle(screenX, screenY, size);
            break;
        }
        case LADYBUG: {
            drawLadybugShell(screenX, screenY, (double)size, mob->dir,
                             mobColor, C_LB_SPOT, C_LB_SHADE);
            break;
        }
        case SANDSTORM: {
            int i;
            POINT ss[10],sss[10];
            int points = 3;
            for (i = 0; i < points * 2; i++) {
                double angle = i * 3.14159 / points + mob->dir;
                int radius = size;
                ss[i].x = screenX + (int)(cos(angle) * radius);
                ss[i].y = screenY + (int)(sin(angle) * radius);
                sss[i].x = screenX + (int)(sin(angle) * radius *0.5);
                sss[i].y = screenY + (int)(cos(angle) * radius *0.5);
            }
            setfillcolor(mobColor);
            solidpolygon(ss, points*2);
            setfillcolor(RGB(220, 220, 180));
            solidpolygon(sss, points*2);
            break;
        }
        case SANDSTORM_SUMMON: {
            int i;
		    POINT ss[10],sss[10];
		    int points = 3;
		    for (i = 0; i < points * 2; i++) {
		        double angle = i * 3.14159 / points + mob->dir;
		        int radius = size;
		        ss[i].x = screenX + (int)(cos(angle) * radius);
		        ss[i].y = screenY + (int)(sin(angle) * radius);
		        sss[i].x = screenX + (int)(sin(angle) * radius *0.5);
                sss[i].y = screenY + (int)(cos(angle) * radius *0.5);
		    }
		    setfillcolor(mobColor);
		    solidpolygon(ss, points*2);
		    setfillcolor(RGB(240, 230, 150));
		    solidpolygon(sss, points*2);
		    
		    break;
		}case CORAL: {
			int i;
		    POINT cr[100];
		    POINT crs[100];
		    int points = 50;
		    for (i = 0; i < points * 2; i++) {
		        double angle = i * 3.14159 / points + mob->dir;
		        int radius = (sin(i)/1.5+10)*size/8;
		        cr[i].x = screenX + (int)(cos(angle) * radius);
		        cr[i].y = screenY + (int)(sin(angle) * radius);
		        crs[i].x = screenX + (int)(cos(angle) * radius * 0.8);
		        crs[i].y = screenY + (int)(sin(angle) * radius * 0.8);
		    }
		    setfillcolor(ARGB(55,GetRValue(mobColor),GetGValue(mobColor),GetBValue(mobColor)));
		    solidpolygon(cr, points*2);
		    COLORREF coralc;
			if(!showMode)coralc=RGB(((int)(mob->x*100)^114514)%50+180,130,((int)(mob->y*100)^114514)%25+210);
			else coralc=RGB(205,130,223);
		    setfillcolor(coralc);
		    solidpolygon(crs, points*2);
		    break;
		}
		case BEE: {
			int i;
		    double dirX = cos(mob->dir);
		    double dirY = sin(mob->dir);
		    
		    // Adjust body shape based on direction - fatter when diagonal, thinner when vertical
		    int bodyWidth = size * 1.4;   // Base width
		    int bodyLength = size * 1.2;  // Base length
		    
		    // Make body thinner when more vertical
		    double verticalFactor = abs(dirY);  // 0 = horizontal, 1 = vertical
		    bodyWidth = (int)(bodyWidth * (1.0 - verticalFactor * 0.3));  // Up to 30% thinner when vertical
		    
		    // Body center
		    int bodyCenterX = screenX;
		    int bodyCenterY = screenY;
		    
		    // Draw body as circle (not ellipse)
		    setfillcolor(mobColors[BEE]);
		    setlinecolor(BLACK);
		    setlinestyle(PS_SOLID, 1);
		    
		    // Use circle instead of ellipse
		    int bodyRadius = (bodyWidth + bodyLength) / 4;  // Average radius
		    circle(bodyCenterX, bodyCenterY, bodyRadius);
		    solidcircle(bodyCenterX, bodyCenterY, bodyRadius);
		    
		    // Black stripes (3 stripes perpendicular to body direction)
		    setlinecolor(BLACK);
		    setlinestyle(PS_SOLID, BeeborderWidth/6);
		    
		    for (i = -1; i <= 1; i++) {
		        double stripeRatio = i * 0.53;
		        
		        // Stripe center along body direction
		        int stripeCenterX = bodyCenterX + (int)(dirX * stripeRatio * bodyRadius);
		        int stripeCenterY = bodyCenterY + (int)(dirY * stripeRatio * bodyRadius);
		        
		        // Perpendicular direction for stripe ends
		        double perpX = -dirY;
		        double perpY = dirX;
		        
		        int stripeLeftX = stripeCenterX + (int)(perpX * bodyRadius * (i==0?0.8:0.6));
		        int stripeLeftY = stripeCenterY + (int)(perpY * bodyRadius * (i==0?0.8:0.6));
		        int stripeRightX = stripeCenterX - (int)(perpX * bodyRadius * (i==0?0.8:0.6));
		        int stripeRightY = stripeCenterY - (int)(perpY * bodyRadius * (i==0?0.8:0.6));
		        
		        line(stripeLeftX, stripeLeftY, stripeRightX, stripeRightY);
		    }
		    
		    
		    // Stinger at FRONT (opposite direction)
		    int stingerSize = bodyRadius / 2;
		    int stingerBaseX = bodyCenterX + (int)(dirX * bodyRadius);
		    int stingerBaseY = bodyCenterY + (int)(dirY * bodyRadius);
		    int stingerTipX = stingerBaseX + (int)(dirX * stingerSize);
		    int stingerTipY = stingerBaseY + (int)(dirY * stingerSize);
		    
		    // Stinger base width
		    double stingerPerpX = -dirY;
		    double stingerPerpY = dirX;
		    int stingerLeftX = stingerBaseX + (int)(stingerPerpX * stingerSize/2);
		    int stingerLeftY = stingerBaseY + (int)(stingerPerpY * stingerSize/2);
		    int stingerRightX = stingerBaseX - (int)(stingerPerpX * stingerSize/2);
		    int stingerRightY = stingerBaseY - (int)(stingerPerpY * stingerSize/2);
		    
		    POINT stingerPoly[3] = {
		        {stingerLeftX, stingerLeftY},
		        {stingerRightX, stingerRightY},
		        {stingerTipX, stingerTipY}
		    };
		    setfillcolor(BLACK);
		    solidpolygon(stingerPoly, 3);
		    
		    break;
		}
        case DARK_LADYBUG: {
            drawLadybugShell(screenX, screenY, (double)size, mob->dir,
                             mobColor, C_DLB_SPOT, C_DLB_SHADE);
            break;
        }
        case SHINY_LADYBUG: {
            drawLadybugShell(screenX, screenY, (double)size, mob->dir,
                             mobColor, C_SLB_SPOT, C_SLB_SHADE);
            break;
        }
        case CACTUS: {
            /* A cactus is a fluted column, so the silhouette still scallops -
             * but only gently: the crest-to-valley gap used to be ~36% of the
             * radius and read as a gear wheel rather than a plant.
             *
             * Spines are no longer one-per-rib.  Their pitch is a fixed
             * number of WORLD pixels (scaled by renderSize, so zooming out
             * with T keeps the same look), which means every spine is the
             * same size and the same distance apart no matter how big the
             * cactus is: a Mythic one carries ~50 of them, a Common one ~7.
             * Drawn black and LAST, on top of the body, with the roots
             * sitting just inside the body edge so each spine grows
             * straight out of the green. */
            int i, j;
            POINT ca[32];
            POINT cas[32];
            POINT sp[4];
            const int ribs = 8;
            const int N    = ribs * 4;
            double pitch;                       /* world px between spines */
            double ravg;
            int nsp;
            for (i = 0; i < N; i++) {
                double angle = (double)i / N * 2 * 3.14159265 + mob->dir;
                double rib   = cos((double)i * 2 * 3.14159265 * ribs / N);
                double wob   = fabs(sin((double)i)) * 0.25;   /* slight unevenness */
                double r     = (10.10 + 0.55 * rib - wob) * size / 8;
                ca[i].x   = (int)(screenX + cos(angle) * r);
                ca[i].y   = (int)(screenY + sin(angle) * r);
                cas[i].x  = (int)(screenX + cos(angle) * r * 0.8);
                cas[i].y  = (int)(screenY + sin(angle) * r * 0.8);
            }
            ravg  = 10.10 * size / 8.0;
            /* 48.3 world px is what makes a Mythic cactus (radius ~384 px at
             * renderSize 72, circumference ~2416) come out at exactly 50. */
            pitch = 48.3 * (double)renderSize / 72.0;
            nsp   = (int)(2 * 3.14159265 * ravg / pitch + 0.5);
            if (nsp < 3)   nsp = 3;
            if (nsp > 160) nsp = 160;
            /* Outside in: the darker body, the lighter core, then the spines
             * straight off the body edge.  The black rim that used to sit
             * between the green and the spines is gone, so a spine now
             * grows out of the green instead of out of a black ring. */
            setfillcolor(mobColor);
            solidpolygon(ca, N);
            setfillcolor(RGB(70, 200, 70));
            solidpolygon(cas, N);
            setfillcolor(BLACK);
            for (j = 0; j < nsp; j++) {                     /* black spines */
                double a     = (double)j / nsp * 2 * 3.14159265;
                double angle = a + mob->dir;
                double c = cos(angle), s = sin(angle);
                double ribA = cos((double)ribs * a);   /* same rib the body uses */
                double rA   = (10.10 + 0.55 * ribA) * size / 8;
                double rIn  = rA * 0.92;                     /* just inside body */
                double rOut = rA * 1.09;              /* clear of the rim */
                double w    = max(1.2, min(size * 0.04, pitch * 0.10));
                double wt   = w * 0.1;                       /* narrower tip */
                sp[0].x = (int)(screenX + c * rIn  - s * w);
                sp[0].y = (int)(screenY + s * rIn  + c * w);
                sp[1].x = (int)(screenX + c * rOut - s * wt);
                sp[1].y = (int)(screenY + s * rOut + c * wt);
                sp[2].x = (int)(screenX + c * rOut + s * wt);
                sp[2].y = (int)(screenY + s * rOut - c * wt);
                sp[3].x = (int)(screenX + c * rIn  + s * w);
                sp[3].y = (int)(screenY + s * rIn  - c * w);
                solidpolygon(sp, 4);
            }
            break;
        }
		case PEARL: {
            setlinecolor(RGB(200, 200, 180));
            setlinestyle(PS_SOLID, max(1,PearlborderWidth/8));
            setfillcolor(ARGB(55,GetRValue(mobColor),GetGValue(mobColor),GetBValue(mobColor)));
            fillcircle(screenX, screenY, size);
            break;
        }
    }
    
    /* The flash timer is ticked in renderGame() now, for every mob, not
     * just the ones that survive culling - a culled mob used to keep
     * flashing forever. */
}
/* screenX/screenY are doubles now: they used to be truncated to whole
 * logical pixels, which with fixhighdpi() on is a 2 device pixel step, and
 * walking slowly showed it as a stutter. */
void drawChunk(double screenX, double screenY, int chunkX, int chunkY, int renderSize) {
    int dx, dy;
    BlockType type = worldMap[chunkX][chunkY].type;
    int cell = renderSize / 4;

    setfillcolor(chunkShade(baseColors[type], chunkX, chunkY));
    solidrectangle(screenX, screenY, screenX + renderSize, screenY + renderSize);

    if (type == WATER) {
        /* A slow swell.
         *
         * The phase is a function of the chunk's position in the *world*,
         * in chunk units.  It used to be driven by screenX/screenY, which
         * are the same chunk coordinates translated by the camera - so
         * every step the player took slid the whole pattern along and the
         * swell looked like it had sped up while walking.  Being in chunk
         * units also means the wavelength is independent of renderSize, so
         * the water does not re-pattern when you zoom out with T.
         *
         * Two translucent bands keep it to four vertices per chunk. */
        int k;
        double band = (double)cell * 0.55;
        double ph = g_waveT * 0.55 + (double)chunkX * 0.55 + (double)chunkY * 0.85;
        setfillcolor(g_waterHi);
        for (k = 0; k < 2; k++) {
            double fr = sin(ph + (double)k * 2.4) * 0.13 + (double)k * 0.42 + 0.10;
            if (fr < 0.0) fr = 0.0;
            if (fr > 0.85) fr = 0.85;
            solidrectangle(screenX, screenY + fr * (double)renderSize,
                           screenX + renderSize, screenY + fr * (double)renderSize + band);
        }
        return;
    }

    /* ICE has had a 4x4 mask generated for it since the world builder ran,
     * but drawChunk() never drew one, so ice was the only terrain with no
     * detail at all. */
    if (type != SOIL && type != GRASS && type != SAND && type != ICE)
        return;

    /* Sub-cell detail is dropped once a cell is only a few pixels wide.
     * At renderSize 12 a cell is 3 px across, so the 4x4 masks come out
     * as 3x3 px squares: invisible, and roughly 30000 rects a frame for
     * the ~3300 chunks on screen.  The base rect alone is both far
     * cheaper and cleaner looking at that zoom. */
    if (cell < 8)
        return;

    unsigned char (*pattern)[4] = NULL;

    if (type == SOIL)
        pattern = worldMap[chunkX][chunkY].collisionPattern;
    else
        pattern = worldMap[chunkX][chunkY].texturePattern;

    int ti = (type == SOIL) ? 0 : (type == GRASS ? 1 : (type == SAND ? 2 : 3));
    /* collisionPattern is the 0/1 mask the collision code shares, texture
     * masks are 0..5 with 0..2 empty, so "is this cell on" differs. */
    unsigned char thr = (type == SOIL) ? 1 : 3;

    /* The 16 sub-cells are drawn as horizontal runs instead of one rect
     * each.  Neighbouring "on" cells share an edge, so merging them covers
     * exactly the same pixels with fewer rects.  A run only continues while
     * the *tone* is unchanged, otherwise the multi-tone palette would be
     * flattened back to a single colour by the merge. */
    for (dy = 0; dy < 4; dy++) {
        int run;
        dx = 0;
        while (dx < 4) {
            unsigned char v = pattern[dx][dy];
            if (v < thr) { dx++; continue; }
            run = dx;
            while (run < 4 && pattern[run][dy] == v) run++;
            {
                double px = screenX + dx * cell;
                double py = screenY + dy * cell;
                COLORREF c = (type == SOIL) ? textureColors[0]
                                            : speckleColors[ti][(v - 3) % 3];
                setfillcolor(chunkShade(c, chunkX, chunkY));
                solidrectangle(px, py, px + (run - dx) * cell, py + cell);
            }
            dx = run;
        }
    }
}

void drawPlayer(int centerX, int centerY, int renderSize) {
    int screenX = centerX;
    int screenY = centerY;
    int radius = (int)(player.radius * renderSize);
    
    radius = max(2, radius);
    
     
    
    
    // Flash effect when taking damage
    COLORREF playerColor = player.color;
    if (player.damageFlashTimer > 0) {
        double brightness = 1.0 + (1.0 - min(player.health,player.maxHealth) / player.maxHealth) * 2.0;
        playerColor = RGB(min(255, (int)(GetRValue(playerColor) * brightness)),
                        min(255, (int)(GetGValue(playerColor) * brightness)),
                        min(255, (int)(GetBValue(playerColor) * brightness)));
        player.damageFlashTimer--;
    }
    
    setfillcolor(playerColor);
    setlinecolor(BLACK);
    solidcircle(screenX, screenY, radius);
    
    if (player.canSummonSandstorm){
    	setlinestyle(PS_SOLID,3);
    	setlinecolor(RGB(60,50,0));
    	line(screenX-10*renderSize/50,screenY+10*renderSize/50,screenX-5*renderSize/50,screenY+5*renderSize/50);
    	line(screenX-7*renderSize/50,screenY+7*renderSize/50,screenX-10*renderSize/50,screenY+4*renderSize/50);
	}
    
    // Draw player name and body damage above player
    LOGFONT oldFont;
    getfont(&oldFont);
    
    LOGFONT nameFont = oldFont;
    nameFont.lfHeight = 16;
    nameFont.lfWidth = 0;
    strcpy(nameFont.lfFaceName, "Arial");
    setfont(&nameFont);
    
    settextcolor(BLACK);
    setbkmode(TRANSPARENT);
    
    // Draw player name
    int nameWidth = textwidth(player.name);
    outtextxy(screenX - nameWidth/2, screenY - radius - 30, player.name);
    
    // Draw body damage
    char damageText[20];
    sprintf(damageText, "DMG: %.2f", player.bodyDamage);
    int damageWidth = textwidth(damageText);
    outtextxy(screenX - damageWidth/2, screenY - radius - 50, damageText);
    
    setfont(&oldFont);
} 
 
void drawFloatingTexts(int centerX, int centerY, int renderSize) {
    int ftIdx;
    for (ftIdx = 0; ftIdx < vec_size(floatingTexts); ) {
        FloatingText* it = vec_ptr(floatingTexts, ftIdx);
        int screenX = centerX + (int)((it->x - player.x) * renderSize);
        int screenY = centerY + (int)((it->y - player.y) * renderSize);
        
        LOGFONT oldFont;
        getfont(&oldFont);
        
        LOGFONT textFont = oldFont;
        textFont.lfHeight = 14;
        textFont.lfWidth = 0;
        strcpy(textFont.lfFaceName, "Arial");
        setfont(&textFont);
        
        char text[10];
        sprintf(text, "%d", it->value);
        
        settextcolor(it->color);
        setbkmode(TRANSPARENT);
        outtextxy(screenX - textwidth(text)/2, screenY, text);
        
        setfont(&oldFont);
        
        // Move text upward
        /* Ease out: quick off the mark, then drifting.  A constant rate
         * made the numbers look like they were on rails. */
        it->y -= 0.005 + 0.012 * ((double)it->life / 30.0);
        it->life--;
        
        if (it->life <= 0) {
            vec_erase(floatingTexts, ftIdx);
        } else {
            ftIdx++;
        }
    }
}
bool bosscmp(BOSSBAR a,BOSSBAR b){
	if(String_equals(a.rarity, "Hyper"))return true;
	if(String_equals(b.rarity, "Hyper"))return false;
	if(String_equals(a.rarity, "Eternal"))return true;
	if(String_equals(b.rarity, "Eternal"))return false;
	if(String_equals(a.rarity, "Super"))return true;
	if(String_equals(b.rarity, "Super"))return false;
	return false;
}
/* Equivalent of std::sort(boss.begin(), boss.end(), bosscmp) (insertion sort, few elements) */
static void sortBoss(Vec_BOSSBAR* v) {
    int i;
    for (i = 1; i < vec_size(*v); i++) {
        BOSSBAR key = vec_at(*v, i);
        int j = i - 1;
        while (j >= 0 && bosscmp(key, vec_at(*v, j))) {
            vec_at(*v, j + 1) = vec_at(*v, j);
            j--;
        }
        vec_at(*v, j + 1) = key;
    }
}

void moveMobs(int renderSize){
	int centerX = SCREEN_WIDTH / 2;
    int centerY = SCREEN_HEIGHT / 2;
    
    
    int viewRadius = max(15, SCREEN_WIDTH / renderSize + 2);
	int startChunkX = max(0, (int)(player.x) - viewRadius);
    int endChunkX = min(MAP_SIZE - 1, (int)(player.x) + viewRadius);
    int startChunkY = max(0, (int)(player.y) - viewRadius);
    int endChunkY = min(MAP_SIZE - 1, (int)(player.y) + viewRadius);
	moveMobsInView(startChunkX, endChunkX, startChunkY, endChunkY);
	
	if (player.canSummonSandstorm) {
        player.sandstormSummonTimer++;
        if (player.sandstormSummonTimer >= 60 && player.activeSandstorms < 4) { // at most 4 active summons
            /*double angle = rand() / (double)(RAND_MAX) * 2 * 3.14159;
            double distance = 1.0 + rand() / (double)(RAND_MAX) * 2.0;*/
            double summonX = player.x /*+ cos(angle) * distance*/;
            double summonY = player.y /*+ sin(angle) * distance*/;
            
            Mob summonMob = Mob_new(nextMobId++, SANDSTORM_SUMMON, summonX, summonY, 2, true, 0.0, 0.0); // rarity 2
            vec_push(mobs, summonMob);
            vec_push(worldMap[(int)(summonX)][(int)(summonY)].mobIds, summonMob.id);
            
            player.activeSandstorms++;
            player.sandstormSummonTimer = 0;
        }
    }
}
void renderGame(int renderSize) {
    int x;
    int i;
    int centerX = SCREEN_WIDTH / 2;
    int centerY = SCREEN_HEIGHT / 2;
    
    
    int viewRadius = max(15, SCREEN_WIDTH / renderSize + 2);
    
    int startChunkX = max(0, (int)(player.x) - viewRadius);
    int endChunkX = min(MAP_SIZE - 1, (int)(player.x) + viewRadius);
    int startChunkY = max(0, (int)(player.y) - viewRadius);
    int endChunkY = min(MAP_SIZE - 1, (int)(player.y) + viewRadius);
    
    double offsetX = (player.x - (int)(player.x)) * renderSize;
    double offsetY = (player.y - (int)(player.y)) * renderSize;
    
    /* viewRadius has a floor of 15, so at renderSize 72 the loop above walks
     * 31x31 = 961 chunks while only ~12x9 of them can reach the screen.
     * Culling here is what actually costs nothing to keep: a chunk is a
     * solid rect from (screenX, screenY) to +renderSize, so it can be
     * rejected before drawChunk is ever entered. */
    for (x = startChunkX; x <= endChunkX; x++) {
        int y;
        double rowX = centerX + (x - (int)(player.x)) * renderSize - offsetX;
        if (rowX + renderSize < 0 || rowX > SCREEN_WIDTH) continue;
        for (y = startChunkY; y <= endChunkY; y++) {
            double screenX = rowX;
            double screenY = centerY + (y - (int)(player.y)) * renderSize - offsetY;
            if (screenY + renderSize < 0 || screenY > SCREEN_HEIGHT) continue;

            drawChunk(screenX, screenY, x, y, renderSize);
        }
    }
    
    /* One pass over the mob list instead of three.
     *
     * It used to be a summon lifetime pass, a dead check pass and a draw
     * pass, each over the whole vector, with dead mobs removed by erase()
     * - which is a memmove of everything after it, so d deaths cost
     * O(d*n) bytes of copying.  With the "B" key spawning ten maps' worth
     * of mobs that dominated the frame.
     *
     * Here survivors are compacted towards the front in a single sweep,
     * O(n) however many die.  The flash timer that used to be ticked at
     * the end of drawMob() runs for every mob here, and boss bars are
     * collected from the wide box drawMob() used to test, so skipping the
     * draw for an off screen mob loses nothing. */
    {
        int w = 0;                 /* write cursor: survivors only */
        vec_clear(boss);
        for (i = 0; i < vec_size(mobs); i++) {
            Mob* mob = vec_ptr(mobs, i);
            bool keep = true;

            if (mob->type == SANDSTORM_SUMMON) {
                mob->summonLifetime--;
                if (mob->summonLifetime <= 0) {
                    vec_remove_int(&worldMap[mob->chunkX][mob->chunkY].mobIds, mob->id);
                    player.activeSandstorms--;
                    keep = false;
                }
            }
            if (keep && Mob_isDead(mob)) {
                vec_remove_int(&worldMap[mob->chunkX][mob->chunkY].mobIds, mob->id);
                keep = false;
            }

            if (mob->damageFlashTimer > 0) mob->damageFlashTimer--;

            if (keep) {
                if (!IsProjectile(mob->type) && mob->rarity >= 7) {
                    double mx = centerX + (mob->x - player.x) * renderSize;
                    double my = centerY + (mob->y - player.y) * renderSize;
                    if (mx >= -200 && mx < SCREEN_WIDTH * 2 &&
                        my >= -200 && my < SCREEN_HEIGHT * 2) {
                        BOSSBAR _bb;
                        _bb.hp     = mob->health    / mob->maxHealth;
                        _bb.ohp    = mob->oldHealth / mob->maxHealth;
                        _bb.type   = g_mobNames[mob->type];
                        _bb.rarity = (mob->type == SANDSTORM_SUMMON)
                                     ? g_rarityNames[0] : g_rarityNames[mob->rarity];
                        _bb.color  = rarityColors[mob->rarity];
                        vec_push(boss, _bb);
                    }
                }
                if (w != i) vec_at(mobs, w) = vec_at(mobs, i);
                w++;
            } else {
                free(mob->bodySegments.data);
            }
        }
        /* Everything past w is a dead mob that has already been freed.
         * The vector keeps its capacity, so the next spawn does not
         * realloc. */
        mobs.size = w;
    }
    
    for (i = 0; i < vec_size(mobs); i++) {
        Mob* mob = vec_ptr(mobs, i);
        int mobChunkX = (int)(mob->x);
        int mobChunkY = (int)(mob->y);
        
        if (mobChunkX >= startChunkX && mobChunkX <= endChunkX &&
            mobChunkY >= startChunkY && mobChunkY <= endChunkY) {
            /* Tight box: the radius the mob will actually be drawn at,
             * plus room for the two labels underneath it.  The old test
             * let everything within a doubled screen through, which is
             * roughly 3x more mobs than can possibly show. */
            double mx = centerX + (mob->x - player.x) * renderSize;
            double my = centerY + (mob->y - player.y) * renderSize;
            double pad = (double)mobDrawSize(mob, renderSize, 0) * 1.5 + 60.0;
            if (mx + pad < 0.0 || mx - pad > (double)SCREEN_WIDTH ||
                my + pad < 0.0 || my - pad > (double)SCREEN_HEIGHT) continue;
            drawMob(mob, centerX, centerY, renderSize, 0);
        }
    }
    
    sortBoss(&boss);
    LOGFONT oldFont;
    getfont(&oldFont);
    LOGFONT levelFont = oldFont;
    levelFont.lfHeight = 20;
    levelFont.lfWidth = 0;
    levelFont.lfWeight = 900;           /* FW_HEAVY -- bold bossbar labels */
    strcpy(levelFont.lfFaceName, "CONSOLAS");
    setfont(&levelFont);
    int l=vec_size(boss);
    for(i =0;i<min(4,l);i++){ 
    	int by = 80 + 50 * i;
    	/* No hudPanel(): the health bar already draws its own black backing
    	 * and the two labels carry a 1px outline, so the translucent slab
    	 * that used to sit behind the whole row is gone. */
    	drawHealthBar(SCREEN_WIDTH/4, by, 320, 20, vec_at(boss, i).hp, vec_at(boss, i).ohp, 1);
    	outlinedText(SCREEN_WIDTH/4+41, by + 15, vec_at(boss, i).color, vec_at(boss, i).rarity);
    	outlinedText(SCREEN_WIDTH/4+41, by - 5,  WHITE,                  vec_at(boss, i).type);
	}
	setfont(&oldFont);
    
    drawPlayer(centerX, centerY, renderSize);
    drawFloatingTexts(centerX, centerY, renderSize);
    
    // Draw player health bar
    int healthBarWidth = 200;
    int healthBarHeight = 20;
    int healthBarX = 10;
    int healthBarY = 10;
    
    double healthPercent = player.health / player.maxHealth;
    double oldhealthPercent = player.oldHealth / player.maxHealth;
    drawHealthBar(healthBarX, healthBarY, healthBarWidth, healthBarHeight, healthPercent,oldhealthPercent, 1);
    
    // Draw health text
    getfont(&oldFont);
    
    LOGFONT healthFont = oldFont;
    healthFont.lfHeight = 16;
    healthFont.lfWidth = 0;
    strcpy(healthFont.lfFaceName, "Arial");
    setfont(&healthFont);
    
    char healthText[50];
    sprintf(healthText, "HP: %.2f/%.2f", player.health, player.maxHealth);
    settextcolor(WHITE);
    setbkmode(TRANSPARENT);
    outtextxy(healthBarX + healthBarWidth + 10, healthBarY, healthText);
    
    // Draw experience
    char expText[50];
    sprintf(expText, "SSHP: %lld SSDMG: %lld SPEED: %.3f HEAL: %lld", (long long)player.summonSandstormHealth, (long long)player.summonSandstormDamage, player.baseSpeed,(long long)player.healp);
    outtextxy(healthBarX + healthBarWidth + 10, healthBarY + 25, expText);
    
    
    
    // Draw UI text
    hudPanel(0, SCREEN_HEIGHT - 66, SCREEN_WIDTH, SCREEN_HEIGHT);
    settextcolor(WHITE);
    setbkmode(TRANSPARENT);
    outtextxy(10, SCREEN_HEIGHT - 60, "WASD Move | T: Toggle View | C: Clear Mobs (V) and Summon (B: Crazy mode)");
    
    char info[100];
    sprintf(info, "Pos: (%.3f, %.3f)   FPS %.1f   K: Kill Yourself | L: Gallery | G: Mysterious Stick", player.x, player.y,g_fps);
    outtextxy(10, SCREEN_HEIGHT - 40, info);
    
    int chunkX = (int)(player.x);
    int chunkY = (int)(player.y);
    const char* typeNames[] = {"Air", "Soil", "Grass", "Sand", "Water", "Ice"};
    sprintf(info, "Terrain: %s", typeNames[worldMap[chunkX][chunkY].type]);
    outtextxy(10, SCREEN_HEIGHT - 20, info);
    
    sprintf(info, "View: %dx%d | Mobs: %d", renderSize, renderSize, vec_size(mobs));
    outtextxy(SCREEN_WIDTH - 200, 10, info);
    
    
    setfont(&oldFont);
}
void showInfo(){
    int i;
	double ox=player.x,oy=player.y;
	player.x=0;player.y=0;
	
	setfillcolor(baseColors[GRASS]);
	solidrectangle(0,0,600,420);
	setfillcolor(RGB(200,200,150));
	solidrectangle(0,420,600,770);
	setfillcolor(baseColors[WATER]);
	solidrectangle(0,770,600,880);
	
	int il[]={0,1,2,3,5,12,8,13,4,16,6,9,10,14,15,7,11};
	for(i =0;i<=ENDMOBTYPE;i++){
		int j;
		for(j =1;j<=10;j++){
			Mob mob = Mob_new(0, (MobType)il[i], j*50, i*50, j, false, 0.0, 0.0);
			drawMob(&mob, 0, 25, 5, 1);
			free(mob.bodySegments.data);   /* the C++ version frees this in the Mob destructor */
		}
	}
	FlushBatchDraw();
	player.x=ox;player.y=oy;
}
int main() {
    int cuIdx;
	
	SetUnhandledExceptionFilter(CrashExceptionFilter);
	Player_init(&player);          /* the C++ version does this in the constructor of the global Player player */
	
	fixhighdpi(); variablewinsize(1);
    hwnd=initgraph(SCREEN_WIDTH, SCREEN_HEIGHT);
    SetWindowText(hwnd,"Floram");
    initColors(); 
    setvsync(true);
    setaasamples(2);
    setfontmode(GLF_INT);
    setfiltermode(GL_LINEAR);
     
    
    // Save/Load variables
    time_t sta = time(0), end = time(0);
    FILE* fin = NULL;              /* stands in for the C++ ifstream */
    FILE* fout = NULL;             /* stands in for the C++ ofstream */
    bool fileExists = false;
    
    // Check if save file exists
    fin = fopen("floram.txt", "r");
    
    if (fin != NULL) {
        int x;
        int i;
        fileExists = true;
        // Load player data
        fscanf(fin, "%lf %lf %lf %lf %lf %lf",
               &player.x, &player.y, &player.vx, &player.vy, &player.radius, &player.baseSpeed);
        { int _boolTmp = 0;   /* a bool cannot be scanned with %d, so read an int and convert */
          fscanf(fin, "%lf %lf %lf %lf %d %lf %lf %d",
                 &player.health, &player.maxHealth, &player.bodyDamage, &player.healp,
                 &player.damageFlashTimer, &player.summonSandstormHealth, &player.summonSandstormDamage, &_boolTmp);
          player.canSummonSandstorm = (_boolTmp != 0); }
        
        // Load world map
        for (x = 0; x < MAP_SIZE; x++) {
            int y;
            for (y = 0; y < MAP_SIZE; y++) {
                int type;
                fscanf(fin, "%d", &type);
                worldMap[x][y].type = (BlockType)(type);
                // Regenerate chunk patterns (since masks are not saved)
                if (worldMap[x][y].type == SOIL) {
                    generateChunkPattern(x, y, true);
                } else if (worldMap[x][y].type == GRASS || worldMap[x][y].type == SAND || worldMap[x][y].type == ICE) {
                    generateChunkPattern(x, y, false);
                }
            }
        }
        
        // Load mob data
        int mobCount;
        fscanf(fin, "%d", &mobCount);
        mobs_clear();
        int maxId = 0;
        for (i = 0; i < mobCount; i++) {
            int id, type, rarity, chunkX, chunkY;
            double x, y, vx, vy, health, maxHealth, armor, bodyDamage;
            int moveTimer, moveDelay;
            double targetAngle, dir;
            int damageFlashTimer, lastDamageTaken;
            double Shellx,Shelly,Pearlx,Pearly;int ShellTimer;
            
            fscanf(fin, "%d %d %lf %lf %lf %lf %d %d %d",
                   &id, &type, &x, &y, &vx, &vy, &rarity, &chunkX, &chunkY);
            fscanf(fin, "%lf %lf %lf %lf", &health, &maxHealth, &armor, &bodyDamage);
            fscanf(fin, "%d %d %lf %lf", &moveTimer, &moveDelay, &targetAngle, &dir);
            fscanf(fin, "%d %d", &damageFlashTimer, &lastDamageTaken);
            fscanf(fin, "%lf %lf", &Shellx, &Shelly);
            fscanf(fin, "%lf %lf", &Pearlx, &Pearly);
            fscanf(fin, "%d", &ShellTimer);
            
            Mob mob = Mob_new(id, (MobType)(type), x, y, rarity, (Shellx != 0), Shelly, 0.0); /* keeps the argument positions produced by the original C++ default arguments */
            mob.vx = vx;
            mob.vy = vy;
            mob.chunkX = chunkX;
            mob.chunkY = chunkY;
            mob.health = health;
            mob.maxHealth = maxHealth;
            mob.armor = armor;
            mob.bodyDamage = bodyDamage;
            mob.moveTimer = moveTimer;
            mob.moveDelay = moveDelay;
            mob.targetAngle = targetAngle;
            mob.dir = dir;
            mob.damageFlashTimer = damageFlashTimer;
            mob.lastDamageTaken = lastDamageTaken;
            mob.shellx = Shellx; 
            mob.shelly = Shelly; 
            mob.pearlx = Pearlx; 
            mob.pearly = Pearly; 
            mob.shellTimer = ShellTimer;
            
            vec_push(mobs, mob);
            vec_push(worldMap[chunkX][chunkY].mobIds, id);
            
            if (id > maxId) maxId = id;
        }
        
        fclose(fin); fin = NULL;
        nextMobId = maxId + 1; // Next ID is max+1
    } else {
        // File doesn't exist, initialize new world
        srand(FIXED_SEED);
        initWorld();
    }
    
    bool paused = false;
    int renderSize = 72;
    
    
    BeginBatchDraw();
    LOGFONT oldFont;
    getfont(&oldFont);
    
    LOGFONT uiFont = oldFont;
    uiFont.lfHeight = getUIFontSize(renderSize);
    uiFont.lfWidth = 0;
    strcpy(uiFont.lfFaceName, "Arial");
    setfont(&uiFont);
    
    
    player.activeSandstorms=0;
    for (cuIdx = 0; cuIdx < vec_size(mobs); cuIdx++) { Mob cu = vec_at(mobs, cuIdx);
    	if(cu.type==SANDSTORM_SUMMON)player.activeSandstorms++;
	}
    double tit = 20;
    while (true) {
    	int crashCode = setjmp(g_exceptionJmp);
        if (crashCode != 0) {
            int ax;
            int ay;
	        srand(time(0)*112+time(0)*3);
            mobs_clear();
            nextMobId=0;
            for(ax = 0; ax < MAP_SIZE; ax++)for(ay = 0; ay < MAP_SIZE; ay++)
			    vec_clear(worldMap[ax][ay].mobIds);
            player.activeSandstorms=0;
            generateMobs();
             
            
            LOGFONT oldFont;
		    getfont(&oldFont);
		    LOGFONT levelFont = oldFont;
		    levelFont.lfHeight = 30;
		    levelFont.lfWidth = 0;
		    strcpy(levelFont.lfFaceName, "CONSOLAS");
		    setfont(&levelFont);
            cleardevice();
            settextcolor(RED);
            setbkmode(TRANSPARENT);
            char crashinfo[128];
            sprintf(crashinfo,"CrashExceptionFilter: Error (%d), switched map",crashCode);
            outtextxy(50, SCREEN_HEIGHT/2, crashinfo);
            setfont(&oldFont);
			FlushBatchDraw();
            
            Sleep(100);
            
            continue;
	    }
    	
		double dt;
		{
		    static LARGE_INTEGER s_prev, s_freq;
		    static bool s_init = false;
		    LARGE_INTEGER now;
		    if (!s_init) { QueryPerformanceFrequency(&s_freq);
		                   QueryPerformanceCounter(&s_prev); s_init = true; }
		    QueryPerformanceCounter(&now);
		    dt = (double)(now.QuadPart - s_prev.QuadPart) / (double)s_freq.QuadPart;
		    s_prev = now;
		    g_fps = (dt > 1e-9) ? (float)(1.0 / dt) : 0.0f;
		}
		tit+=dt*1000;
		g_waveT += dt;
		
		if(peekvariablemsg()){
			int xx=800,yy=600;
			getwinsize(&xx,&yy);
			SCREEN_WIDTH=xx;
			SCREEN_HEIGHT=yy;
		}
        
        if (gx_kbhit()) {
            char ch = gx_getch();
            if (ch == 27) break;
            if (ch == ' ') paused = !paused;
        }
        
        if (!paused && IsWindowActive()) {
            if (GetAsyncKeyState('W')) player.vy = -player.baseSpeed;
            else if (GetAsyncKeyState('S')) player.vy = player.baseSpeed;
            else player.vy /=1.5;
            
            if (GetAsyncKeyState('A')) player.vx = -player.baseSpeed;
            else if (GetAsyncKeyState('D')) player.vx = player.baseSpeed;
            else player.vx /=1.5;
            
            if (KDOWN('T') && !s_prevKey['T']) {
                s_prevKey['T'] = 1;
                renderSize = (renderSize == 72) ? 12 : 72;
            }
            if (KDOWN('R') && !s_prevKey['R']) {
                s_prevKey['R'] = 1;
                if (Player_isDead(&player)) {
                    Player_respawn(&player);
                }
            }
            if (KDOWN('C') && !s_prevKey['C']) {
                int ax;
                int ay;
                s_prevKey['C'] = 1;
                srand(time(0));
                mobs_clear();
                nextMobId=0;
                for(ax = 0; ax < MAP_SIZE; ax++)
			        for(ay = 0; ay < MAP_SIZE; ay++)
			            vec_clear(worldMap[ax][ay].mobIds);
                player.activeSandstorms=0;
                generateMobs();
            }
            if (KDOWN('B') && !s_prevKey['B']) {
                int ax;
                int ay;
                int i;
                s_prevKey['B'] = 1;
                srand(time(0));
                mobs_clear();
                nextMobId=0;
                for(ax = 0; ax < MAP_SIZE; ax++)
			        for(ay = 0; ay < MAP_SIZE; ay++)
			            vec_clear(worldMap[ax][ay].mobIds);
                player.activeSandstorms=0;
                for(i =1;i<=10;i++)generateMobs();
            }
            if (KDOWN('V') && !s_prevKey['V']) {
                int ax;
                int ay;
                s_prevKey['V'] = 1;
                mobs_clear();
                nextMobId=0;
                for(ax = 0; ax < MAP_SIZE; ax++)
			        for(ay = 0; ay < MAP_SIZE; ay++)
			            vec_clear(worldMap[ax][ay].mobIds);
                player.activeSandstorms=0;
            }
            if (KDOWN('K') && !s_prevKey['K']) {
                s_prevKey['K'] = 1;
                Player_respawn(&player);
            }
            if (GetAsyncKeyState('L')) {
                while(GetAsyncKeyState('L'));
                cleardevice();
				EndBatchDraw();
				closegraph();
				variablewinsize(0);
				hwnd=initgraph(600, 800);
				BeginBatchDraw();
				showInfo();
                while(!GetAsyncKeyState('L') || !IsWindowActive()){
                	FlushBatchDraw();Sleep(128);
				}
                while(GetAsyncKeyState('L'));
                EndBatchDraw();
                closegraph();
                variablewinsize(1);
                hwnd=initgraph(SCREEN_WIDTH, SCREEN_HEIGHT);
			    BeginBatchDraw();
            }
            if (KDOWN('G') && !s_prevKey['G']) {
                s_prevKey['G'] = 1;
                player.canSummonSandstorm=!player.canSummonSandstorm;
            }
            {
                int i;
                static const int watched[] = { 'T','R','C','B','V','K','G' };
                for (i = 0; i < 7; i++)
                    s_prevKey[watched[i]] = KDOWN(watched[i]) ? 1 : 0;
            }
        }
        /* Clamp the catch-up.  Without it a slow frame - a save, a
         * resize, a thousand mobs on screen - leaves tit large, so the
         * next frame runs several ticks, gets slower still, and the loop
         * feeds itself until the program stops responding.  Three ticks
         * is the most a frame will ever try to make up. */
        if (tit > 50.0) tit = 50.0;
        if (Player_isDead(&player)) {
            Sleep(500);
            Player_respawn(&player);
        }
        while(tit>=16.7){
			movePlayer();
	        moveMobs(renderSize);
	        Player_heal(&player);
	        tit-=16.7;
    	}
        
        cleardevice();
        
        renderGame(renderSize);
        
        // Auto-save every 10 seconds
        end = time(0);
        if (end - sta <=1 || end - sta >=9) {
        	LOGFONT oldFont;
		    getfont(&oldFont);
		    LOGFONT levelFont = oldFont;
		    levelFont.lfHeight = 15;
		    levelFont.lfWidth = 0;
		    strcpy(levelFont.lfFaceName, "CONSOLAS");
		    setfont(&levelFont);
        	setfillcolor(RGB(50,50,200));
        	solidrectangle(5,100,55,150);
        	setfillcolor(RGB(150,150,50));
        	solidrectangle(10,105,50,145);
        	settextcolor(WHITE);
        	outtextxy(12,115,"Saving");
        	setfont(&oldFont);
		}
        if (end - sta >= 10) {
            sta = end;
            fout = fopen("floram.txt", "w");
            if (fout) {
                int x;
                int saveIdx;
            
            // Save player data
            fprintf(fout, "%f %f %f %f %f %f\n",
                    player.x, player.y, player.vx, player.vy, player.radius, player.baseSpeed);
            fprintf(fout, "%f %f %f %f %d %f %f %d\n",
                    player.health, player.maxHealth, player.bodyDamage, player.healp,
                    player.damageFlashTimer, player.summonSandstormHealth,
                    player.summonSandstormDamage, (int)player.canSummonSandstorm);
            
            // Save world map
            /* 16384 fprintf() calls stalled the frame for tens of
             * milliseconds.  One pass into a buffer, one fwrite().
             * Same text, so loadGame() still reads it. */
            {
                size_t need = (size_t)MAP_SIZE * ((size_t)MAP_SIZE * 2 + 2) + 64;
                char* wb = (char*)malloc(need);
                if (wb) {
                    size_t p = 0;
                    for (x = 0; x < MAP_SIZE; x++) {
                        int y;
                        for (y = 0; y < MAP_SIZE; y++) {
                            wb[p++] = (char)('0' + (int)worldMap[x][y].type);
                            wb[p++] = (y == MAP_SIZE - 1) ? '\n' : ' ';
                        }
                    }
                    fwrite(wb, 1, p, fout);
                    free(wb);
                } else {
                    for (x = 0; x < MAP_SIZE; x++) {
                        int y;
                        for (y = 0; y < MAP_SIZE; y++)
                            fprintf(fout, "%d ", (int)worldMap[x][y].type);
                        fprintf(fout, "\n");
                    }
                }
            }
            
            // Save mob data
            fprintf(fout, "%d\n", vec_size(mobs));
            for (saveIdx = 0; saveIdx < vec_size(mobs); saveIdx++) {
                const Mob* mob = vec_ptr(mobs, saveIdx);
                fprintf(fout, "%d %d %f %f %f %f %d %d %d\n",
                        mob->id, (int)mob->type, mob->x, mob->y, mob->vx, mob->vy,
                        mob->rarity, mob->chunkX, mob->chunkY);
                fprintf(fout, "%f %f %f %f\n", mob->health, mob->maxHealth, mob->armor, mob->bodyDamage);
                fprintf(fout, "%d %d %f %f\n", mob->moveTimer, mob->moveDelay, mob->targetAngle, mob->dir);
                fprintf(fout, "%d %d\n", mob->damageFlashTimer, mob->lastDamageTaken);
                fprintf(fout, "%f %f\n", mob->shellx, mob->shelly);
                fprintf(fout, "%f %f\n", mob->pearlx, mob->pearly);
                fprintf(fout, "%d\n", mob->shellTimer);
            }
            fclose(fout); fout = NULL;
            }
    	}
    	
    	
    	FlushBatchDraw();
    	
    }
    
    EndBatchDraw();
    closegraph();
    return 0;
}




