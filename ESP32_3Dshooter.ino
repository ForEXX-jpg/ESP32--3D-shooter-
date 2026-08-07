#include <WiFi.h>
#include <WebServer.h>

// ---------------- НАСТРОЙКИ ----------------

const char* AP_SSID     = "ESP32-Lab";
const char* AP_PASSWORD = "12345678";

const char* WEB_USER     = "admin";
const char* WEB_PASSWORD = "esp32pass";

#define BOOT_BTN 0
#define LED_PIN  2

WebServer server(80);
bool serverEnabled = true;
bool hasError = false;

unsigned long lastPress = 0;
const unsigned long DEBOUNCE_MS = 300;
unsigned long lastBlink = 0;
bool ledState = false;
const unsigned long BLINK_MS = 100;

// =====================================================================
//  MAZE RAIDER
// =====================================================================

#define MAPW 12
#define MAPH 12
#define MAX_ENEMIES 8
#define NUM_RAYS 100
#define ENEMY_MAX_HP 4.0f

const float FOV = PI / 3.0f;
const float MAX_DEPTH = 20.0f;
const float RAY_STEP = 0.05f;
const unsigned long TICK_MS = 80;

// старая монохромная карта: 0 = пусто, 1 = стена (один тип, без цветов)
const uint8_t MAPD[MAPH][MAPW] = {
  {1,1,1,1,1,1,1,1,1,1,1,1},
  {1,0,0,0,0,0,0,0,0,0,0,1},
  {1,0,1,1,0,1,1,1,0,1,0,1},
  {1,0,1,0,0,0,0,1,0,1,0,1},
  {1,0,1,0,1,1,0,1,0,0,0,1},
  {1,0,0,0,1,0,0,0,0,1,0,1},
  {1,0,1,1,1,0,1,1,0,1,0,1},
  {1,0,0,0,0,0,1,0,0,0,0,1},
  {1,1,1,0,1,0,1,0,1,1,0,1},
  {1,0,0,0,1,0,0,0,1,0,0,1},
  {1,0,1,1,1,1,1,1,1,0,0,1},
  {1,1,1,1,1,1,1,1,1,1,1,1}
};

struct Enemy { float x; float y; float hp; bool alive; };
Enemy enemies[MAX_ENEMIES];

struct WeaponDef {
  const char* name; float damage; unsigned long cooldownMs;
  int pellets; float spreadRad; float tolerance; int maxAmmo; float splashRadius;
};
WeaponDef WEAPONS[4] = {
  {"PISTOL",  1.0f, 350,  1, 0.0f,  0.15f, -1, 0.0f},
  {"SHOTGUN", 1.0f, 700,  5, 0.35f, 0.05f, 30, 0.0f},
  {"RIFLE",   2.0f, 180,  1, 0.0f,  0.07f, 90, 0.0f},
  {"GRENADE", 6.0f, 1500, 1, 0.0f,  0.0f,   5, 1.6f}
};
int currentWeapon = 0;
unsigned long lastFireMs = 0;
int ammo[4];

// сложность: 0 easy, 1 normal, 2 hard
int difficulty = 1;
float diffEnemySpeed[3]      = {0.035f, 0.05f, 0.075f};
float diffEnemyDamage[3]     = {1.0f, 1.5f, 2.2f};
unsigned long diffSpawnMs[3] = {6000, 4000, 2500};
int diffMaxEnemies[3]        = {4, 6, 8};

bool gamePaused = true;

float playerX, playerY, playerAngle;
float health;
int kills;
bool shooterOver;
bool keyLeft = false, keyRight = false, keyFwd = false, keyBack = false;

unsigned long lastSpawnMs = 0;

float wallDist[NUM_RAYS];
bool wallSide[NUM_RAYS];
float spriteDist[NUM_RAYS];
int8_t spriteEnemyIdx[NUM_RAYS];

unsigned long lastTick = 0;

unsigned long busyMicrosAccum = 0;
unsigned long loadWindowStart = 0;
int cpuLoadPercent = 0;
const unsigned long LOAD_WINDOW_MS = 500;

bool isWallCell(float x, float y) {
  int mx = (int)floor(x), my = (int)floor(y);
  if (mx < 0 || mx >= MAPW || my < 0 || my >= MAPH) return true;
  return MAPD[my][mx] != 0;
}

float normalizeAngle(float a) {
  while (a > PI) a -= 2 * PI;
  while (a < -PI) a += 2 * PI;
  return a;
}

float castRayDist(float angle) {
  float dist = 0;
  float cosA = cos(angle), sinA = sin(angle);
  while (dist < MAX_DEPTH) {
    dist += RAY_STEP;
    float x = playerX + cosA * dist;
    float y = playerY + sinA * dist;
    if (isWallCell(x, y)) return dist;
  }
  return MAX_DEPTH;
}

void computeFrame() {
  for (int i = 0; i < NUM_RAYS; i++) {
    float rayAngle = playerAngle - FOV / 2.0f + FOV * i / (float)NUM_RAYS;
    float cosA = cos(rayAngle), sinA = sin(rayAngle);
    float dist = 0;
    bool hitSide = false;

    while (dist < MAX_DEPTH) {
      dist += RAY_STEP;
      float x = playerX + cosA * dist;
      float y = playerY + sinA * dist;
      int mx = (int)floor(x), my = (int)floor(y);
      if (mx < 0 || mx >= MAPW || my < 0 || my >= MAPH) { hitSide = false; break; }
      if (MAPD[my][mx] != 0) {
        float fx = x - mx, fy = y - my;
        float dv = (fx < 1.0f - fx) ? fx : (1.0f - fx);
        float dh = (fy < 1.0f - fy) ? fy : (1.0f - fy);
        hitSide = (dv < dh);
        break;
      }
    }
    if (dist > MAX_DEPTH) dist = MAX_DEPTH;

    wallDist[i] = dist * cos(rayAngle - playerAngle);
    wallSide[i] = hitSide;
    spriteDist[i] = -1;
    spriteEnemyIdx[i] = -1;
  }

  for (int e = 0; e < MAX_ENEMIES; e++) {
    if (!enemies[e].alive) continue;
    float dx = enemies[e].x - playerX;
    float dy = enemies[e].y - playerY;
    float dist = sqrt(dx * dx + dy * dy);
    float angleTo = atan2(dy, dx);
    float relAngle = normalizeAngle(angleTo - playerAngle);
    if (fabs(relAngle) < FOV / 2.0f) {
      int col = (int)((relAngle + FOV / 2.0f) / FOV * NUM_RAYS);
      float perpDist = dist * cos(relAngle);
      if (col >= 0 && col < NUM_RAYS && perpDist < wallDist[col]) {
        spriteDist[col] = perpDist;
        spriteEnemyIdx[col] = e;
      }
    }
  }
}

