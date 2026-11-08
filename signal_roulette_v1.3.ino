// =============================================================
//  SIGNAL ROULETTE  –  Firmware v1.3
//  Platform : ESP32 / Arduino Uno compatible
//  Display  : 16x2 LCD via I2C (address 0x27)
//  Shift Reg: 2x 74HC595 chained (16 bits total)
//             REG1 (first shifted out, QA-QH):
//               bits 0-4 = HP LEDs        (Q1-Q5)
//               bit  5   = CLEAN LED      (Q6)  lights on Clean reveal
//               bit  6   = NOISE LED      (Q7)  lights on Noise reveal
//               bit  7   = NULL  LED      (Q8)  lights on Null  reveal
//             REG2 (second shifted out):
//               bits 0-3 = Item LEDs (Scanner/Firewall/Recovery/Block)
//  Buzzer   : Pin 23 (dedicated buzzer, tone() driven)
//  Changes v1.3:
//    - Force reload when no Noise signals remain
//    - Cinematic batch-reload animation (scroll + per-signal reveal)
//    - "Show batch info" toggle setting (cfgShowBatch)
//    - 2-4 player support; target-select prompt for 3-4P
//    - Near-death LED pulse (1 HP = slow blink loop)
//    - Item grant on reload with 2.5 s per-player notification
//    - Revised signal odds: Noise 57%, Clean 36%, Null 7%
// =============================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ── VERSION ───────────────────────────────────────────────────
#define FW_VERSION "v1.3"

// ── PIN DEFINITIONS  (keep originals) ─────────────────────────
const int PIN_DATA  = 13;   // Shift register SER
const int PIN_LATCH = 12;   // Shift register RCLK
const int PIN_CLOCK = 14;   // Shift register SRCLK

const int BTN_FWD  = 4;     // Self / Menu down
const int BTN_BCK  = 5;     // Enemy / Menu up
const int BTN_CNF  = 15;    // Confirm / Select
const int BTN_ITEM = 18;    // Item scroll
const int BTN_PS   = 19;    // Pause / Back / No

// ── BUZZER ────────────────────────────────────────────────────
const int PIN_BUZZER = 23;  // Dedicated buzzer pin

// ── SIGNAL LED BIT MASKS (within REG1 HP byte) ────────────────
// Q6=bit5=Clean  Q7=bit6=Noise  Q8=bit7=Null
#define SIG_LED_CLEAN  0x20   // 0b00100000
#define SIG_LED_NOISE  0x40   // 0b01000000
#define SIG_LED_NULL   0x80   // 0b10000000

// ── LCD ───────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── TIMING & DEBOUNCE ─────────────────────────────────────────
const unsigned long DEBOUNCE_MS   = 40;
const unsigned long BLINK_MS      = 180;
const unsigned long BOT_STEP_MS   = 420;   // delay between bot "thinking" steps
const unsigned long TRANSITION_MS = 700;   // HP-bar wipe delay

// ── GAME LIMITS ───────────────────────────────────────────────
const int MAX_HP        = 5;
const int MIN_HP        = 1;
const int MAX_ROUNDS    = 5;
const int MIN_ROUNDS    = 1;
const int BATCH_SIZE    = 6;
const int MAX_ITEMS     = 4;   // item types: Scanner Firewall Recovery Block
const int MAX_PLAYERS   = 4;   // supports 1v1 up to 4-player

// ── ENUMS ─────────────────────────────────────────────────────
enum GameState {
  ST_SPLASH,
  // Main menu tree
  ST_MENU_MAIN,          // Sound / Start / Guide
  ST_GUIDE_LIST,         // Rules / Signals / Items
  ST_GUIDE_PAGE,         // Scrollable text page
  ST_CONFIRM_GENERIC,    // Yes / No overlay
  // Setup flow
  ST_SETUP_MODE,         // 2P vs SP
  ST_SETUP_ROUNDS,
  ST_SETUP_HP,
  ST_SETUP_TIMER,
  ST_SETUP_BOTDIFF,
  ST_SETUP_SHOWBATCH,
  ST_SETUP_CONFIRM,
  // Core game loop
  ST_ROUND_ANNOUNCE,     // "Round X of Y"
  ST_BATCH_ANNOUNCE,     // Show batch counts
  ST_TURN_TRANSITION,    // HP wipe animation + whose turn
  ST_ACTION,             // Choose Self or Enemy
  ST_ITEM_MENU,          // Scroll items
  ST_ITEM_USE_CONFIRM,   // "Use Scanner?" Y/N
  ST_RESOLVE_SIGNAL,     // Reveal + apply signal
  ST_TARGET_SELECT,      // 3-4P: pick which player to shoot
  ST_ROUND_END,          // Winner of round
  ST_GAME_OVER,          // Match winner
  ST_SUDDEN_DEATH_ANNOUNCE,
  // Bot sub-states
  ST_BOT_THINKING,
  // Pause overlay
  ST_PAUSE_MENU          // Resume / Quit
};

// ── SIGNAL TYPES ──────────────────────────────────────────────
enum Signal { SIG_NOISE = 0, SIG_CLEAN = 1, SIG_NULL = 2 };

// ── ITEM IDS ──────────────────────────────────────────────────
enum ItemID { ITEM_SCANNER = 0, ITEM_FIREWALL = 1, ITEM_RECOVERY = 2, ITEM_BLOCK = 3 };
const char* ITEM_NAMES[]   = { "Scanner", "Firewall", "Recovery", "Block" };
const char* ITEM_SHORT[]   = { "SCNR", "FWLL", "RCVR", "BLCK" };

// ── STATE MACHINE ─────────────────────────────────────────────
GameState state         = ST_SPLASH;
GameState prevState     = ST_SPLASH;   // for confirm / pause return
GameState postConfirm   = ST_SPLASH;   // where to go if confirmed YES

// ── SETTINGS (configured in setup menus) ──────────────────────
bool  soundOn      = true;
bool  cfgShowBatch = true;  // show remaining signal counts during play
int   gameMode     = 1;     // 1 = SP, 2-4 = human player count
int   cfgPlayers   = 2;     // total players (SP=2 means 1 human+bot)
int   cfgRounds    = 3;
int   cfgHP        = 3;
int   cfgTimer     = 0;     // 0 = off, else seconds
int   botDiff      = 1;     // 1 = Normal, 2 = Hard

// ── LIVE MATCH STATE ──────────────────────────────────────────
int   activePlayer  = 1;
int   numPlayers    = 2;           // actual players this match (1 bot counts as 1)
int   playerHP[5]   = {0,0,0,0,0}; // [0] unused, [1..4]
int   playerWins[5] = {0,0,0,0,0};
bool  playerAlive[5]= {false,false,false,false,false};
int   currentRound  = 1;
bool  suddenDeath   = false;

// Items: inventory[1..4][ItemID]
int   inventory[5][MAX_ITEMS];

// Firewalls active per player [1..4]
int   fwActive[5]   = {0,0,0,0,0};

// Reload item notification: which players got an item this reload
bool  reloadItemGiven[5] = {false,false,false,false,false};
// Target-select state
int   targetCursor  = 2;   // which player is being targeted (default opp of P1)

// Signal block pending
bool  blockPending = false;

// Extra turn (from Clean→Self)
bool  bonusTurn = false;

// Signal batch
Signal batch[BATCH_SIZE];
int    batchIndex = 0;
int    batchNoise, batchClean, batchNull;

// Last resolved signal (for display)
Signal lastSignal;
bool   signalScanned = false;

// Timer
unsigned long moveTimerStart = 0;
bool   timerRunning = false;

// ── MENU / CURSOR ─────────────────────────────────────────────
int menuCursor  = 0;   // general-purpose menu index
int itemCursor  = 0;   // item sub-menu (0-4, 4 = Back)
int guidePage   = 0;   // which guide page
int guideSection= 0;   // 0=Rules 1=Signals 2=Items
int pauseCursor = 0;   // 0=Resume 1=Quit

// Confirm overlay text lines
char confirmL1[17] = "";
char confirmL2[17] = "Yes[CNF] No[PS]";

// ── BUTTON EDGE DETECTION ─────────────────────────────────────
bool lastFwd  = HIGH, lastBck  = HIGH;
bool lastCnf  = HIGH, lastItm  = HIGH, lastPs = HIGH;

