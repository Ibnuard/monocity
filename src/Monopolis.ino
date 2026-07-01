#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include "CardData.h"

#define SS_PIN D4
#define RST_PIN D8
#define BUZZER_PIN D0
#define BTN_START D3

#define MAX_PLAYERS 4
#define SALDO_AWAL 1500
#define ACTION_TIMEOUT 15000

MFRC522 mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

struct PlayerState {
  int cardIndex;
  int saldo;
  bool active;
};

struct PropertyState {
  int owner;
  byte level;
  bool mortgaged;
};

enum GameStage {
  STAGE_WAIT_START,
  STAGE_REGISTER,
  STAGE_IDLE,
  STAGE_WAIT_PLAYER,
  STAGE_WAIT_MORTGAGE_PROPERTY,
  STAGE_WAIT_MORTGAGE_PLAYER
};

PlayerState players[MAX_PLAYERS];
PropertyState properties[CARD_COUNT];

GameStage stage = STAGE_WAIT_START;
int playerCount = 0;
int pendingCard = -1;
int pendingProperty = -1;
unsigned long pendingStartedAt = 0;
unsigned long lastButtonAt = 0;
unsigned long lastScrollAt = 0;
int scrollPos = 0;
String idleText = "";

void beepOk() {
  tone(BUZZER_PIN, 2500);
  delay(90);
  tone(BUZZER_PIN, 3200);
  delay(100);
  noTone(BUZZER_PIN);
}

void beepError() {
  tone(BUZZER_PIN, 700);
  delay(300);
  noTone(BUZZER_PIN);
}

void printLine(byte row, String text) {
  lcd.setCursor(0, row);
  if (text.length() > 16) {
    text = text.substring(0, 16);
  }
  lcd.print(text);
  for (int i = text.length(); i < 16; i++) {
    lcd.print(" ");
  }
}

void showMessage(String top, String bottom, bool okSound = false, int waitMs = 0) {
  lcd.clear();
  printLine(0, top);
  printLine(1, bottom);
  if (okSound) {
    beepOk();
  }
  if (waitMs > 0) {
    delay(waitMs);
  }
}

String getUID() {
  String uidStr = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) {
      uidStr += "0";
    }
    uidStr += String(mfrc522.uid.uidByte[i], HEX);
  }
  uidStr.toUpperCase();
  return uidStr;
}

bool buttonPressed() {
  if (digitalRead(BTN_START) == LOW && millis() - lastButtonAt > 400) {
    lastButtonAt = millis();
    return true;
  }
  return false;
}

int findCardByUid(const String& uid) {
  for (int i = 0; i < CARD_COUNT; i++) {
    if (uid == CARD_DEFS[i].uid) {
      return i;
    }
  }
  return -1;
}

int findPlayerByCard(int cardIndex) {
  for (int i = 0; i < playerCount; i++) {
    if (players[i].active && players[i].cardIndex == cardIndex) {
      return i;
    }
  }
  return -1;
}

void resetPending() {
  pendingCard = -1;
  pendingProperty = -1;
  pendingStartedAt = 0;
}

void updateIdleText() {
  idleText = "";
  for (int i = 0; i < playerCount; i++) {
    idleText += "P";
    idleText += String(i + 1);
    idleText += ":";
    idleText += String(players[i].saldo);
    idleText += " ";
  }
}

void showIdle() {
  if (millis() - lastScrollAt < 350) {
    return;
  }

  lastScrollAt = millis();
  if (idleText.length() <= 16) {
    printLine(0, idleText);
  } else {
    String text = idleText + "   ";
    if (scrollPos >= text.length()) {
      scrollPos = 0;
    }
    String shown = text.substring(scrollPos);
    shown += text;
    printLine(0, shown.substring(0, 16));
    scrollPos++;
  }
  printLine(1, "Scan kartu...");
}

void goIdle() {
  resetPending();
  stage = STAGE_IDLE;
  updateIdleText();
  scrollPos = 0;
  lcd.clear();
  showIdle();
}

void startPending(GameStage nextStage, int cardIndex, String top, String bottom) {
  pendingCard = cardIndex;
  pendingStartedAt = millis();
  stage = nextStage;
  showMessage(top, bottom, true, 0);
}