bool findSpawnCell(float &sx, float &sy) {
  for (int tries = 0; tries < 30; tries++) {
    int mx = random(1, MAPW - 1);
    int my = random(1, MAPH - 1);
    if (MAPD[my][mx] != 0) continue;
    float cx = mx + 0.5f, cy = my + 0.5f;
    float dx = cx - playerX, dy = cy - playerY;
    float d = sqrt(dx * dx + dy * dy);
    if (d < 3.5f) continue;
    sx = cx; sy = cy;
    return true;
  }
  return false;
}

void trySpawnEnemy() {
  int aliveCount = 0, freeSlot = -1;
  for (int i = 0; i < MAX_ENEMIES; i++) {
    if (enemies[i].alive) aliveCount++;
    else if (freeSlot == -1) freeSlot = i;
  }
  if (aliveCount >= diffMaxEnemies[difficulty]) return;
  if (freeSlot == -1) return;
  unsigned long now = millis();
  if (now - lastSpawnMs < diffSpawnMs[difficulty]) return;
  lastSpawnMs = now;
  float sx, sy;
  if (findSpawnCell(sx, sy)) {
    enemies[freeSlot] = {sx, sy, ENEMY_MAX_HP, true};
  }
}

void resetShooterGame() {
  playerX = 1.5f; playerY = 1.5f; playerAngle = 0.0f;
  for (int i = 0; i < MAX_ENEMIES; i++) enemies[i].alive = false;
  enemies[0] = {9.5f, 1.5f, ENEMY_MAX_HP, true};
  enemies[1] = {1.5f, 9.5f, ENEMY_MAX_HP, true};
  enemies[2] = {9.5f, 9.5f, ENEMY_MAX_HP, true};
  health = 100; kills = 0; shooterOver = false;
  currentWeapon = 0; lastFireMs = 0;
  for (int i = 0; i < 4; i++) ammo[i] = WEAPONS[i].maxAmmo;
  keyLeft = keyRight = keyFwd = keyBack = false;
  lastSpawnMs = millis();
  computeFrame();
}

void tickShooterGame() {
  if (shooterOver || gamePaused) return;
  unsigned long tStart = micros();

  const float rotSpeed = 0.12f;
  const float moveSpeed = 0.15f;
  if (keyLeft) playerAngle -= rotSpeed;
  if (keyRight) playerAngle += rotSpeed;
  float mx = 0, my = 0;
  if (keyFwd)  { mx += cos(playerAngle) * moveSpeed; my += sin(playerAngle) * moveSpeed; }
  if (keyBack) { mx -= cos(playerAngle) * moveSpeed; my -= sin(playerAngle) * moveSpeed; }
  float nx = playerX + mx, ny = playerY + my;
  if (!isWallCell(nx, playerY)) playerX = nx;
  if (!isWallCell(playerX, ny)) playerY = ny;

  float espd = diffEnemySpeed[difficulty];
  float edmg = diffEnemyDamage[difficulty];
  for (int e = 0; e < MAX_ENEMIES; e++) {
    if (!enemies[e].alive) continue;
    float dx = playerX - enemies[e].x, dy = playerY - enemies[e].y;
    float dist = sqrt(dx * dx + dy * dy);
    if (dist > 0.5f) {
      float nex = enemies[e].x + dx / dist * espd;
      float ney = enemies[e].y + dy / dist * espd;
      if (!isWallCell(nex, enemies[e].y)) enemies[e].x = nex;
      if (!isWallCell(enemies[e].x, ney)) enemies[e].y = ney;
    }
    if (dist < 0.6f) {
      health -= edmg;
      if (health <= 0) { health = 0; shooterOver = true; }
    }
  }

  trySpawnEnemy();
  computeFrame();

  unsigned long tEnd = micros();
  busyMicrosAccum += (tEnd - tStart);
}