bool edgeFwd, edgeBck, edgeCnf, edgeItm, edgePs;

// ── DAMAGE BLINK STATE ─────────────────────────────────────────
bool   damageBlink     = false;
int    blinkPlayer     = 0;
int    blinkCount      = 0;
unsigned long blinkLast = 0;
bool   blinkOn         = false;

// ── BOT AI STATE ──────────────────────────────────────────────
int    botStep         = 0;
unsigned long botStepTime = 0;
bool   botTargetSelf   = false;
int    botItemToUse    = -1;   // -1 = none

// ── RANDOM SEED ───────────────────────────────────────────────
// Use unconnected analog pin for noise seed
#define RAND_PIN 34

// =============================================================
//   SHIFT REGISTER HELPERS
// =============================================================

// Push 16 bits: hpByte (REG1, bits 0-4 = HP LEDs)
//               itemByte (REG2, bits 0-3 = item LEDs)
void shiftWrite(byte hpByte, byte itemByte) {
  digitalWrite(PIN_LATCH, LOW);
  shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, itemByte);  // second SR
  shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, hpByte);    // first SR
  digitalWrite(PIN_LATCH, HIGH);
}

// Build HP byte from current value, optional override for blink
byte hpByte(int hp, bool ledOn = true) {
  if (!ledOn) return 0x00;
  byte b = 0;
  for (int i = 0; i < constrain(hp, 0, MAX_HP); i++) b |= (1 << i);
  return b;
}

// Build item byte from active player's inventory (only owned items lit)
byte itemByte(int player) {
  byte b = 0;
  for (int i = 0; i < MAX_ITEMS; i++) {
    if (inventory[player][i] > 0) b |= (1 << i);
  }
  return b;
}

// Show a specific player's HP + item LEDs
void showLEDs(int player) {
  int hp = playerHP[player];
  shiftWrite(hpByte(hp), itemByte(player));
}

// All LEDs off
void ledsOff() { shiftWrite(0x00, 0x00); }

// Light the correct signal-reveal LED (Q6/Q7/Q8) for 1.5 s then clear.
// HP LEDs for the active player stay lit during the reveal.
void showSignalLED(Signal sig, int player) {
  int hp = playerHP[player];
  byte hpB = hpByte(hp);
  byte itmB = itemByte(player);
  byte sigBit = 0;
  if (sig == SIG_CLEAN) sigBit = SIG_LED_CLEAN;
  else if (sig == SIG_NOISE) sigBit = SIG_LED_NOISE;
  else                       sigBit = SIG_LED_NULL;
  // Signal LED shares the REG1 byte with HP LEDs
  shiftWrite(hpB | sigBit, itmB);
  delay(1500);
  // Clear signal LED, restore normal HP+item display
  shiftWrite(hpB, itmB);
}

// =============================================================
//   DAMAGE BLINK ANIMATION  (non-blocking)
// =============================================================

void startDamageBlink(int player) {
  damageBlink = true;
  blinkPlayer = player;
  blinkCount  = 6;   // 3 on-off cycles
  blinkLast   = millis();
  blinkOn     = false;
}

void updateDamageBlink() {
  if (!damageBlink) return;
  if (millis() - blinkLast < BLINK_MS) return;
  blinkLast = millis();
  blinkOn = !blinkOn;
  int hp = playerHP[blinkPlayer];
  shiftWrite(hpByte(hp, blinkOn), itemByte(blinkPlayer));
  blinkCount--;
  if (blinkCount <= 0) {
    damageBlink = false;
    showLEDs(blinkPlayer);
  }
}

// ── Near-death pulse state (continuous slow blink at 1 HP) ────
bool   nearDeathPulse  = false;
int    nearDeathPlayer = 0;
unsigned long ndLast   = 0;
bool   ndOn            = true;
const unsigned long ND_BLINK_MS = 500;  // slow pulse

void startNearDeathPulse(int player) {
  nearDeathPulse  = true;
  nearDeathPlayer = player;
  ndLast = millis();
  ndOn   = true;
}

void stopNearDeathPulse() {
  nearDeathPulse = false;
  if (nearDeathPlayer > 0) showLEDs(nearDeathPlayer);
}

void updateNearDeathPulse() {
  if (!nearDeathPulse || damageBlink) return;
  if (millis() - ndLast < ND_BLINK_MS) return;
  ndLast = millis();
  ndOn = !ndOn;
  int hp = playerHP[nearDeathPlayer];
  shiftWrite(hpByte(hp, ndOn), itemByte(nearDeathPlayer));
}

// =============================================================
//   HP TRANSITION WIPE ANIMATION  (blocking, short)
//   Wipes LEDs down to 0 then back up to new player's HP
// =============================================================

void animateHPTransition(int fromHP, int toPlayer) {
  int toHP = playerHP[toPlayer];
  // Wipe down
  for (int i = fromHP; i >= 0; i--) {
    shiftWrite(hpByte(i), 0x00);
    delay(TRANSITION_MS / (fromHP + 1));
  }
  delay(120);
  // Fill up for new player
  for (int i = 0; i <= toHP; i++) {
    shiftWrite(hpByte(i), itemByte(toPlayer));
    delay(TRANSITION_MS / (toHP + 1));
  }
}

// =============================================================
//   LCD HELPERS  (16-char padded lines)
// =============================================================

void lcdLine(int row, const char* text) {
  lcd.setCursor(0, row);
  char buf[17];
  snprintf(buf, 17, "%-16s", text);
  lcd.print(buf);
}

void lcdLine(int row, String text) {
  lcdLine(row, text.c_str());
}

void lcdClear() { lcd.clear(); }

// Print two lines at once
void lcdShow(const char* l1, const char* l2) {
  lcdLine(0, l1);
  lcdLine(1, l2);
}

void lcdShow(String l1, String l2) {
  lcdShow(l1.c_str(), l2.c_str());
}

// Scroll a long message across line 1 (blocking)
void lcdScroll(const char* msg, int row, int delayMs) {
  int len = strlen(msg);
  if (len <= 16) { lcdLine(row, msg); return; }
  for (int i = 0; i <= len - 16; i++) {
    lcd.setCursor(0, row);
    for (int j = i; j < i + 16; j++) lcd.print(msg[j]);
    delay(delayMs);
  }
}

// =============================================================
//   BUTTON READING
// =============================================================

void readButtons() {
  bool curFwd = digitalRead(BTN_FWD);
  bool curBck = digitalRead(BTN_BCK);
  bool curCnf = digitalRead(BTN_CNF);
  bool curItm = digitalRead(BTN_ITEM);
  bool curPs  = digitalRead(BTN_PS);

  edgeFwd = (lastFwd == HIGH && curFwd == LOW);
  edgeBck = (lastBck == HIGH && curBck == LOW);
  edgeCnf = (lastCnf == HIGH && curCnf == LOW);
  edgeItm = (lastItm == HIGH && curItm == LOW);
  edgePs  = (lastPs  == HIGH && curPs  == LOW);

  lastFwd = curFwd;  lastBck = curBck;
  lastCnf = curCnf;  lastItm = curItm;
  lastPs  = curPs;

  if (edgeFwd || edgeBck || edgeCnf || edgeItm || edgePs) delay(DEBOUNCE_MS);
}

bool anyEdge() { return edgeFwd || edgeBck || edgeCnf || edgeItm || edgePs; }

// =============================================================
//   BUZZER  –  PIN 23, tone() driven
//   Each game event has a unique frequency/pattern signature.
// =============================================================

// Low-level: play one tone burst (blocking).
// noTone() is called after so the pin goes silent cleanly.
void beep(int freq, int dur) {
  if (!soundOn) return;
  tone(PIN_BUZZER, freq, dur);
  delay(dur + 5);
  noTone(PIN_BUZZER);
}

// Two-tone helper for jingles
void beep2(int f1, int d1, int f2, int d2) {
  beep(f1, d1);
  beep(f2, d2);
}

// ── Unique SFX signatures ────────────────────────────────────

// Menu scroll — soft click
void sfxMenu()    { beep(880, 35); }

// Confirm / select — rising pair
void sfxConfirm() { beep2(1046, 45, 1318, 55); }

// Noise hit — low descending growl
void sfxDamage() {
  if (!soundOn) return;
  tone(PIN_BUZZER, 220, 60); delay(65);
  tone(PIN_BUZZER, 180, 60); delay(65);
  tone(PIN_BUZZER, 140, 80); delay(85);
  noTone(PIN_BUZZER);
}