void checkPendingTimeout() {
  if ((stage == STAGE_WAIT_PLAYER ||
       stage == STAGE_WAIT_MORTGAGE_PROPERTY ||
       stage == STAGE_WAIT_MORTGAGE_PLAYER) &&
      millis() - pendingStartedAt > ACTION_TIMEOUT) {
    showMessage("Waktu habis!", "Ulangi scan", false, 1200);
    beepError();
    goIdle();
  }
}

int propertyPrice(const CardDef& card) {
  if (card.price > 0) {
    return card.price;
  }
  if (card.mortgagePrice > 0) {
    return card.mortgagePrice * 2;
  }
  if (card.propertyKind == PROP_COMPANY) {
    return 150;
  }
  return 0;
}

int mortgageValue(const CardDef& card) {
  if (card.mortgagePrice > 0) {
    return card.mortgagePrice;
  }
  return propertyPrice(card) / 2;
}

int unmortgageValue(const CardDef& card) {
  if (card.unmortgagePrice > 0) {
    return card.unmortgagePrice;
  }
  int base = mortgageValue(card);
  return base + ((base + 9) / 10);
}

bool sameGroup(const char* a, const char* b) {
  return String(a).length() > 0 && String(a) == String(b);
}

bool ownsFullColorSet(int playerIndex, const CardDef& card) {
  if (card.propertyKind != PROP_CITY || String(card.group).length() == 0) {
    return false;
  }

  int total = 0;
  int owned = 0;
  for (int i = 0; i < CARD_COUNT; i++) {
    if (CARD_DEFS[i].kind == CARD_PROPERTY &&
        CARD_DEFS[i].propertyKind == PROP_CITY &&
        sameGroup(CARD_DEFS[i].group, card.group)) {
      total++;
      if (properties[i].owner == playerIndex && !properties[i].mortgaged) {
        owned++;
      }
    }
  }
  return total > 0 && total == owned;
}

int countOwnedKind(int playerIndex, PropertyKind kind) {
  int count = 0;
  for (int i = 0; i < CARD_COUNT; i++) {
    if (CARD_DEFS[i].kind == CARD_PROPERTY &&
        CARD_DEFS[i].propertyKind == kind &&
        properties[i].owner == playerIndex &&
        !properties[i].mortgaged) {
      count++;
    }
  }
  return count;
}

int countOwnedProperties(int playerIndex) {
  int count = 0;
  for (int i = 0; i < CARD_COUNT; i++) {
    if (CARD_DEFS[i].kind == CARD_PROPERTY && properties[i].owner == playerIndex) {
      count++;
    }
  }
  return count;
}

int countMortgagedProperties(int playerIndex) {
  int count = 0;
  for (int i = 0; i < CARD_COUNT; i++) {
    if (CARD_DEFS[i].kind == CARD_PROPERTY &&
        properties[i].owner == playerIndex &&
        properties[i].mortgaged) {
      count++;
    }
  }
  return count;
}

int rentForProperty(int cardIndex) {
  const CardDef& card = CARD_DEFS[cardIndex];
  PropertyState& state = properties[cardIndex];
  if (state.owner < 0 || state.mortgaged) {
    return 0;
  }

  if (card.propertyKind == PROP_CITY) {
    if (state.level >= 5) {
      return card.rentHotel;
    }
    if (state.level == 4) {
      return card.rent4;
    }
    if (state.level == 3) {
      return card.rent3;
    }
    if (state.level == 2) {
      return card.rent2;
    }
    if (state.level == 1) {
      return card.rent1;
    }
    if (ownsFullColorSet(state.owner, card) && card.rentColorSet > 0) {
      return card.rentColorSet;
    }
    return card.rent;
  }

  if (card.propertyKind == PROP_STATION) {
    int count = constrain(countOwnedKind(state.owner, PROP_STATION), 1, 4);
    if (count == 4) return card.rent4;
    if (count == 3) return card.rent3;
    if (count == 2) return card.rent2;
    return card.rent1;
  }

  if (card.propertyKind == PROP_COMPANY) {
    int count = constrain(countOwnedKind(state.owner, PROP_COMPANY), 1, 2);
    return count == 2 ? card.rent2 : card.rent1;
  }

  return 0;
}

void showBalance(int playerIndex) {
  int propertyCount = countOwnedProperties(playerIndex);
  int mortgagedCount = countMortgagedProperties(playerIndex);
  showMessage("P" + String(playerIndex + 1) + " Saldo:" + String(players[playerIndex].saldo),
              "Prop:" + String(propertyCount) + " M:" + String(mortgagedCount),
              true,
              1800);
}