void doFire() {
  if (shooterOver || gamePaused) return;
  unsigned long now = millis();
  WeaponDef &w = WEAPONS[currentWeapon];
  if (now - lastFireMs < w.cooldownMs) return;
  if (w.maxAmmo >= 0 && ammo[currentWeapon] <= 0) return;
  lastFireMs = now;
  if (w.maxAmmo >= 0) ammo[currentWeapon]--;

  if (w.splashRadius > 0) {
    // граната: летит по направлению взгляда до стены или максимальной дальности
    float throwRange = 5.0f;
    float wallD = castRayDist(playerAngle);
    float landDist = (wallD < throwRange) ? wallD * 0.9f : throwRange;
    float lx = playerX + cos(playerAngle) * landDist;
    float ly = playerY + sin(playerAngle) * landDist;
    for (int e = 0; e < MAX_ENEMIES; e++) {
      if (!enemies[e].alive) continue;
      float dx = enemies[e].x - lx, dy = enemies[e].y - ly;
      float d = sqrt(dx * dx + dy * dy);
      if (d <= w.splashRadius) {
        enemies[e].hp -= w.damage;
        if (enemies[e].hp <= 0) { enemies[e].alive = false; kills++; }
      }
    }
  } else {
    for (int p = 0; p < w.pellets; p++) {
      float offset = 0;
      if (w.pellets > 1) offset = -w.spreadRad / 2.0f + w.spreadRad * p / (float)(w.pellets - 1);
      float fireAngle = playerAngle + offset;
      int bestIdx = -1; float bestDist = 1e9;
      for (int e = 0; e < MAX_ENEMIES; e++) {
        if (!enemies[e].alive) continue;
        float dx = enemies[e].x - playerX, dy = enemies[e].y - playerY;
        float dist = sqrt(dx * dx + dy * dy);
        float angleTo = atan2(dy, dx);
        float relAngle = fabs(normalizeAngle(angleTo - fireAngle));
        if (relAngle < w.tolerance && dist < MAX_DEPTH && dist < bestDist) {
          float wD = castRayDist(fireAngle);
          if (dist < wD) { bestIdx = e; bestDist = dist; }
        }
      }
      if (bestIdx >= 0) {
        enemies[bestIdx].hp -= w.damage;
        if (enemies[bestIdx].hp <= 0) {
          enemies[bestIdx].alive = false;
          kills++;
        }
      }
    }
  }
  computeFrame();
}

void switchWeaponCycle() { currentWeapon = (currentWeapon + 1) % 4; }

String buildShooterFrame() {
  String s; s.reserve(2200);
  s += String((int)round(health)); s += "|";
  s += String(kills); s += "|";
  s += (shooterOver ? "1" : "0"); s += "|";
  s += WEAPONS[currentWeapon].name; s += "|";
  s += String(currentWeapon); s += "|";
  s += (WEAPONS[currentWeapon].maxAmmo < 0 ? String(-1) : String(ammo[currentWeapon])); s += "|";
  s += String(cpuLoadPercent); s += "|";
  s += String(playerX, 2); s += ","; s += String(playerY, 2); s += ","; s += String(playerAngle, 2); s += "|";

  for (int i = 0; i < NUM_RAYS; i++) {
    s += String(wallDist[i], 2); s += ":";
    s += (wallSide[i] ? "1" : "0");
    s += ",";
  }
  s += "|";

  bool first = true;
  for (int i = 0; i < NUM_RAYS; i++) {
    if (spriteDist[i] >= 0) {
      if (!first) s += ";";
      s += String(i); s += ":"; s += String(spriteDist[i], 2); s += ":"; s += String(spriteEnemyIdx[i]);
      first = false;
    }
  }
  s += "|";

  bool firstE = true;
  for (int e = 0; e < MAX_ENEMIES; e++) {
    if (!enemies[e].alive) continue;
    if (!firstE) s += ";";
    s += String(enemies[e].x, 2); s += ","; s += String(enemies[e].y, 2); s += ","; s += String(enemies[e].hp, 1);
    firstE = false;
  }
  return s;
}

// ---------------- АВТОРИЗАЦИЯ ----------------