// Clean signal — bright ascending chime
void sfxClean() {
  if (!soundOn) return;
  tone(PIN_BUZZER, 1046, 60); delay(65);
  tone(PIN_BUZZER, 1318, 60); delay(65);
  tone(PIN_BUZZER, 1568, 90); delay(95);
  noTone(PIN_BUZZER);
}

// Null signal — flat mid blip
void sfxNull()    { beep(660, 70); }

// Item use — two quick mid beeps
void sfxItem()    { beep2(1000, 40, 1200, 50); }

// Firewall block — heavy thud + rising
void sfxFirewall() {
  if (!soundOn) return;
  tone(PIN_BUZZER, 150, 80); delay(85);
  tone(PIN_BUZZER, 800, 60); delay(65);
  noTone(PIN_BUZZER);
}

// Signal Block skips turn — punchy staccato
void sfxBlock() {
  if (!soundOn) return;
  tone(PIN_BUZZER, 500, 40); delay(45);
  tone(PIN_BUZZER, 500, 40); delay(45);
  tone(PIN_BUZZER, 300, 80); delay(85);
  noTone(PIN_BUZZER);
}

// Round win fanfare
void sfxRoundWin() {
  if (!soundOn) return;
  int notes[] = {784, 880, 988, 1174};
  int durs[]  = {80, 80, 80, 160};
  for (int i = 0; i < 4; i++) {
    tone(PIN_BUZZER, notes[i], durs[i]);
    delay(durs[i] + 10);
  }
  noTone(PIN_BUZZER);
}

// Game over — descending dirge
void sfxGameOver() {
  if (!soundOn) return;
  int notes[] = {523, 440, 392, 349};
  int durs[]  = {120, 120, 120, 300};
  for (int i = 0; i < 4; i++) {
    tone(PIN_BUZZER, notes[i], durs[i]);
    delay(durs[i] + 15);
  }
  noTone(PIN_BUZZER);
}

// Sudden death — frantic alarm (3 pulses)
void sfxSuddenDeath() {
  if (!soundOn) return;
  for (int i = 0; i < 3; i++) {
    tone(PIN_BUZZER, 1400, 100); delay(110);
    tone(PIN_BUZZER, 700,  100); delay(110);
  }
  noTone(PIN_BUZZER);
}

// Splash / boot chime
void sfxBoot() {
  if (!soundOn) return;
  int notes[] = {523, 659, 784, 1046};
  for (int i = 0; i < 4; i++) {
    tone(PIN_BUZZER, notes[i], 80);
    delay(90);
  }
  noTone(PIN_BUZZER);
}

// =============================================================
//   SIGNAL BATCH GENERATION
// =============================================================

// Check if remaining batch has no Noise signals — forces reload
bool batchHasNoise() {
  for (int i = batchIndex; i < BATCH_SIZE; i++)
    if (batch[i] == SIG_NOISE) return true;
  return false;
}

void generateBatch(bool sudden) {
  int noise, clean, nul;
  if (sudden) {
    // Sudden death: ~67-83% noise, 0-17% clean, 0-16% null
    noise = 4 + random(0, 2);   // 4 or 5
    clean = random(0, 2);       // 0 or 1
    nul   = BATCH_SIZE - noise - clean;
    if (nul < 0) { clean += nul; nul = 0; }
  } else {
    // Normal odds: Noise 57% (~3-4), Clean 36% (~2), Null 7% (0-1)
    // Over 6 slots: guarantee at least 3 noise, 2 clean, try 0-1 null
    noise = 3 + random(0, 2);   // 3 or 4  → 50-67%
    nul   = random(0, 2);       // 0 or 1  → 0-17%
    clean = BATCH_SIZE - noise - nul;
    if (clean < 0) { nul += clean; clean = 0; }
    if (nul < 0)   { nul = 0; }
    // Fallback guard
    if (noise + clean + nul != BATCH_SIZE)
      { noise = 3; clean = 2; nul = 1; }
  }
  batchNoise = noise; batchClean = clean; batchNull = nul;

  int idx = 0;
  for (int i = 0; i < noise; i++) batch[idx++] = SIG_NOISE;
  for (int i = 0; i < clean; i++) batch[idx++] = SIG_CLEAN;
  for (int i = 0; i < nul;   i++) batch[idx++] = SIG_NULL;

  // Fisher-Yates shuffle
  for (int i = BATCH_SIZE - 1; i > 0; i--) {
    int j = random(0, i + 1);
    Signal tmp = batch[i]; batch[i] = batch[j]; batch[j] = tmp;
  }
  batchIndex = 0;
}

// Remaining counts in current batch
void batchRemaining(int &n, int &c, int &u) {
  n = c = u = 0;
  for (int i = batchIndex; i < BATCH_SIZE; i++) {
    if (batch[i] == SIG_NOISE) n++;
    else if (batch[i] == SIG_CLEAN) c++;
    else u++;
  }
}

int batchLeft() { return BATCH_SIZE - batchIndex; }

// =============================================================
//   ITEM DISTRIBUTION
// =============================================================

void dealItems(int player, int count) {
  for (int i = 0; i < MAX_ITEMS; i++) inventory[player][i] = 0;
  for (int k = 0; k < count; k++) {
    int id = random(0, MAX_ITEMS);
    inventory[player][id]++;
  }
}

// =============================================================
//   GUIDE CONTENT  (pages stored in flash via PROGMEM-style arrays)
//   Each section is an array of {line1, line2} pairs.
//   Last entry is {"","END"} sentinel.
// =============================================================

// Rules pages (row 0 = top, row 1 = bottom)
const char* GUIDE_RULES[][2] = {
  { "RULES (1/5)",     "6 signals/batch"  },
  { "RULES (2/5)",     "Count revealed"   },
  { "RULES (3/5)",     "Order hidden"     },
  { "RULES (4/5)",     "Lose all HP=loss" },
  { "RULES (5/5)",     "Win rounds=win"   },
  { "", "END" }
};

const char* GUIDE_SIGS[][2] = {
  { "NOISE SIGNAL",    "-1 HP to target"  },
  { "CLEAN SIGNAL",    "Self=bonus turn"  },
  { "CLEAN (cont.)",   "Enemy=no effect"  },
  { "NULL SIGNAL",     "No effect at all" },
  { "", "END" }
};

const char* GUIDE_ITEMS[][2] = {
  { "SCANNER",         "Reveal signal"    },
  { "FIREWALL",        "Block next noise" },
  { "RECOVERY",        "+1 HP (if<max)"   },
  { "BLOCK",           "Skip enemy turn"  },
  { "", "END" }
};

// =============================================================
//   CONFIRM OVERLAY HELPER
// =============================================================

void requestConfirm(const char* msg, GameState returnOnYes) {
  prevState   = state;
  postConfirm = returnOnYes;
  strncpy(confirmL1, msg, 16);
  state = ST_CONFIRM_GENERIC;
  lcdShow(confirmL1, "[CNF]Yes [PS]No ");
}

// =============================================================
//   STATE TRANSITION HELPERS
// =============================================================

void enterMainMenu() {
  menuCursor = 0;
  state = ST_MENU_MAIN;
  drawMainMenu();
}

void enterGame() {
  numPlayers = cfgPlayers;
  currentRound = 1;
  suddenDeath  = false;
  blockPending = false;
  bonusTurn    = false;
  activePlayer = 1;
  signalScanned = false;
  stopNearDeathPulse();
  for (int p = 1; p <= MAX_PLAYERS; p++) {
    playerHP[p]    = (p <= numPlayers) ? cfgHP : 0;
    playerWins[p]  = 0;
    playerAlive[p] = (p <= numPlayers);
    fwActive[p]    = 0;
    reloadItemGiven[p] = false;
    dealItems(p, (p <= numPlayers) ? 2 : 0);
  }
  generateBatch(false);
  state = ST_ROUND_ANNOUNCE;
  drawRoundAnnounce();
}

// Helper: return next alive player after current
int nextAlivePlayer(int cur) {
  for (int i = 1; i <= numPlayers; i++) {
    int p = ((cur - 1 + i) % numPlayers) + 1;
    if (playerAlive[p]) return p;
  }
  return cur;
}