bool payBank(int playerIndex, int amount, String title) {
  if (amount <= 0) {
    showMessage(title, "Tidak ada biaya", true, 1200);
    return true;
  }

  if (players[playerIndex].saldo < amount) {
    players[playerIndex].saldo = 0;
    showMessage("Uang tidak cukup", "Saldo jadi 0", false, 1600);
    beepError();
    return false;
  }

  players[playerIndex].saldo -= amount;
  showMessage(title, "- " + String(amount) + " S:" + String(players[playerIndex].saldo), true, 1600);
  return true;
}

void payOwner(int payer, int owner, int amount) {
  if (amount <= 0) {
    showMessage("Bebas bayar", "Tidak ada sewa", true, 1200);
    return;
  }

  int paid = amount;
  if (players[payer].saldo < amount) {
    paid = players[payer].saldo;
    players[payer].saldo = 0;
    players[owner].saldo += paid;
    showMessage("Uang tidak cukup", "Bayar sisa " + String(paid), false, 1800);
    beepError();
    return;
  }

  players[payer].saldo -= amount;
  players[owner].saldo += amount;
  showMessage("Bayar P" + String(owner + 1),
              String(amount) + " S:" + String(players[payer].saldo),
              true,
              1600);
}

void buyOrUpgradeProperty(int cardIndex, int playerIndex) {
  const CardDef& card = CARD_DEFS[cardIndex];
  PropertyState& state = properties[cardIndex];

  if (state.owner < 0) {
    int price = propertyPrice(card);
    if (price <= 0) {
      showMessage("Data harga", "belum lengkap", false, 1600);
      beepError();
      return;
    }
    if (players[playerIndex].saldo < price) {
      showMessage("Uang tidak cukup", "Butuh " + String(price), false, 1600);
      beepError();
      return;
    }

    players[playerIndex].saldo -= price;
    state.owner = playerIndex;
    state.level = 0;
    state.mortgaged = false;
    showMessage("Property terbeli", "P" + String(playerIndex + 1) + " -" + String(price), true, 1800);
    return;
  }

  if (state.owner == playerIndex) {
    if (state.mortgaged) {
      showMessage("Property mortgage", "Unmortgage dulu", false, 1600);
      beepError();
      return;
    }

    if (card.propertyKind != PROP_CITY) {
      showMessage("Sudah dimiliki", "Tidak bisa upgrade", true, 1400);
      return;
    }

    if (!ownsFullColorSet(playerIndex, card)) {
      showMessage("Butuh colorset", "Beli set dulu", false, 1600);
      beepError();
      return;
    }

    if (state.level >= 5) {
      showMessage("Sudah hotel", "Max upgrade", false, 1400);
      beepError();
      return;
    }

    int cost = state.level < 4 ? card.houseCost : card.hotelCost;
    if (cost <= 0) {
      showMessage("Data biaya", "belum lengkap", false, 1600);
      beepError();
      return;
    }
    if (players[playerIndex].saldo < cost) {
      showMessage("Uang tidak cukup", "Butuh " + String(cost), false, 1600);
      beepError();
      return;
    }

    players[playerIndex].saldo -= cost;
    state.level++;
    if (state.level >= 5) {
      showMessage("Hotel terbeli", "P" + String(playerIndex + 1) + " -" + String(cost), true, 1800);
    } else {
      showMessage("Rumah terbeli", "Level " + String(state.level) + " -" + String(cost), true, 1800);
    }
    return;
  }

  int rent = rentForProperty(cardIndex);
  if (rent <= 0) {
    showMessage("Tidak ada sewa", state.mortgaged ? "Mortgage" : "Gratis", true, 1400);
    return;
  }
  payOwner(playerIndex, state.owner, rent);
}

void applyMoneyCard(int cardIndex, int playerIndex) {
  const CardDef& card = CARD_DEFS[cardIndex];

  if (card.action == ACT_RECEIVE) {
    players[playerIndex].saldo += card.value;
    showMessage(card.name, "+ " + String(card.value) + " S:" + String(players[playerIndex].saldo), true, 1700);
    return;
  }

  if (card.action == ACT_PAY || card.action == ACT_TAX) {
    payBank(playerIndex, card.value, card.action == ACT_TAX ? "Bayar pajak" : "Bayar kartu");
    return;
  }

  if (card.action == ACT_JAIL) {
    payBank(playerIndex, card.value > 0 ? card.value : 50, "Keluar penjara");
    return;
  }

  if (card.action == ACT_ACTION_ONLY) {
    showMessage(card.name, "Ikuti instruksi", true, 1800);
    return;
  }

  showMessage(card.name, "Tidak ada aksi", false, 1400);
}