bool checkAuth() {
  if (!server.authenticate(WEB_USER, WEB_PASSWORD)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ---------------- HTML СТРАНИЦА ----------------

const char SHOOTER_page[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width, initial-scale=1, user-scalable=no, viewport-fit=cover'>
<title>Maze Raider</title>
<style>
*{box-sizing:border-box}
html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;background:#000;font-family:sans-serif;
-webkit-user-select:none;user-select:none}
#rotateHint{display:none;position:fixed;inset:0;background:#000;color:#fff;align-items:center;justify-content:center;
font-size:16px;text-align:center;padding:30px;z-index:9999;letter-spacing:1px}
@media (orientation: portrait){ #rotateHint{display:flex} #gameWrap{display:none} }
#gameWrap{position:relative;width:100vw;height:100vh;background:#000}
canvas{position:absolute;top:0;left:0}

#hudTopLeft{position:absolute;top:8px;left:10px;z-index:5;font-size:11px;color:#9fdcff;
text-shadow:0 0 4px #000;line-height:1.5;font-family:monospace}

#pauseBtn{position:absolute;top:8px;right:14px;z-index:15;padding:8px 14px;border-radius:8px;
background:rgba(40,40,50,0.6);border:1px solid rgba(255,255,255,0.25);color:#fff;font-size:11px;font-weight:bold}

#statusBar{position:absolute;left:0;right:0;bottom:0;height:64px;background:linear-gradient(180deg,#1a1a22,#0a0a0e);
border-top:2px solid #3a3a48;z-index:8;display:flex;align-items:center;padding:0 10px;gap:16px}
.sbFace{width:44px;height:44px;border-radius:6px;background:#111;border:1px solid #444;font-size:16px;
font-weight:bold;display:flex;align-items:center;justify-content:center;color:#fff;flex-shrink:0}
.sbGroup{display:flex;flex-direction:column;justify-content:center;min-width:60px}
.sbLabel{font-size:9px;color:#888;letter-spacing:1px}
.sbValue{font-size:15px;color:#fff;font-weight:bold}
.barBg{width:90px;height:8px;background:#000;border-radius:4px;overflow:hidden;margin-top:3px;border:1px solid #333}
.barFill{height:100%;border-radius:4px;transition:width 0.2s}

.dpad{position:absolute;left:14px;bottom:78px;width:160px;height:110px;z-index:10}
.dbtn{position:absolute;width:50px;height:50px;border-radius:10px;background:rgba(40,40,50,0.55);
border:1px solid rgba(255,255,255,0.25);color:#fff;font-size:14px;font-weight:bold;touch-action:none}
#bFwd{left:55px;top:0}
#bBack{left:55px;top:60px}
#bLeft{left:0;top:30px}
#bRight{left:110px;top:30px}

#switchBtn{position:absolute;right:20px;bottom:170px;width:70px;height:34px;border-radius:8px;
background:rgba(40,40,60,0.6);border:1px solid rgba(150,150,200,0.6);color:#fff;font-size:10px;font-weight:bold;
z-index:10;touch-action:none}
#fireBtn{position:absolute;right:16px;bottom:80px;width:78px;height:78px;border-radius:50%;
background:rgba(160,20,20,0.55);border:2px solid rgba(255,90,90,0.8);color:#fff;font-size:13px;font-weight:bold;
z-index:10;touch-action:none}

#restartBtn{display:none;position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);z-index:20;
padding:12px 26px;background:#2b8aef;color:#fff;border:none;border-radius:8px;font-size:15px}

#mainMenu, #pauseMenu, #settingsPanel{position:absolute;inset:0;background:rgba(5,5,10,0.94);z-index:50;
display:flex;flex-direction:column;align-items:center;justify-content:center;color:#fff;overflow-y:auto}
#pauseMenu, #settingsPanel{display:none}
#mainMenu h1, #pauseMenu h1, #settingsPanel h1{font-size:24px;letter-spacing:5px;margin-bottom:22px;
background:linear-gradient(90deg,#4fa8ff,#a64fff);-webkit-background-clip:text;background-clip:text;color:transparent}
.menuBtn{width:210px;padding:14px;margin:7px;border-radius:10px;border:1px solid rgba(255,255,255,0.15);
background:#1c1c26;color:#fff;font-size:14px;letter-spacing:1px}
.menuBtn.primary{background:linear-gradient(90deg,#2b8aef,#4fa8ff);border:none;font-weight:bold}
.settingRow{margin:10px 0;text-align:center}
.settingRow .label{font-size:11px;color:#999;letter-spacing:2px;margin-bottom:6px}
.optBtns{display:flex;gap:6px}
.optBtn{padding:8px 13px;border-radius:8px;border:1px solid rgba(255,255,255,0.2);background:#1c1c26;
color:#aaa;font-size:11px}
.optBtn.active{background:#2b8aef;color:#fff;border-color:#2b8aef}
</style></head><body>

<div id="rotateHint">ROTATE DEVICE<br>TO LANDSCAPE</div>

<div id="gameWrap">
  <canvas id="world"></canvas>
  <canvas id="overlay"></canvas>

  <div id="hudTopLeft">FPS: --<br>GPU: -- ms<br>ESP32: --%</div>
  <button id="pauseBtn">MENU</button>

  <div id="statusBar">
    <div class="sbFace" id="sbFace">:)</div>
    <div class="sbGroup">
      <span class="sbLabel">HEALTH</span>
      <span class="sbValue" id="sbHp">100</span>
      <div class="barBg"><div class="barFill" id="sbHpBar" style="width:100%;background:#4dff6a"></div></div>
    </div>
    <div class="sbGroup">
      <span class="sbLabel" id="sbWeaponLabel">PISTOL</span>
      <span class="sbValue" id="sbAmmo">INF</span>
    </div>
    <div class="sbGroup" style="margin-left:auto;align-items:flex-end">
      <span class="sbLabel">KILLS</span>
      <span class="sbValue" id="sbKills">0</span>
    </div>
  </div>

  <div class="dpad">
    <button class="dbtn" id="bFwd">FWD</button>
    <button class="dbtn" id="bBack">BACK</button>
    <button class="dbtn" id="bLeft">LFT</button>
    <button class="dbtn" id="bRight">RGT</button>
  </div>
  <button id="switchBtn">SWITCH</button>
  <button id="fireBtn">FIRE</button>
  <button id="restartBtn">RESTART</button>

  <div id="mainMenu">
    <h1>MAZE RAIDER</h1>
    <button class="menuBtn primary" id="menuStart">START GAME</button>
    <button class="menuBtn" id="menuSettings">SETTINGS</button>
  </div>

  <div id="pauseMenu">
    <h1 style="font-size:20px">PAUSED</h1>
    <button class="menuBtn primary" id="pauseResume">RESUME</button>
    <button class="menuBtn" id="pauseRestart">RESTART</button>
    <button class="menuBtn" id="pauseSettings">SETTINGS</button>
    <button class="menuBtn" id="pauseMainMenu">MAIN MENU</button>
  </div>

  <div id="settingsPanel">
    <h1 style="font-size:20px">SETTINGS</h1>
    <div class="settingRow">
      <div class="label">DIFFICULTY</div>
      <div class="optBtns" id="optDiff">
        <button class="optBtn" data-v="0">EASY</button>
        <button class="optBtn active" data-v="1">NORMAL</button>
        <button class="optBtn" data-v="2">HARD</button>
      </div>
    </div>
    <div class="settingRow">
      <div class="label">FPS LIMIT</div>
      <div class="optBtns" id="optFps">
        <button class="optBtn" data-v="15">15</button>
        <button class="optBtn" data-v="30">30</button>
        <button class="optBtn active" data-v="0">UNLIMITED</button>
      </div>
    </div>
    <div class="settingRow">
      <div class="label">GRAPHICS (GPU)</div>
      <div class="optBtns" id="optGfx">
        <button class="optBtn" data-v="low">LOW</button>
        <button class="optBtn active" data-v="high">HIGH</button>
      </div>
    </div>
    <div class="settingRow">
      <div class="label">TIME OF DAY</div>
      <div class="optBtns" id="optDayNight">
        <button class="optBtn active" data-v="day">DAY</button>
        <button class="optBtn" data-v="night">NIGHT</button>
      </div>
    </div>
    <button class="menuBtn primary" id="settingsBack" style="margin-top:18px">BACK</button>
  </div>
</div>

<script>
const world=document.getElementById('world');
const wctx=world.getContext('2d');
const overlay=document.getElementById('overlay');
const octx=overlay.getContext('2d');
const worldBuffer=document.createElement('canvas');
const bctx=worldBuffer.getContext('2d');

const MAPW_JS=12, MAPH_JS=12;
const MAP_JS=[
[1,1,1,1,1,1,1,1,1,1,1,1],
[1,0,0,0,0,0,0,0,0,0,0,1],
[1,0,1,1,0,1,1,1,0,1,0,1],
[1,0,1,0,0,0,0,1,0,1,0,1],
[1,0,1,0,1,1,0,1,0,0,0,1],
[1,0,0,0,1,0,0,0,0,1,0,1],
[1,0,1,1,1,0,1,1,0,1,0,1],
[1,0,0,0,0,0,1,0,0,0,0,1],
[1,1,1,0,1,0,1,0,1,1,0,1],
[1,0,0,0,1,0,0,0,1,0,0,1],
[1,0,1,1,1,1,1,1,1,0,0,1],
[1,1,1,1,1,1,1,1,1,1,1,1]
];
const ENEMY_MAX_HP=4;
const WEAPON_COOLDOWN={PISTOL:350,SHOTGUN:700,RIFLE:180,GRENADE:1500};

let keyState={left:false,right:false,fwd:false,back:false};
let lastPlayer={x:1.5,y:1.5,angle:0};
let lastEnemies=[];
let lastWeapon='PISTOL';
let recoilStart=0;
let fireLocked=false;

let gfxQuality='high';
let dayNight='day';
let targetFrameMs=0;
let lastFrameTime=0;

let lastFps=0, lastGpuMs=0, lastCpuLoad=0;
let frameCount=0, lastFpsTime=performance.now();

function resizeCanvases(){
  const w=window.innerWidth, h=window.innerHeight;
  world.width=w; world.height=h;
  overlay.width=w; overlay.height=h;
  computeBufferSize();
}
function computeBufferSize(){
  const w=world.width||window.innerWidth, h=world.height||window.innerHeight;
  if(gfxQuality==='low'){
    worldBuffer.width=Math.max(1,Math.floor(w/4));
    worldBuffer.height=Math.max(1,Math.floor(h/4));
  } else {
    worldBuffer.width=w;
    worldBuffer.height=h;
  }
}
window.addEventListener('resize',resizeCanvases);
window.addEventListener('orientationchange',()=>setTimeout(resizeCanvases,200));
resizeCanvases();

try{ if(screen.orientation && screen.orientation.lock){ screen.orientation.lock('landscape').catch(()=>{}); } }catch(e){}

function setKey(key,state){ fetch('/key?key='+key+'&state='+state).catch(()=>{}); }

function releaseAllKeys(){
  Object.keys(keyState).forEach(k=>{
    if(keyState[k]){ keyState[k]=false; setKey(k,'up'); }
  });
}
document.addEventListener('visibilitychange',()=>{ if(document.hidden) releaseAllKeys(); });
window.addEventListener('blur', releaseAllKeys);

function bindHold(id,key){
  const el=document.getElementById(id);
  const down=e=>{ e.preventDefault(); try{el.setPointerCapture(e.pointerId);}catch(err){} keyState[key]=true; setKey(key,'down'); };
  const up=e=>{ e.preventDefault(); keyState[key]=false; setKey(key,'up'); };
  el.addEventListener('pointerdown',down);
  el.addEventListener('pointerup',up);
  el.addEventListener('pointercancel',up);
  el.addEventListener('pointerout',up);
  el.addEventListener('pointerleave',up);
  el.addEventListener('contextmenu',e=>e.preventDefault());
}
bindHold('bLeft','left');
bindHold('bRight','right');
bindHold('bFwd','fwd');
bindHold('bBack','back');

setInterval(()=>{
  Object.keys(keyState).forEach(k=>{ setKey(k, keyState[k]?'down':'up'); });
},1000);

function doFireClient(){
  if(fireLocked)return;
  fetch('/fire').catch(()=>{});
  recoilStart=performance.now();
  const cd=WEAPON_COOLDOWN[lastWeapon]||300;
  fireLocked=true;
  const btn=document.getElementById('fireBtn');
  btn.style.opacity='0.4';
  setTimeout(()=>{ fireLocked=false; btn.style.opacity='1'; }, cd);
}
document.getElementById('fireBtn').addEventListener('pointerdown',e=>{ e.preventDefault(); doFireClient(); });
document.getElementById('switchBtn').addEventListener('pointerdown',e=>{ e.preventDefault(); fetch('/switchweapon').catch(()=>{}); });
document.getElementById('restartBtn').addEventListener('pointerdown',e=>{
  e.preventDefault();
  fetch('/restart').then(()=>{ document.getElementById('restartBtn').style.display='none'; });
});

document.addEventListener('keydown',e=>{
  if(e.key==='ArrowLeft' && !keyState.left){keyState.left=true;setKey('left','down');}
  else if(e.key==='ArrowRight' && !keyState.right){keyState.right=true;setKey('right','down');}
  else if(e.key==='ArrowUp' && !keyState.fwd){keyState.fwd=true;setKey('fwd','down');}
  else if(e.key==='ArrowDown' && !keyState.back){keyState.back=true;setKey('back','down');}
  else if(e.code==='Space'){ e.preventDefault(); doFireClient(); }
});
document.addEventListener('keyup',e=>{
  if(e.key==='ArrowLeft'){keyState.left=false;setKey('left','up');}
  else if(e.key==='ArrowRight'){keyState.right=false;setKey('right','up');}
  else if(e.key==='ArrowUp'){keyState.fwd=false;setKey('fwd','up');}
  else if(e.key==='ArrowDown'){keyState.back=false;setKey('back','up');}
});

function wallColor(dist,side,night){
  let shade=Math.max(20,255-dist*13);
  if(side) shade*=0.75;
  if(night) shade*=0.5;
  shade=Math.floor(shade);
  return 'rgb('+shade+','+shade+','+shade+')';
}

function drawEnemySprite(ctx,cx,topY,size,hpFrac,idx,night){
  const palette=[['#7a1010','#2a0505'],['#7a3d10','#2a1503'],['#4a1080','#180530'],['#0d4a6b','#031824']];
  const pal=palette[idx%palette.length];
  const glow=night?1.0:0.6;

  const grad=ctx.createRadialGradient(cx,topY+size*0.45,size*0.05,cx,topY+size*0.45,size*0.6);
  grad.addColorStop(0,pal[0]); grad.addColorStop(1,pal[1]);
  ctx.fillStyle=grad;
  ctx.beginPath();
  ctx.moveTo(cx-size*0.28, topY+size*0.9);
  ctx.lineTo(cx-size*0.32, topY+size*0.55);
  ctx.lineTo(cx-size*0.18, topY+size*0.35);
  ctx.lineTo(cx-size*0.22, topY+size*0.1);
  ctx.lineTo(cx-size*0.08, topY+size*0.28);
  ctx.lineTo(cx, topY);
  ctx.lineTo(cx+size*0.08, topY+size*0.28);
  ctx.lineTo(cx+size*0.22, topY+size*0.1);
  ctx.lineTo(cx+size*0.18, topY+size*0.35);
  ctx.lineTo(cx+size*0.32, topY+size*0.55);
  ctx.lineTo(cx+size*0.28, topY+size*0.9);
  ctx.closePath();
  ctx.fill();

  ctx.fillStyle='rgba(255,60,30,'+glow+')';
  ctx.shadowColor='#ff3300'; ctx.shadowBlur=night?10:4;
  ctx.beginPath(); ctx.arc(cx-size*0.1,topY+size*0.32,size*0.045,0,Math.PI*2); ctx.fill();
  ctx.beginPath(); ctx.arc(cx+size*0.1,topY+size*0.32,size*0.045,0,Math.PI*2); ctx.fill();
  ctx.shadowBlur=0;

  const barW=size*0.55, barH=Math.max(3,size*0.05);
  const barX=cx-barW/2, barY=topY-size*0.05;
  ctx.fillStyle='rgba(0,0,0,0.55)'; ctx.fillRect(barX,barY,barW,barH);
  ctx.fillStyle= hpFrac>0.5?'#4dff6a':(hpFrac>0.25?'#ffe14f':'#ff4d4d');
  ctx.fillRect(barX,barY,barW*Math.max(0,hpFrac),barH);
}

function drawWorld(walls, spriteStr){
  const bw=worldBuffer.width, bh=worldBuffer.height;
  const numRays=walls.length;
  if(numRays===0)return;
  const colW=bw/numRays;
  const night=(dayNight==='night');

  bctx.fillStyle= night?'#111':'#333';
  bctx.fillRect(0,0,bw,bh/2);
  bctx.fillStyle= night?'#0a0a0a':'#555';
  bctx.fillRect(0,bh/2,bw,bh/2);

  for(let i=0;i<numRays;i++){
    const wall=walls[i];
    const wallH=Math.min(bh, bh/Math.max(0.15,wall.dist));
    bctx.fillStyle=wallColor(wall.dist,wall.side,night);
    bctx.fillRect(i*colW,(bh-wallH)/2,colW+1,wallH);
  }

  if(spriteStr){
    spriteStr.split(';').forEach(pair=>{
      if(!pair)return;
      const [colStr,distStr,idxStr]=pair.split(':');
      const col=Number(colStr), dist=Number(distStr), idx=Number(idxStr);
      const size=Math.min(bh,bh/Math.max(0.2,dist)*0.7);
      const sx=col*colW;
      const info=lastEnemies[idx];
      const hpFrac=info?Math.max(0,info.hp/ENEMY_MAX_HP):1;
      drawEnemySprite(bctx,sx,(bh-size)/2,size,hpFrac,idx,night);
    });
  }

  wctx.clearRect(0,0,world.width,world.height);
  wctx.imageSmoothingEnabled=(gfxQuality==='high');
  wctx.filter=(gfxQuality==='high')?'blur(0.5px)':'none';
  wctx.drawImage(worldBuffer,0,0,bw,bh,0,0,world.width,world.height);
  wctx.filter='none';
}

function drawMinimap(w,h){
  const cell=5;
  const mmW=MAPW_JS*cell, mmH=MAPH_JS*cell;
  const ox=w-mmW-14, oy=50;
  octx.fillStyle='rgba(0,0,0,0.5)';
  octx.fillRect(ox-4,oy-4,mmW+8,mmH+8);
  for(let y=0;y<MAPH_JS;y++){
    for(let x=0;x<MAPW_JS;x++){
      if(MAP_JS[y][x]!==0){
        octx.fillStyle='#d8d8de';
        octx.fillRect(ox+x*cell,oy+y*cell,cell-1,cell-1);
      }
    }
  }
  lastEnemies.forEach(e=>{
    octx.fillStyle='#ff4444';
    octx.beginPath(); octx.arc(ox+e.x*cell,oy+e.y*cell,2.4,0,Math.PI*2); octx.fill();
  });
  octx.fillStyle='#4fd6ff';
  octx.beginPath(); octx.arc(ox+lastPlayer.x*cell,oy+lastPlayer.y*cell,2.8,0,Math.PI*2); octx.fill();
  octx.strokeStyle='#4fd6ff'; octx.lineWidth=1.5;
  octx.beginPath();
  octx.moveTo(ox+lastPlayer.x*cell,oy+lastPlayer.y*cell);
  octx.lineTo(ox+lastPlayer.x*cell+Math.cos(lastPlayer.angle)*7,oy+lastPlayer.y*cell+Math.sin(lastPlayer.angle)*7);
  octx.stroke();
}

function drawCrosshair(w,h){
  const cx=w/2, cy=h/2;
  octx.strokeStyle='rgba(255,255,255,0.85)'; octx.lineWidth=2;
  octx.beginPath();
  octx.moveTo(cx-8,cy); octx.lineTo(cx-3,cy);
  octx.moveTo(cx+3,cy); octx.lineTo(cx+8,cy);
  octx.moveTo(cx,cy-8); octx.lineTo(cx,cy-3);
  octx.moveTo(cx,cy+3); octx.lineTo(cx,cy+8);
  octx.stroke();
}

function drawGun(w,h){
  const now=performance.now();
  const moving=keyState.fwd||keyState.back||keyState.left||keyState.right;
  const bobAmp=moving?5:1.5;
  const bobSpeed=moving?170:900;
  const bobY=Math.sin(now/bobSpeed*6.28)*bobAmp;
  const bobX=Math.cos(now/(bobSpeed*2)*6.28)*bobAmp*0.5;

  const elapsed=now-recoilStart;
  const kick=elapsed<150?(1-elapsed/150):0;

  const cx=w*0.80+bobX;
  const baseY=h;

  octx.save();
  octx.translate(cx, baseY - kick*22 + bobY);
  octx.rotate(-0.18);

  if(lastWeapon==='SHOTGUN'){
    octx.fillStyle='#3a2410'; octx.fillRect(-26,-58,50,58);
    octx.fillStyle='#1a1a1a'; octx.fillRect(-8,-140,16,88);
    octx.fillStyle='#111'; octx.fillRect(-11,-145,22,10);
  } else if(lastWeapon==='RIFLE'){
    octx.fillStyle='#2a331f'; octx.fillRect(-19,-62,38,62);
    octx.fillStyle='#151515'; octx.fillRect(-6,-150,12,92);
    octx.fillStyle='#444'; octx.fillRect(-18,-108,36,10);
    octx.fillStyle='#1a1a1a'; octx.fillRect(-3,-166,6,20);
  } else if(lastWeapon==='GRENADE'){
    octx.fillStyle='#3d5a2f';
    octx.beginPath(); octx.ellipse(0,-70,20,26,0,0,Math.PI*2); octx.fill();
    octx.strokeStyle='#1a1a1a'; octx.lineWidth=2;
    octx.beginPath(); octx.moveTo(-14,-92); octx.lineTo(14,-92); octx.stroke();
    octx.fillStyle='#888'; octx.fillRect(-4,-100,8,10);
  } else {
    octx.fillStyle='#222'; octx.fillRect(-16,-64,32,64);
    octx.fillStyle='#111'; octx.fillRect(-8,-98,16,40);
    octx.fillStyle='#555'; octx.fillRect(-10,-103,20,8);
  }

  if(kick>0.05 && lastWeapon!=='GRENADE'){
    octx.fillStyle='rgba(255,220,120,'+kick+')';
    const flashY = lastWeapon==='RIFLE' ? -166 : (lastWeapon==='SHOTGUN' ? -145 : -103);
    octx.beginPath(); octx.arc(0,flashY,16*kick+8,0,Math.PI*2); octx.fill();
  }

  octx.restore();
}

function renderOverlay(now){
  if(targetFrameMs>0 && now-lastFrameTime<targetFrameMs){
    requestAnimationFrame(renderOverlay);
    return;
  }
  lastFrameTime=now;

  const w=overlay.width, h=overlay.height;
  octx.clearRect(0,0,w,h);

  drawMinimap(w,h);
  drawGun(w,h);
  drawCrosshair(w,h);

  frameCount++;
  if(now-lastFpsTime>500){
    lastFps=Math.round(frameCount*1000/(now-lastFpsTime));
    document.getElementById('hudTopLeft').innerHTML=
      'FPS: '+lastFps+'<br>GPU: '+lastGpuMs.toFixed(1)+' ms<br>ESP32: '+lastCpuLoad+'%';
    frameCount=0; lastFpsTime=now;
  }

  requestAnimationFrame(renderOverlay);
}
requestAnimationFrame(renderOverlay);

function poll(){
  fetch('/frame').then(r=>r.text()).then(txt=>{
    const parts=txt.split('|');
    const hp=Number(parts[0]);
    const kl=parts[1];
    const over=parts[2]==='1';
    const weapon=parts[3];
    const ammoRaw=Number(parts[5]);
    const cpuLoad=parts[6];
    const pInfo=parts[7].split(',').map(Number);
    const wallStr=parts[8]||'';
    const spriteStr=parts[9]||'';
    const enemyStr=parts[10]||'';

    lastPlayer={x:pInfo[0],y:pInfo[1],angle:pInfo[2]};
    lastWeapon=weapon;
    lastCpuLoad=cpuLoad;
    lastEnemies=enemyStr.split(';').filter(x=>x.length>0).map(e=>{
      const [ex,ey,eh]=e.split(',');
      return {x:Number(ex),y:Number(ey),hp:Number(eh)};
    });

    document.getElementById('sbHp').textContent=hp;
    document.getElementById('sbHpBar').style.width=Math.max(0,hp)+'%';
    document.getElementById('sbHpBar').style.background = hp>50?'#4dff6a':(hp>25?'#ffe14f':'#ff4d4d');
    document.getElementById('sbFace').textContent = hp>66?':)':(hp>33?':|':(hp>0?':(':'X_X'));
    document.getElementById('sbWeaponLabel').textContent=weapon;
    document.getElementById('sbAmmo').textContent = ammoRaw<0 ? 'INF' : ammoRaw;
    document.getElementById('sbKills').textContent=kl;

    const walls=wallStr.split(',').filter(x=>x.length>0).map(e=>{
      const [d,sd]=e.split(':');
      return {dist:Number(d),side:sd==='1'};
    });

    const gpuStart=performance.now();
    drawWorld(walls, spriteStr);
    lastGpuMs=performance.now()-gpuStart;

    if(over) document.getElementById('restartBtn').style.display='block';
  }).catch(()=>{});
}
setInterval(poll,100);
poll();

// ---------------- ГЛАВНОЕ МЕНЮ / ПАУЗА / НАСТРОЙКИ ----------------

const mainMenu=document.getElementById('mainMenu');
const pauseMenu=document.getElementById('pauseMenu');
const settingsPanel=document.getElementById('settingsPanel');
let settingsOrigin='main';

document.getElementById('menuStart').addEventListener('click',()=>{
  mainMenu.style.display='none';
  fetch('/restart').then(()=>fetch('/resume')).catch(()=>{});
});
document.getElementById('menuSettings').addEventListener('click',()=>{
  mainMenu.style.display='none';
  settingsPanel.style.display='flex';
  settingsOrigin='main';
});

document.getElementById('pauseBtn').addEventListener('click',()=>{
  fetch('/pause').catch(()=>{});
  pauseMenu.style.display='flex';
});
document.getElementById('pauseResume').addEventListener('click',()=>{
  pauseMenu.style.display='none';
  fetch('/resume').catch(()=>{});
});
document.getElementById('pauseRestart').addEventListener('click',()=>{
  pauseMenu.style.display='none';
  fetch('/restart').then(()=>fetch('/resume')).catch(()=>{});
});
document.getElementById('pauseSettings').addEventListener('click',()=>{
  pauseMenu.style.display='none';
  settingsPanel.style.display='flex';
  settingsOrigin='pause';
});
document.getElementById('pauseMainMenu').addEventListener('click',()=>{
  pauseMenu.style.display='none';
  mainMenu.style.display='flex';
});

document.getElementById('settingsBack').addEventListener('click',()=>{
  settingsPanel.style.display='none';
  if(settingsOrigin==='pause'){ pauseMenu.style.display='flex'; }
  else { mainMenu.style.display='flex'; }
});

function bindOptGroup(id,onSelect){
  const group=document.getElementById(id);
  group.querySelectorAll('.optBtn').forEach(btn=>{
    btn.addEventListener('click',()=>{
      group.querySelectorAll('.optBtn').forEach(b=>b.classList.remove('active'));
      btn.classList.add('active');
      onSelect(btn.dataset.v);
    });
  });
}
bindOptGroup('optDiff', v=>{ fetch('/difficulty?d='+v).catch(()=>{}); });
bindOptGroup('optFps', v=>{ targetFrameMs=(v==='0')?0:1000/Number(v); });
bindOptGroup('optGfx', v=>{ gfxQuality=v; computeBufferSize(); });
bindOptGroup('optDayNight', v=>{ dayNight=v; });
</script>
</body></html>
)=====";

// ---------------- HTTP ХЕНДЛЕРЫ ----------------

void handleShooterPage() {
  if (!checkAuth()) return;
  server.send_P(200, "text/html", SHOOTER_page);
}

void handleFrame() {
  if (!checkAuth()) return;
  server.send(200, "text/plain", buildShooterFrame());
}

void handleKey() {
  if (!checkAuth()) return;
  String key = server.arg("key");
  String state = server.arg("state");
  bool down = (state == "down");
  if (key == "left") keyLeft = down;
  else if (key == "right") keyRight = down;
  else if (key == "fwd") keyFwd = down;
  else if (key == "back") keyBack = down;
  server.send(200, "text/plain", "ok");
}

void handleFire() {
  if (!checkAuth()) return;
  doFire();
  server.send(200, "text/plain", "ok");
}

void handleSwitchWeapon() {
  if (!checkAuth()) return;
  switchWeaponCycle();
  server.send(200, "text/plain", "ok");
}

void handleRestart() {
  if (!checkAuth()) return;
  resetShooterGame();
  server.send(200, "text/plain", "ok");
}

void handleDifficulty() {
  if (!checkAuth()) return;
  int d = server.arg("d").toInt();
  if (d >= 0 && d <= 2) difficulty = d;
  server.send(200, "text/plain", "ok");
}

void handlePause() {
  if (!checkAuth()) return;
  gamePaused = true;
  server.send(200, "text/plain", "ok");
}

void handleResume() {
  if (!checkAuth()) return;
  gamePaused = false;
  server.send(200, "text/plain", "ok");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---------------- КНОПКА BOOT / СВЕТОДИОД ----------------

void checkButton() {
  unsigned long now = millis();
  if (digitalRead(BOOT_BTN) == LOW && now - lastPress > DEBOUNCE_MS) {
    lastPress = now;
    serverEnabled = !serverEnabled;
    if (serverEnabled) { server.begin(); Serial.println("Server: ON"); }
    else { server.stop(); Serial.println("Server: OFF"); }
  }
}

void updateLed() {
  if (hasError) {
    unsigned long now = millis();
    if (now - lastBlink > BLINK_MS) {
      lastBlink = now;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
  } else if (serverEnabled) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
  }
}

// ---------------- SETUP / LOOP ----------------

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  randomSeed(analogRead(34));

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
  if (!ok) {
    hasError = true;
    Serial.println("Error: could not start AP");
  } else {
    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());
  }

  resetShooterGame();
  loadWindowStart = millis();

  server.on("/", handleShooterPage);
  server.on("/frame", handleFrame);
  server.on("/key", handleKey);
  server.on("/fire", handleFire);
  server.on("/switchweapon", handleSwitchWeapon);
  server.on("/restart", handleRestart);
  server.on("/difficulty", handleDifficulty);
  server.on("/pause", handlePause);
  server.on("/resume", handleResume);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  checkButton();
  updateLed();

  if (serverEnabled && !hasError) {
    server.handleClient();
    unsigned long now = millis();
    if (now - lastTick > TICK_MS) {
      lastTick = now;
      tickShooterGame();
    }
    if (now - loadWindowStart >= LOAD_WINDOW_MS) {
      float pct = (float)busyMicrosAccum / ((float)LOAD_WINDOW_MS * 1000.0f) * 100.0f;
      if (pct > 100) pct = 100;
      cpuLoadPercent = (int)pct;
      busyMicrosAccum = 0;
      loadWindowStart = now;
    }
  }
}