void enterTurnTransition() {
  state = ST_TURN_TRANSITION;
  // Wipe from some previous-ish HP (use min HP present for visual)
  int prevP = nextAlivePlayer(activePlayer); // any other
  if (prevP == activePlayer) prevP = 1;
  int prevHP = playerHP[prevP];
  lcdShow("  Switching...  ", "                ");
  animateHPTransition(prevHP, activePlayer);

  // If this player got a reload item, show 2.5 s notification
  if (reloadItemGiven[activePlayer]) {
    reloadItemGiven[activePlayer] = false;
    sfxItem();
    lcdShow("+NEW ITEM GRANT!", "Check your items");
    showLEDs(activePlayer);
    delay(2500);
  }

  drawTurnStart();
  delay(5000);  // 5 s: players read remaining batch counts

  // Near-death pulse setup for active player
  stopNearDeathPulse();
  if (playerHP[activePlayer] <= 1) startNearDeathPulse(activePlayer);

  // After transition, go to action or bot
  bool isBot = (gameMode == 1 && activePlayer == numPlayers);
  if (isBot) {
    state = ST_BOT_THINKING;
    botStep = 0;
    botStepTime = millis();
    drawBotThinking(0);
  } else {
    state = ST_ACTION;
    drawAction();
  }
}

void endTurn() {
  bool needReload = (batchIndex >= BATCH_SIZE) || (!batchHasNoise() && batchLeft() > 0);

  if (bonusTurn) {
    bonusTurn = false;
    signalScanned = false;
    if (needReload) {
      showBatchReload();
    } else {
      bool isBot = (gameMode == 1 && activePlayer == numPlayers);
      if (isBot) {
        state = ST_BOT_THINKING;
        botStep = 0; botStepTime = millis();
        drawBotThinking(0);
      } else {
        state = ST_ACTION;
        drawAction();
      }
    }
    return;
  }

  // Apply block if pending — skip next alive player
  if (blockPending) {
    blockPending = false;
    int skipped = nextAlivePlayer(activePlayer);
    char l1[17];
    snprintf(l1, 17, "BLOCK! P%d skips!", skipped);
    lcdShow(l1, "                ");
    sfxBlock();
    delay(800);
    // Advance past the skipped player
    activePlayer = nextAlivePlayer(skipped);
  } else {
    activePlayer = nextAlivePlayer(activePlayer);
  }

  signalScanned = false;

  if (needReload) {
    if (!batchHasNoise() && batchLeft() > 0) {
      lcdShow("No Noise left!  ", "Force reloading.");
      delay(700);
      batchIndex = BATCH_SIZE; // flush remaining
    }
    showBatchReload();
  } else {
    enterTurnTransition();
  }
}

// SFX: rising sweep for reload
void sfxReload() {
  if (!soundOn) return;
  int freqs[] = {400, 550, 700, 900, 1100, 1300};
  for (int i = 0; i < 6; i++) {
    tone(PIN_BUZZER, freqs[i], 55); delay(60);
  }
  noTone(PIN_BUZZER);
}

// Cinematic batch-reload animation + per-signal reveal + item grant
void showBatchReload() {
  generateBatch(suddenDeath);

  // ── Step 1: scrolling header ──────────────────────────────
  sfxReload();
  const char* scrollMsg = "Generating New Batch...";
  int msgLen = strlen(scrollMsg);
  lcdLine(0, "                ");
  // Animate "..." cycling on line 1 while scrolling top
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i <= max(0, msgLen - 16); i++) {
      lcd.setCursor(0, 0);
      for (int j = i; j < i + 16 && j < msgLen; j++) lcd.print(scrollMsg[j]);
      // Cycling dots on line 1
      char dots[17]; int d = ((i + pass*20) % 3) + 1;
      snprintf(dots, 17, "%-16s", d==1?".":(d==2?"..":(d==3?"...":".")));
      lcdLine(1, dots);
      delay(110);
    }
  }

  // ── Step 2: reveal each signal type one by one ───────────
  lcdShow("NEW BATCH READY!", "                ");
  delay(400);

  // Noise count
  char buf[17];
  snprintf(buf, 17, "Noise   : %d     ", batchNoise);
  lcdShow("Loading signals:", buf);
  beep(220, 80); delay(600);

  // Clean count
  snprintf(buf, 17, "Clean   : %d     ", batchClean);
  lcdShow("Loading signals:", buf);
  beep(1046, 80); delay(600);

  // Null count
  snprintf(buf, 17, "Null    : %d     ", batchNull);
  lcdShow("Loading signals:", buf);
  beep(660, 70); delay(600);

  // Final summary
  snprintf(buf, 17, "N:%d C:%d U:%d    ", batchNoise, batchClean, batchNull);
  lcdShow("Batch Loaded!   ", buf);
  beep2(900,60,1200,80);
  delay(1000);

  // ── Step 3: grant 1 item to each alive player ─────────────
  for (int p = 1; p <= numPlayers; p++) {
    reloadItemGiven[p] = false;
    if (!playerAlive[p]) continue;
    int newItem = random(0, MAX_ITEMS);
    inventory[p][newItem]++;
    reloadItemGiven[p] = true;
    // 2.5 s notification — shown at that player's turn start
    // (stored for display in enterTurnTransition)
  }

  enterTurnTransition();
}

// =============================================================
//   DRAW FUNCTIONS  (each state has a draw function)
// =============================================================

void drawMainMenu() {
  const char* opts[] = { "Sound", "Start Game", "Guide" };
  char l1[17], l2[17];
  snprintf(l1, 17, "MAIN MENU %s", FW_VERSION);
  snprintf(l2, 17, ">%-15s", opts[menuCursor]);
  if (menuCursor == 0) snprintf(l2, 17, ">Sound: %-9s", soundOn ? "ON" : "OFF");
  lcdShow(l1, l2);
}

void drawGuideList() {
  const char* sections[] = { "Rules", "Signals", "Items" };
  char l2[17];
  snprintf(l2, 17, ">%-15s", sections[menuCursor]);
  lcdShow("GUIDE           ", l2);
}

void drawGuidePage(int section, int page) {
  const char* (*pages)[2];
  if (section == 0) pages = GUIDE_RULES;
  else if (section == 1) pages = GUIDE_SIGS;
  else pages = GUIDE_ITEMS;
  lcdShow(pages[page][0], pages[page][1]);
}

bool guideAtEnd(int section, int page) {
  const char* (*pages)[2];
  if (section == 0) pages = GUIDE_RULES;
  else if (section == 1) pages = GUIDE_SIGS;
  else pages = GUIDE_ITEMS;
  return strcmp(pages[page + 1][1], "END") == 0;
}

void drawSetupMode() {
  // cursor: 0=SP, 1=2P, 2=3P, 3=4P
  const char* opts[] = {">SinglePlayer   ",">2 Player       ",">3 Player       ",">4 Player       "};
  lcdShow("SELECT MODE:    ", opts[menuCursor % 4]);
}

void drawSetupRounds() {
  char l2[17];
  snprintf(l2, 17, "Rounds: %d [1-%d] ", cfgRounds, MAX_ROUNDS);
  lcdShow("CONFIG ROUNDS   ", l2);
}

void drawSetupHP() {
  char l2[17];
  snprintf(l2, 17, "HP/Lives:%d [%d-%d]", cfgHP, MIN_HP, MAX_HP);
  lcdShow("CONFIG HP       ", l2);
}

void drawSetupTimer() {
  char l2[17];
  if (cfgTimer == 0) snprintf(l2, 17, "Timer: OFF      ");
  else snprintf(l2, 17, "Timer: %2ds      ", cfgTimer);
  lcdShow("MOVE TIMER      ", l2);
}

void drawSetupBotDiff() {
  lcdShow("BOT DIFFICULTY  ", botDiff == 1 ? ">Normal         " : ">Hard           ");
}

void drawSetupShowBatch() {
  lcdShow("SHOW BATCH INFO?", cfgShowBatch ? ">Yes (visible)  " : ">No  (hidden)   ");
}

void drawSetupConfirm() {
  char l1[17];
  const char* modeStr = (gameMode==1) ? (botDiff==1?"NBot":"HBot") : (cfgPlayers==3?"3P":(cfgPlayers==4?"4P":"2P"));
  snprintf(l1, 17, "%dR %dHP %s %s  ",
    cfgRounds, cfgHP,
    cfgTimer == 0 ? "NoTmr" : "Tmr",
    modeStr);
  lcdShow(l1, "Start? CNF/PS   ");
}