void processMortgage(int propertyIndex, int playerIndex) {
  const CardDef& card = CARD_DEFS[propertyIndex];
  PropertyState& state = properties[propertyIndex];

  if (state.owner < 0) {
    showMessage("Belum dimiliki", "Tidak bisa", false, 1500);
    beepError();
    return;
  }
  if (state.owner != playerIndex) {
    showMessage("Bukan milik P" + String(playerIndex + 1), "Ditolak", false, 1500);
    beepError();
    return;
  }

  if (!state.mortgaged && state.level > 0) {
    int value = 0;
    if (state.level >= 5) {
      value = card.hotelCost / 2;
      state.level = 4;
      players[playerIndex].saldo += value;
      showMessage("Hotel terjual", "+" + String(value) + " Lv:4", true, 1700);
      return;
    }

    value = card.houseCost / 2;
    state.level--;
    players[playerIndex].saldo += value;
    showMessage("Rumah terjual", "+" + String(value) + " Lv:" + String(state.level), true, 1700);
    return;
  }

  if (!state.mortgaged) {
    int value = mortgageValue(card);
    if (value <= 0) {
      showMessage("Mortgage kosong", "Cek data", false, 1500);
      beepError();
      return;
    }
    state.mortgaged = true;
    players[playerIndex].saldo += value;
    showMessage("Mortgage OK", "+" + String(value) + " S:" + String(players[playerIndex].saldo), true, 1700);
    return;
  }

  int cost = unmortgageValue(card);
  if (players[playerIndex].saldo < cost) {
    showMessage("Uang tidak cukup", "Butuh " + String(cost), false, 1600);
    beepError();
    return;
  }
  players[playerIndex].saldo -= cost;
  state.mortgaged = false;
  showMessage("Unmortgage OK", "-" + String(cost) + " S:" + String(players[playerIndex].saldo), true, 1700);
}

void registerPlayer(int cardIndex) {
  if (CARD_DEFS[cardIndex].kind != CARD_PLAYER) {
    showMessage("Bukan kartu", "player", false, 1000);
    beepError();
    return;
  }

  if (findPlayerByCard(cardIndex) >= 0) {
    showMessage("Player sudah", "terdaftar", false, 1000);
    beepError();
    return;
  }

  if (playerCount >= MAX_PLAYERS) {
    showMessage("Player penuh", "Max 4", false, 1000);
    beepError();
    return;
  }

  players[playerCount].cardIndex = cardIndex;
  players[playerCount].saldo = SALDO_AWAL;
  players[playerCount].active = true;
  playerCount++;
  showMessage("P" + String(playerCount) + " terdaftar", "Saldo " + String(SALDO_AWAL), true, 1200);
  showMessage("Scan kartu player", "Tombol = mulai", false, 0);
}

void showTurnOrder() {
  lcd.clear();
  String line1 = "";
  String line2 = "";
  for (int i = 0; i < playerCount; i++) {
    String part = "P" + String(i + 1) + ":" + String(i + 1) + " ";
    if (i < 2) {
      line1 += part;
    } else {
      line2 += part;
    }
  }
  printLine(0, line1);
  printLine(1, line2);
  beepOk();
  delay(2500);
}

void startGameIfReady() {
  if (playerCount < 2) {
    showMessage("Min 2 Player", "Scan lagi", false, 1400);
    beepError();
    showMessage("Scan kartu player", "Tombol = mulai", false, 0);
    return;
  }

  showTurnOrder();
  goIdle();
}