void drawRoundAnnounce() {
  char l1[17], l2[17];
  if (suddenDeath) {
    snprintf(l1, 17, "SUDDEN DEATH!   ");
    snprintf(l2, 17, "1HP High Noise  ");
  } else {
    snprintf(l1, 17, "ROUND %d OF %d    ", currentRound, cfgRounds);
    // Show wins for up to 4 players compactly
    if (numPlayers <= 2)
      snprintf(l2, 17, "P1:%d P2:%d Wins ", playerWins[1], playerWins[2]);
    else if (numPlayers == 3)
      snprintf(l2, 17, "P1:%d P2:%d P3:%d ", playerWins[1], playerWins[2], playerWins[3]);
    else
      snprintf(l2, 17, "1:%d 2:%d 3:%d 4:%d", playerWins[1], playerWins[2], playerWins[3], playerWins[4]);
  }
  lcdShow(l1, l2);
}

void drawBatchAnnounce() {
  char l1[17], l2[17];
  snprintf(l1, 17, "BATCH: %d signals", batchLeft());
  snprintf(l2, 17, "N:%d C:%d U:%d    ", batchNoise, batchClean, batchNull);
  lcdShow(l1, l2);
}

void drawTurnStart() {
  int hp = playerHP[activePlayer];
  bool isBot = (gameMode==1 && activePlayer==numPlayers);
  char pname[5]; snprintf(pname,5,isBot?"BOT":"P%d",activePlayer);
  char l1[17];
  snprintf(l1, 17, "%s TURN  HP:%-2d  ", pname, hp);
  char l2[17];
  if (cfgShowBatch) {
    int n,c,u; batchRemaining(n,c,u);
    snprintf(l2, 17, "N:%d C:%d U:%d L:%d", n, c, u, batchLeft());
  } else {
    snprintf(l2, 17, "Batch: hidden   ");
  }
  lcdShow(l1, l2);
}

void drawAction() {
  int hp = playerHP[activePlayer];
  char l1[17], l2[17];
  bool isBot = (gameMode==1 && activePlayer==numPlayers);
  snprintf(l1, 17, "%s HP:%d [ITM?]  ", isBot?"BOT":(char[]){'P',(char)('0'+activePlayer),0}, hp);
  // build name properly
  char pname[5]; snprintf(pname,5,isBot?"BOT":"P%d",activePlayer);
  snprintf(l1, 17, "%s HP:%-2d [ITM?] ", pname, hp);
  if (numPlayers > 2) snprintf(l2, 17, "FWD=Self BCK=Who");
  else                snprintf(l2, 17, "FWD=Self BCK=Foe");
  showLEDs(activePlayer);
  lcdShow(l1, l2);
  if (cfgTimer > 0) { moveTimerStart = millis(); timerRunning = true; }
}

void drawItemMenu() {
  showLEDs(activePlayer);
  // itemCursor 0-3 = items, 4 = Back
  char l1[17], l2[17];
  snprintf(l1, 17, "ITEMS [ITEM=next]");
  if (itemCursor < MAX_ITEMS) {
    int cnt = inventory[activePlayer][itemCursor];
    snprintf(l2, 17, ">%-8s x%d   ", ITEM_NAMES[itemCursor], cnt);
  } else {
    snprintf(l2, 17, ">(Back)         ");
  }
  lcdShow(l1, l2);
}

void drawItemUseConfirm() {
  char l1[17];
  snprintf(l1, 17, "Use %-12s", ITEM_NAMES[itemCursor]);
  lcdShow(l1, "[CNF]Yes [PS]No ");
}

void drawResolveSignal() {
  char l1[17];
  const char* sigName;
  if (lastSignal == SIG_NOISE) sigName = "NOISE";
  else if (lastSignal == SIG_CLEAN) sigName = "CLEAN";
  else sigName = "NULL";
  snprintf(l1, 17, "SIGNAL: %-8s", sigName);
  lcdShow(l1, "                ");
}

void drawRoundEnd(int winner) {
  char l1[17], l2[17];
  if (winner == 0) {
    snprintf(l1, 17, "DRAW ROUND!     ");
  } else {
    bool isBot = (gameMode==1 && winner==numPlayers);
    char wname[5]; snprintf(wname,5,isBot?"BOT":"P%d",winner);
    snprintf(l1, 17, "%s WINS ROUND!  ", wname);
  }
  if (numPlayers <= 2)
    snprintf(l2, 17, "P1:%d P2:%d      ", playerWins[1], playerWins[2]);
  else if (numPlayers == 3)
    snprintf(l2, 17, "P1:%d P2:%d P3:%d", playerWins[1], playerWins[2], playerWins[3]);
  else
    snprintf(l2, 17, "1:%d 2:%d 3:%d 4:%d", playerWins[1], playerWins[2], playerWins[3], playerWins[4]);
  lcdShow(l1, l2);
}

void drawGameOver() {
  int topWins = 0, winner = 0, tied = 0;
  for (int p = 1; p <= numPlayers; p++) if (playerWins[p] > topWins) { topWins = playerWins[p]; winner = p; }
  for (int p = 1; p <= numPlayers; p++) if (playerWins[p] == topWins) tied++;
  if (tied > 1) winner = 0;
  char l1[17];
  if (winner == 0) {
    snprintf(l1, 17, "DRAW! GG        ");
  } else {
    bool isBot = (gameMode==1 && winner==numPlayers);
    char wname[5]; snprintf(wname,5,isBot?"BOT":"P%d",winner);
    snprintf(l1, 17, "%s WINS! GG!    ", wname);
  }
  char l2[17];
  if (numPlayers <= 2)
    snprintf(l2, 17, "P1:%d P2:%d %s  ", playerWins[1], playerWins[2], FW_VERSION);
  else if (numPlayers == 3)
    snprintf(l2, 17, "1:%d 2:%d 3:%d %s", playerWins[1], playerWins[2], playerWins[3], FW_VERSION);
  else
    snprintf(l2, 17, "1:%d 2:%d 3:%d 4:%d", playerWins[1], playerWins[2], playerWins[3], playerWins[4]);
  lcdShow(l1, l2);
  sfxGameOver();
  ledsOff();
}

void drawPauseMenu() {
  lcdShow("PAUSED          ", pauseCursor == 0 ? ">Resume         " : ">Quit Match     ");
}

// Bot thinking display frames
void drawBotThinking(int frame) {
  const char* frames[] = {
    "BOT ANALYZING.. ",
    "BOT THINKING... ",
    "BOT DECIDING... ",
    "BOT TARGETING!  "
  };
  lcdLine(0, frames[frame % 4]);
  char l2[17];
  if (cfgShowBatch) {
    int n,c,u; batchRemaining(n,c,u);
    snprintf(l2, 17, "N:%d C:%d U:%d    ", n, c, u);
  } else {
    snprintf(l2, 17, "Processing...   ");
  }
  lcdLine(1, l2);
}

// =============================================================
//   RESOLVE SIGNAL LOGIC
// =============================================================

// applySignal — targetPlayer is the resolved target (1..numPlayers)
void applySignal(Signal sig, int targetPlayer) {
  bool targetSelf = (targetPlayer == activePlayer);
  lastSignal = sig;
  stopNearDeathPulse();

  drawResolveSignal();
  showSignalLED(sig, activePlayer);  // light Q6/Q7/Q8

  if (sig == SIG_NOISE) {
    sfxDamage();
    if (fwActive[targetPlayer] > 0) {
      fwActive[targetPlayer]--;
      lcdShow("FIREWALL BLOCKS!", "Noise absorbed  ");
      sfxFirewall();
      delay(1000);
    } else {
      playerHP[targetPlayer]--;
      startDamageBlink(targetPlayer);
      unsigned long blinkWait = millis();
      while (damageBlink && millis() - blinkWait < 2000) updateDamageBlink();
      char l1[17], l2[17];
      const char* tname = (targetPlayer == numPlayers && gameMode==1) ? "BOT" : (char[]){'P',(char)('0'+targetPlayer),0};
      // manual name build for multi-player
      char tnam[5]; snprintf(tnam,5,"P%d",targetPlayer);
      if (targetPlayer == numPlayers && gameMode == 1) snprintf(tnam,5,"BOT");
      snprintf(l1, 17, "NOISE! -%s HP   ", tnam);
      snprintf(l2, 17, "%s HP now %d    ", tnam, playerHP[targetPlayer]);
      lcdShow(l1, l2);
      delay(900);
      // Mark dead
      if (playerHP[targetPlayer] <= 0) playerAlive[targetPlayer] = false;
      // Near-death pulse if at 1 HP
      else if (playerHP[targetPlayer] <= 1) startNearDeathPulse(targetPlayer);
    }
    bonusTurn = false;
  } else if (sig == SIG_CLEAN) {
    sfxClean();
    if (targetSelf) {
      lcdShow("CLEAN! Bonus    ", "turn granted!   ");
      delay(900);
      bonusTurn = true;
    } else {
      lcdShow("CLEAN->Enemy    ", "No effect.      ");
      delay(900);
      bonusTurn = false;
    }
  } else {
    sfxNull();
    lcdShow("NULL SIGNAL     ", "No effect.      ");
    delay(800);
    bonusTurn = false;
  }
}

// Count how many players are still alive
int countAlive() {
  int n = 0;
  for (int p = 1; p <= numPlayers; p++) if (playerAlive[p]) n++;
  return n;
}

// Return the sole surviving player, or 0 if multiple/none
int soloSurvivor() {
  int last = 0, cnt = 0;
  for (int p = 1; p <= numPlayers; p++) { if (playerAlive[p]) { last = p; cnt++; } }
  return (cnt == 1) ? last : 0;
}

bool checkRoundOver() {
  return countAlive() <= 1;
}

void handleRoundOver() {
  stopNearDeathPulse();
  int winner = soloSurvivor(); // 0 = draw/all-dead

  if (winner > 0) playerWins[winner]++;

  drawRoundEnd(winner);
  showLEDs(winner > 0 ? winner : 1);
  if (winner > 0) sfxRoundWin(); else sfxNull();
  delay(1500);

  int needed = (cfgRounds / 2) + 1;
  // Check if any player has enough wins
  int matchWinner = 0;
  for (int p = 1; p <= numPlayers; p++) {
    if (playerWins[p] >= needed) { matchWinner = p; break; }
  }
  if (matchWinner > 0) {
    state = ST_GAME_OVER;
    drawGameOver();
    return;
  }

  if (currentRound >= cfgRounds) {
    // Check tie
    int topWins = 0;
    for (int p = 1; p <= numPlayers; p++) if (playerWins[p] > topWins) topWins = playerWins[p];
    int tiedCount = 0;
    for (int p = 1; p <= numPlayers; p++) if (playerWins[p] == topWins) tiedCount++;
    if (tiedCount > 1) {
      suddenDeath = true;
      cfgHP = 1;
      lcdShow("SUDDEN DEATH!   ", "Press CNF...    ");
      sfxSuddenDeath();
      state = ST_SUDDEN_DEATH_ANNOUNCE;
      return;
    }
    state = ST_GAME_OVER;
    drawGameOver();
    return;
  }

  currentRound++;
  blockPending = false; bonusTurn = false;
  signalScanned = false;
  stopNearDeathPulse();
  for (int p = 1; p <= numPlayers; p++) {
    playerHP[p]    = cfgHP;
    playerAlive[p] = true;
    fwActive[p]    = 0;
    reloadItemGiven[p] = false;
    dealItems(p, 2);
  }
  activePlayer = ((currentRound - 1) % numPlayers) + 1;

  if (batchIndex >= BATCH_SIZE || !batchHasNoise()) generateBatch(suddenDeath);

  state = ST_ROUND_ANNOUNCE;
  drawRoundAnnounce();
}

// =============================================================
//   ITEM USE LOGIC
// =============================================================

void useItem(int itemID) {
  if (inventory[activePlayer][itemID] <= 0) return;
  inventory[activePlayer][itemID]--;

  sfxItem();

  switch (itemID) {
    case ITEM_SCANNER: {
      signalScanned = true;
      Signal s = batch[batchIndex];
      const char* n = (s == SIG_NOISE) ? "NOISE" : (s == SIG_CLEAN) ? "CLEAN" : "NULL";
      char l2[17];
      snprintf(l2, 17, "Signal:%-10s", n);
      lcdShow("SCANNER RESULT  ", l2);
      delay(1500);
      break;
    }
    case ITEM_FIREWALL:
      fwActive[activePlayer]++;
      lcdShow("FIREWALL ARMED! ", "Next noise blkd ");
      sfxFirewall();
      delay(500);
      break;
    case ITEM_RECOVERY: {
      int &hp = playerHP[activePlayer];
      if (hp < cfgHP) {
        hp++;
        char l2[17];
        snprintf(l2, 17, "HP restored: %d  ", hp);
        lcdShow("RECOVERY SIGNAL ", l2);
      } else {
        lcdShow("RECOVERY SIGNAL ", "HP already full!");
      }
      showLEDs(activePlayer);
      delay(900);
      break;
    }
    case ITEM_BLOCK:
      blockPending = true;
      lcdShow("BLOCK ARMED!    ", "Enemy skip next ");
      sfxBlock();
      delay(300);
      break;
  }
  showLEDs(activePlayer);
}

// =============================================================
//   BOT AI
// =============================================================

void botDecide() {
  int n, c, u;
  batchRemaining(n, c, u);
  int total = batchLeft();
  float noisePct = total > 0 ? (float)n / total : 0.4f;

  // Decide item use
  botItemToUse = -1;
  int botHP = playerHP[numPlayers];
  int p1HPv = playerHP[1];
  if (botDiff == 2) {
    if (inventory[numPlayers][ITEM_SCANNER] > 0 && !signalScanned && random(0,10) < 7)
      botItemToUse = ITEM_SCANNER;
    else if (inventory[numPlayers][ITEM_FIREWALL] > 0 && botHP <= 2 && noisePct > 0.4 && random(0,10) < 8)
      botItemToUse = ITEM_FIREWALL;
    else if (inventory[numPlayers][ITEM_RECOVERY] > 0 && botHP < cfgHP && botHP <= 2 && random(0,10) < 7)
      botItemToUse = ITEM_RECOVERY;
    else if (inventory[numPlayers][ITEM_BLOCK] > 0 && p1HPv <= 2 && random(0,10) < 6)
      botItemToUse = ITEM_BLOCK;
  } else {
    if (random(0,10) < 3) {
      for (int i = 0; i < MAX_ITEMS; i++) {
        if (inventory[numPlayers][i] > 0) { botItemToUse = i; break; }
      }
    }
  }

  // Decide target
  if (signalScanned) {
    Signal s = batch[batchIndex];
    botTargetSelf = (s == SIG_CLEAN);
  } else if (botDiff == 2) {
    if (noisePct > 0.5f) botTargetSelf = false;
    else if (noisePct < 0.3f) botTargetSelf = true;
    else botTargetSelf = (botHP > p1HPv) ? false : (random(0,2) == 0);
  } else {
    botTargetSelf = (random(0,2) == 0);
  }
}

// Non-blocking bot turn runner — called every loop
void updateBotThinking() {
  if (state != ST_BOT_THINKING) return;
  if (millis() - botStepTime < BOT_STEP_MS) return;
  botStepTime = millis();
  botStep++;

  // Timer frantic display
  bool frantic = (cfgTimer > 0 && botStep > (cfgTimer * 1000 / BOT_STEP_MS) - 3);
  if (frantic) {
    drawBotThinking(botStep % 4);
    beep(600, 30);
  } else {
    drawBotThinking(botStep);
  }

  // After 3 steps, bot acts
  if (botStep >= 3) {
    botDecide();
    if (botItemToUse >= 0) {
      showLEDs(numPlayers);
      char l1[17];
      snprintf(l1, 17, "BOT uses %-7s", ITEM_NAMES[botItemToUse]);
      lcdShow(l1, "                ");
      delay(700);
      useItem(botItemToUse);
    }
    char l1[17], l2[17];
    int botTarg = botTargetSelf ? numPlayers : 1;
    snprintf(l1, 17, "BOT->%s  ", botTargetSelf ? "SELF   " : "PLAYER1");
    if (cfgShowBatch) {
      int n,c,u; batchRemaining(n,c,u);
      snprintf(l2, 17, "N:%d C:%d U:%d    ", n,c,u);
    } else { snprintf(l2,17,"Processing...   "); }
    lcdShow(l1, l2);
    delay(600);

    Signal sig = batch[batchIndex++];
    applySignal(sig, botTarg);
    showLEDs(activePlayer);

    if (checkRoundOver()) { handleRoundOver(); return; }
    endTurn();
  }
}