void handleIdleCard(int cardIndex) {
  const CardDef& card = CARD_DEFS[cardIndex];

  if (card.kind == CARD_PLAYER) {
    int playerIndex = findPlayerByCard(cardIndex);
    if (playerIndex >= 0) {
      showBalance(playerIndex);
      goIdle();
    } else {
      showMessage("Player tidak", "ikut game", false, 1400);
      beepError();
      goIdle();
    }
    return;
  }

  if (card.kind == CARD_PROPERTY) {
    startPending(STAGE_WAIT_PLAYER, cardIndex, card.name, "Scan player");
    return;
  }

  if (card.kind == CARD_COMMUNITY || card.kind == CARD_CHANCE) {
    startPending(STAGE_WAIT_PLAYER, cardIndex, card.kind == CARD_CHANCE ? "Chance" : "Dana Umum", "Scan player");
    return;
  }

  if (card.kind == CARD_CUSTOM) {
    if (card.action == ACT_MORTGAGE) {
      startPending(STAGE_WAIT_MORTGAGE_PROPERTY, cardIndex, "Mortgage mode", "Scan property");
      return;
    }
    if (card.action == ACT_TAX || card.action == ACT_JAIL) {
      startPending(STAGE_WAIT_PLAYER, cardIndex, card.name, "Scan player");
      return;
    }
  }

  showMessage("Kartu belum", "didukung", false, 1400);
  beepError();
  goIdle();
}

void handleWaitingPlayer(int cardIndex) {
  int playerIndex = findPlayerByCard(cardIndex);
  if (playerIndex < 0) {
    showMessage("Scan kartu", "player aktif", false, 1000);
    beepError();
    return;
  }

  const CardDef& card = CARD_DEFS[pendingCard];
  if (card.kind == CARD_PROPERTY) {
    buyOrUpgradeProperty(pendingCard, playerIndex);
  } else {
    applyMoneyCard(pendingCard, playerIndex);
  }
  goIdle();
}

void handleMortgageProperty(int cardIndex) {
  if (CARD_DEFS[cardIndex].kind != CARD_PROPERTY) {
    showMessage("Scan kartu", "property", false, 1000);
    beepError();
    return;
  }

  pendingProperty = cardIndex;
  pendingStartedAt = millis();
  stage = STAGE_WAIT_MORTGAGE_PLAYER;
  showMessage(CARD_DEFS[cardIndex].name, "Scan owner", true, 0);
}

void handleMortgagePlayer(int cardIndex) {
  int playerIndex = findPlayerByCard(cardIndex);
  if (playerIndex < 0) {
    showMessage("Scan kartu", "player aktif", false, 1000);
    beepError();
    return;
  }

  processMortgage(pendingProperty, playerIndex);
  goIdle();
}

void handleScannedCard(int cardIndex) {
  if (stage == STAGE_REGISTER) {
    registerPlayer(cardIndex);
    return;
  }

  if (stage == STAGE_IDLE) {
    handleIdleCard(cardIndex);
    return;
  }

  if (stage == STAGE_WAIT_PLAYER) {
    handleWaitingPlayer(cardIndex);
    return;
  }

  if (stage == STAGE_WAIT_MORTGAGE_PROPERTY) {
    handleMortgageProperty(cardIndex);
    return;
  }

  if (stage == STAGE_WAIT_MORTGAGE_PLAYER) {
    handleMortgagePlayer(cardIndex);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN_START, INPUT_PULLUP);

  SPI.begin();
  mfrc522.PCD_Init();

  Wire.begin(D2, D1);
  lcd.init();
  lcd.backlight();

  for (int i = 0; i < CARD_COUNT; i++) {
    properties[i].owner = -1;
    properties[i].level = 0;
    properties[i].mortgaged = false;
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    players[i].cardIndex = -1;
    players[i].saldo = SALDO_AWAL;
    players[i].active = false;
  }

  showMessage("Tekan tombol", "untuk mulai", false, 0);
}

void loop() {
  if (stage == STAGE_WAIT_START && buttonPressed()) {
    stage = STAGE_REGISTER;
    showMessage("Scan kartu player", "untuk daftar", true, 0);
    return;
  }

  if (stage == STAGE_REGISTER && buttonPressed()) {
    startGameIfReady();
    return;
  }

  if (stage == STAGE_IDLE) {
    showIdle();
  }

  checkPendingTimeout();

  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  String uid = getUID();
  int cardIndex = findCardByUid(uid);
  Serial.print("SCAN,");
  Serial.println(uid);

  if (cardIndex < 0) {
    showMessage("Kartu tidak", "terdaftar", false, 1000);
    beepError();
    mfrc522.PICC_HaltA();
    return;
  }

  handleScannedCard(cardIndex);
  updateIdleText();

  mfrc522.PICC_HaltA();
  delay(120);
}