// =============================================================
//   TIMER LOGIC (non-blocking check in action state)
// =============================================================

void checkMoveTimer() {
  if (!timerRunning || cfgTimer == 0) return;
  unsigned long elapsed = (millis() - moveTimerStart) / 1000;
  if (elapsed >= (unsigned long)cfgTimer) {
    timerRunning = false;
    lcdShow("TIME'S UP!      ", "Random target!  ");
    delay(700);
    Signal sig = batch[batchIndex++];
    bool randSelf = random(0, 2) == 0;
    int randTarg = randSelf ? activePlayer : nextAlivePlayer(activePlayer);
    applySignal(sig, randTarg);
    showLEDs(activePlayer);
    if (checkRoundOver()) { handleRoundOver(); return; }
    endTurn();
  } else {
    // Update bottom-right of LCD with countdown (only every second)
    static unsigned long lastSec = 0;
    if (millis() - lastSec >= 1000) {
      lastSec = millis();
      int left = cfgTimer - (int)elapsed;
      char buf[5];
      snprintf(buf, 5, " %2d ", left);
      lcd.setCursor(12, 1);
      lcd.print(buf);
    }
  }
}

// =============================================================
//   SETUP
// =============================================================

void setup() {
  randomSeed(analogRead(RAND_PIN));

  pinMode(PIN_DATA,  OUTPUT);
  pinMode(PIN_LATCH, OUTPUT);
  pinMode(PIN_CLOCK, OUTPUT);

  pinMode(BTN_FWD,  INPUT_PULLUP);
  pinMode(BTN_BCK,  INPUT_PULLUP);
  pinMode(BTN_CNF,  INPUT_PULLUP);
  pinMode(BTN_ITEM, INPUT_PULLUP);
  pinMode(BTN_PS,   INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  ledsOff();

  lcd.init();
  lcd.backlight();

  lcdShow("SIGNAL ROULETTE ", FW_VERSION);
  sfxBoot();
  delay(1200);

  state = ST_MENU_MAIN;
  menuCursor = 0;
  drawMainMenu();
}

// =============================================================
//   MAIN LOOP
// =============================================================

void loop() {
  readButtons();
  updateDamageBlink();
  updateNearDeathPulse();
  updateBotThinking();
  checkMoveTimer();

  if (!anyEdge()) return;

  switch (state) {

    // ── MAIN MENU ───────────────────────────────────────────
    case ST_MENU_MAIN:
      if (edgeFwd) { menuCursor = (menuCursor + 1) % 3; sfxMenu(); drawMainMenu(); }
      if (edgeBck) { menuCursor = (menuCursor + 2) % 3; sfxMenu(); drawMainMenu(); }
      if (edgeCnf) {
        sfxConfirm();
        if (menuCursor == 0) {
          // Toggle sound inline
          soundOn = !soundOn;
          drawMainMenu();
        } else if (menuCursor == 1) {
          requestConfirm("Open settings?  ", ST_SETUP_MODE);
        } else {
          state = ST_GUIDE_LIST;
          menuCursor = 0;
          drawGuideList();
        }
      }
      break;

    // ── CONFIRM OVERLAY ─────────────────────────────────────
    case ST_CONFIRM_GENERIC:
      if (edgeCnf) {
        sfxConfirm();
        state = postConfirm;
        // Execute entry actions for dest state
        if (postConfirm == ST_SETUP_MODE) {
          menuCursor = 0;
          drawSetupMode();
        } else if (postConfirm == ST_GUIDE_LIST) {
          menuCursor = 0;
          drawGuideList();
        }
      }
      if (edgePs) {
        // User said no — return to previous
        state = prevState;
        if (state == ST_MENU_MAIN) drawMainMenu();
        else if (state == ST_SETUP_MODE) drawSetupMode();
        else if (state == ST_SETUP_CONFIRM) drawSetupConfirm();
        else drawMainMenu();
      }
      break;

    // ── GUIDE ────────────────────────────────────────────────
    case ST_GUIDE_LIST:
      if (edgeFwd) { menuCursor = (menuCursor + 1) % 3; sfxMenu(); drawGuideList(); }
      if (edgeBck) { menuCursor = (menuCursor + 2) % 3; sfxMenu(); drawGuideList(); }
      if (edgeCnf) {
        sfxConfirm();
        guideSection = menuCursor;
        guidePage = 0;
        state = ST_GUIDE_PAGE;
        drawGuidePage(guideSection, guidePage);
      }
      if (edgePs) { state = ST_MENU_MAIN; menuCursor = 2; drawMainMenu(); }
      break;

    case ST_GUIDE_PAGE:
      if (edgeFwd || edgeCnf) {
        if (!guideAtEnd(guideSection, guidePage)) {
          guidePage++;
          sfxMenu();
          drawGuidePage(guideSection, guidePage);
        } else {
          // End of section — back to guide list
          state = ST_GUIDE_LIST;
          menuCursor = guideSection;
          drawGuideList();
        }
      }
      if (edgePs) {
        state = ST_GUIDE_LIST;
        menuCursor = guideSection;
        sfxMenu();
        drawGuideList();
      }
      break;

    // ── SETUP FLOW ───────────────────────────────────────────
    case ST_SETUP_MODE:
      if (edgeFwd) { menuCursor = (menuCursor + 1) % 4; sfxMenu(); drawSetupMode(); }
      if (edgeBck) { menuCursor = (menuCursor + 3) % 4; sfxMenu(); drawSetupMode(); }
      if (edgeCnf) {
        sfxConfirm();
        if (menuCursor == 0) { gameMode = 1; cfgPlayers = 2; }  // SP = 1 human + 1 bot
        else { gameMode = menuCursor + 1; cfgPlayers = menuCursor + 1; }
        state = ST_SETUP_ROUNDS;
        drawSetupRounds();
      }
      if (edgePs) { state = ST_MENU_MAIN; menuCursor = 1; drawMainMenu(); }
      break;

    case ST_SETUP_ROUNDS:
      if (edgeFwd) { cfgRounds = min(cfgRounds + 1, MAX_ROUNDS); sfxMenu(); drawSetupRounds(); }
      if (edgeBck) { cfgRounds = max(cfgRounds - 1, MIN_ROUNDS); sfxMenu(); drawSetupRounds(); }
      if (edgeCnf) { sfxConfirm(); state = ST_SETUP_HP; drawSetupHP(); }
      if (edgePs)  { state = ST_SETUP_MODE; drawSetupMode(); }
      break;

    case ST_SETUP_HP:
      if (edgeFwd) { cfgHP = min(cfgHP + 1, MAX_HP); sfxMenu(); drawSetupHP(); }
      if (edgeBck) { cfgHP = max(cfgHP - 1, MIN_HP); sfxMenu(); drawSetupHP(); }
      if (edgeCnf) { sfxConfirm(); state = ST_SETUP_TIMER; drawSetupTimer(); }
      if (edgePs)  { state = ST_SETUP_ROUNDS; drawSetupRounds(); }
      break;

    case ST_SETUP_TIMER:
      if (edgeFwd) { cfgTimer = min(cfgTimer + 5, 30); sfxMenu(); drawSetupTimer(); }
      if (edgeBck) { cfgTimer = max(cfgTimer - 5, 0);  sfxMenu(); drawSetupTimer(); }
      if (edgeCnf) {
        sfxConfirm();
        if (gameMode == 1) { state = ST_SETUP_BOTDIFF; drawSetupBotDiff(); }
        else { state = ST_SETUP_SHOWBATCH; drawSetupShowBatch(); }
      }
      if (edgePs) { state = ST_SETUP_HP; drawSetupHP(); }
      break;

    case ST_SETUP_BOTDIFF:
      if (edgeFwd || edgeBck) { botDiff = (botDiff == 1) ? 2 : 1; sfxMenu(); drawSetupBotDiff(); }
      if (edgeCnf) { sfxConfirm(); state = ST_SETUP_SHOWBATCH; drawSetupShowBatch(); }
      if (edgePs)  { state = ST_SETUP_TIMER; drawSetupTimer(); }
      break;

    case ST_SETUP_SHOWBATCH:
      if (edgeFwd || edgeBck) { cfgShowBatch = !cfgShowBatch; sfxMenu(); drawSetupShowBatch(); }
      if (edgeCnf) { sfxConfirm(); state = ST_SETUP_CONFIRM; drawSetupConfirm(); }
      if (edgePs)  {
        if (gameMode==1) { state = ST_SETUP_BOTDIFF; drawSetupBotDiff(); }
        else { state = ST_SETUP_TIMER; drawSetupTimer(); }
      }
      break;

    case ST_SETUP_CONFIRM:
      if (edgeCnf) { sfxConfirm(); enterGame(); }
      if (edgePs)  { 
        if (gameMode == 1) { state = ST_SETUP_BOTDIFF; drawSetupBotDiff(); }
        else { state = ST_SETUP_TIMER; drawSetupTimer(); }
      }
      break;

    // ── ROUND / BATCH ANNOUNCE ───────────────────────────────
    case ST_ROUND_ANNOUNCE:
      if (edgeCnf || edgeFwd) {
        sfxConfirm();
        state = ST_BATCH_ANNOUNCE;
        drawBatchAnnounce();
      }
      break;

    case ST_BATCH_ANNOUNCE:
      if (edgeCnf || edgeFwd) {
        sfxConfirm();
        showLEDs(activePlayer);
        // First turn transition (no wipe needed on very start)
        drawTurnStart();
        delay(5000);  // 5 s: players read remaining batch counts
        if (activePlayer == 2 && gameMode == 1) {
          state = ST_BOT_THINKING;
          botStep = 0; botStepTime = millis();
          drawBotThinking(0);
        } else {
          state = ST_ACTION;
          drawAction();
        }
      }
      break;

    // ── MAIN ACTION ──────────────────────────────────────────
    case ST_ACTION:
      // FWD = target self
      if (edgeFwd) {
        timerRunning = false;
        sfxConfirm();
        Signal sig = batch[batchIndex++];
        applySignal(sig, activePlayer);
        showLEDs(activePlayer);
        if (checkRoundOver()) { handleRoundOver(); break; }
        endTurn();
      }
      // BCK = target enemy (or open target select for 3-4P)
      if (edgeBck) {
        timerRunning = false;
        sfxMenu();
        if (numPlayers > 2) {
          // find first non-self alive player as default target
          targetCursor = nextAlivePlayer(activePlayer);
          state = ST_TARGET_SELECT;
          // draw target select
          char l1[17]; snprintf(l1,17,"TARGET: P%d      ", targetCursor);
          lcdShow("SELECT TARGET:  ", l1);
        } else {
          sfxConfirm();
          int opp = nextAlivePlayer(activePlayer);
          Signal sig = batch[batchIndex++];
          applySignal(sig, opp);
          showLEDs(activePlayer);
          if (checkRoundOver()) { handleRoundOver(); break; }
          endTurn();
        }
      }
      // ITEM = open item menu
      if (edgeItm) {
        timerRunning = false;
        sfxMenu();
        itemCursor = 0;
        // Advance cursor to first owned item
        while (itemCursor < MAX_ITEMS && inventory[activePlayer][itemCursor] == 0) itemCursor++;
        state = ST_ITEM_MENU;
        drawItemMenu();
      }
      // PAUSE
      if (edgePs) {
        prevState = state;
        pauseCursor = 0;
        state = ST_PAUSE_MENU;
        drawPauseMenu();
      }
      break;

    // ── TARGET SELECT (3-4P) ─────────────────────────────────
    case ST_TARGET_SELECT: {
      // FWD/BCK scroll through alive players (excluding self)
      if (edgeFwd || edgeBck) {
        int dir = edgeFwd ? 1 : -1;
        int tries = numPlayers;
        do {
          targetCursor = ((targetCursor - 1 + dir + numPlayers) % numPlayers) + 1;
          tries--;
        } while ((targetCursor == activePlayer || !playerAlive[targetCursor]) && tries > 0);
        sfxMenu();
        char l1[17]; snprintf(l1,17,"TARGET: P%d HP:%d ", targetCursor, playerHP[targetCursor]);
        bool isBot2=(gameMode==1&&targetCursor==numPlayers);
        if(isBot2) snprintf(l1,17,"TARGET: BOT HP:%d",playerHP[targetCursor]);
        lcdShow("SELECT TARGET:  ", l1);
      }
      if (edgeCnf) {
        sfxConfirm();
        Signal sig = batch[batchIndex++];
        applySignal(sig, targetCursor);
        showLEDs(activePlayer);
        state = ST_ACTION; // reset for safety
        if (checkRoundOver()) { handleRoundOver(); break; }
        endTurn();
      }
      if (edgePs) { sfxMenu(); state = ST_ACTION; drawAction(); }
      break;
    }

    // ── ITEM MENU ────────────────────────────────────────────
    case ST_ITEM_MENU:
      if (edgeItm || edgeFwd) {
        // Scroll to next item that player owns, or Back
        do {
          itemCursor = (itemCursor + 1) % (MAX_ITEMS + 1);
        } while (itemCursor < MAX_ITEMS && inventory[activePlayer][itemCursor] == 0);
        sfxMenu();
        drawItemMenu();
      }
      if (edgeBck) {
        // Scroll backward
        do {
          itemCursor = (itemCursor + MAX_ITEMS) % (MAX_ITEMS + 1);
        } while (itemCursor < MAX_ITEMS && inventory[activePlayer][itemCursor] == 0);
        sfxMenu();
        drawItemMenu();
      }
      if (edgeCnf) {
        if (itemCursor == MAX_ITEMS) {
          // Back
          sfxMenu();
          state = ST_ACTION;
          drawAction();
          if (cfgTimer > 0) { moveTimerStart = millis(); timerRunning = true; }
        } else if (inventory[activePlayer][itemCursor] > 0) {
          sfxMenu();
          state = ST_ITEM_USE_CONFIRM;
          drawItemUseConfirm();
        }
      }
      if (edgePs) {
        sfxMenu();
        state = ST_ACTION;
        drawAction();
        if (cfgTimer > 0) { moveTimerStart = millis(); timerRunning = true; }
      }
      break;

    case ST_ITEM_USE_CONFIRM:
      if (edgeCnf) {
        sfxConfirm();
        useItem(itemCursor);
        state = ST_ITEM_MENU;
        itemCursor = 0;
        while (itemCursor < MAX_ITEMS && inventory[activePlayer][itemCursor] == 0) itemCursor++;
        drawItemMenu();
      }
      if (edgePs) {
        sfxMenu();
        state = ST_ITEM_MENU;
        drawItemMenu();
      }
      break;

    // ── ROUND / GAME OVER ────────────────────────────────────
    case ST_ROUND_END:
      if (edgeCnf) {
        sfxConfirm();
        state = ST_ROUND_ANNOUNCE;
        drawRoundAnnounce();
      }
      break;

    case ST_SUDDEN_DEATH_ANNOUNCE:
      if (edgeCnf) {
        sfxConfirm();
        generateBatch(true);
        state = ST_ROUND_ANNOUNCE;
        drawRoundAnnounce();
      }
      break;

    case ST_GAME_OVER:
      if (edgeCnf) {
        sfxConfirm();
        // Return to main menu
        state = ST_MENU_MAIN;
        menuCursor = 0;
        drawMainMenu();
      }
      break;

    // ── PAUSE MENU ───────────────────────────────────────────
    case ST_PAUSE_MENU:
      if (edgeFwd || edgeBck) { pauseCursor = 1 - pauseCursor; sfxMenu(); drawPauseMenu(); }
      if (edgeCnf) {
        sfxConfirm();
        if (pauseCursor == 0) {
          // Resume
          state = prevState;
          if (state == ST_ACTION) {
            drawAction();
            if (cfgTimer > 0) { moveTimerStart = millis(); timerRunning = true; }
          }
        } else {
          // Quit match
          state = ST_MENU_MAIN;
          menuCursor = 0;
          ledsOff();
          drawMainMenu();
        }
      }
      if (edgePs) {
        sfxMenu();
        state = prevState;
        if (state == ST_ACTION) {
          drawAction();
          if (cfgTimer > 0) { moveTimerStart = millis(); timerRunning = true; }
        }
      }
      break;

    default:
      break;
  }
}
