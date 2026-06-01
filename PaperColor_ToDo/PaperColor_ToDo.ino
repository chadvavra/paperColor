/*
 * PaperColor_ToDo.ino
 * ───────────────────────────────────────────────────────────
 * M5Stack PaperColor — BLE ToDo companion display
 *
 * Required libraries (Arduino Library Manager):
 *   • M5Unified       >= 0.2.15
 *   • M5GFX           >= 0.2.21
 *   • NimBLE-Arduino  >= 1.4.0
 *   • ArduinoJson     >= 6.21.0
 *
 * Board target: M5PaperColor
 *
 * BLE Protocol (Nordic UART Service):
 *   Phone → Device (RX char, WRITE):
 *     ADD:<text>   – add a new todo
 *     DONE:<id>    – toggle done/undone
 *     DEL:<id>     – delete a todo
 *     SYNC         – push full list to phone
 *     CLEAR        – delete all todos
 *
 *   Device → Phone (TX char, NOTIFY):
 *     {"todos":[{"id":1,"text":"Buy milk","done":false}…],"count":N}
 *
 * Buttons (on PaperColor):
 *   BtnA (G10) = scroll up
 *   BtnB (G9)  = scroll down
 *   BtnC (G1)  = mark selected item done
 */

#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <M5PM1.h>

// ── BLE UUIDs (Nordic UART Service) ──────────────────────
#define BLE_DEVICE_NAME   "PaperColor-ToDo"
#define SERVICE_UUID      "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ── Storage ───────────────────────────────────────────────
#define TODO_FILE           "/todos.json"
#define MAX_TODOS           20

// SD SPI pins (from official M5Stack PaperColor docs)
#define SD_SPI_CS_PIN       47
#define SD_SPI_SCK_PIN      15
#define SD_SPI_MOSI_PIN     13
#define SD_SPI_MISO_PIN     14

// ── Display layout constants ──────────────────────────────
#define HEADER_H          50
#define FOOTER_H          34
#define ITEM_H            40    // height of each todo row
#define ITEM_PAD_X        10
#define CB_SIZE           22    // checkbox square size
#define SELECTED_COLOR    YELLOW

// ─────────────────────────────────────────────────────────
struct Todo {
    int    id;
    String text;
    bool   done;
};

// ── M5PM1 (SD power + GPIO expander) ─────────────────────
M5PM1 pm1;

// ── Global state ──────────────────────────────────────────
Todo    todos[MAX_TODOS];
int     todoCount   = 0;
int     nextId      = 1;
int     scrollTop   = 0;   // first visible item index
int     selectedIdx = 0;   // which item BtnB acts on
bool    dirty       = true;
bool    sdReady     = false;
bool    fullRefresh = true; // use quality mode on first draw

// ── BLE ───────────────────────────────────────────────────
NimBLECharacteristic* txChar   = nullptr;
volatile bool         bleConn  = false;
volatile bool         hasCmd   = false;
String                pendingCmd;

// ── Sleep ─────────────────────────────────────────────────
#define SLEEP_TIMEOUT_MS  90000UL   // 90s inactivity → deep sleep (>refresh time)
// Button pins from official PaperColor pinmap
#define BTN_A_PIN         10        // USER_KEY3 (Button A) = G10
#define BTN_B_PIN         9         // USER_KEY2 (Button B) = G9
#define BTN_C_PIN         1         // USER_KEY1 (Button C) = G1
unsigned long lastActivityMs = 0;

// ── Direct GPIO button edge detection ────────────────────
// Bypasses M5Unified's button abstraction which can miss presses
// during long EPD refreshes. Buttons are active-LOW.
bool btnALast = HIGH, btnBLast = HIGH, btnCLast = HIGH;

bool btnAPressed() {
    bool cur = digitalRead(BTN_A_PIN);
    bool pressed = (cur == LOW && btnALast == HIGH);
    btnALast = cur;
    return pressed;
}
bool btnBPressed() {
    bool cur = digitalRead(BTN_B_PIN);
    bool pressed = (cur == LOW && btnBLast == HIGH);
    btnBLast = cur;
    return pressed;
}
bool btnCPressed() {
    bool cur = digitalRead(BTN_C_PIN);
    bool pressed = (cur == LOW && btnCLast == HIGH);
    btnCLast = cur;
    return pressed;
}

// ── Canvas ────────────────────────────────────────────────
M5Canvas canvas(&M5.Display);

// ──────────────────────────────────────────────────────────
//  Utility
// ──────────────────────────────────────────────────────────
int visibleRows() {
    return (M5.Display.height() - HEADER_H - FOOTER_H) / ITEM_H;
}

int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ──────────────────────────────────────────────────────────
//  BLE Callbacks
// ──────────────────────────────────────────────────────────
struct RxCallbacks : NimBLECharacteristicCallbacks {
    // NimBLE v1.x: onWrite(NimBLECharacteristic*)
    // NimBLE v2.x: onWrite(NimBLECharacteristic*, NimBLEConnInfo&)
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) {
        pendingCmd = String(c->getValue().c_str());
        hasCmd     = true;
    }
};

struct ServerCallbacks : NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) {
        bleConn    = true;
        dirty      = true;
        fullRefresh = false;
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {
        bleConn    = false;
        dirty      = true;
        fullRefresh = false;
        NimBLEDevice::getAdvertising()->start();
        Serial.println("BLE disconnected — advertising restarted");
    }
};

// ──────────────────────────────────────────────────────────
//  microSD helpers
// ──────────────────────────────────────────────────────────
void loadTodos() {
    if (!SD.exists(TODO_FILE)) return;
    File f = SD.open(TODO_FILE);
    if (!f) return;

    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, f) != DeserializationError::Ok) {
        f.close(); return;
    }

    todoCount = 0;
    nextId    = 1;
    for (JsonObject o : doc["todos"].as<JsonArray>()) {
        if (todoCount >= MAX_TODOS) break;
        todos[todoCount].id   = o["id"]   | nextId;
        todos[todoCount].text = o["text"].as<const char*>();
        todos[todoCount].done = o["done"] | false;
        if (todos[todoCount].id >= nextId) nextId = todos[todoCount].id + 1;
        todoCount++;
    }
    f.close();
    Serial.printf("Loaded %d todos from SD\n", todoCount);
}

void saveTodos() {
    if (!sdReady) return;
    File f = SD.open(TODO_FILE, FILE_WRITE);
    if (!f) { Serial.println("SD write failed"); return; }

    DynamicJsonDocument doc(8192);
    JsonArray arr = doc.createNestedArray("todos");
    for (int i = 0; i < todoCount; i++) {
        JsonObject o = arr.createNestedObject();
        o["id"]   = todos[i].id;
        o["text"] = todos[i].text;
        o["done"] = todos[i].done;
    }
    serializeJson(doc, f);
    f.close();
}

// ──────────────────────────────────────────────────────────
//  BLE send (JSON todo list → phone)
// ──────────────────────────────────────────────────────────
void sendList() {
    if (!bleConn || !txChar) return;

    // Build compact JSON
    StaticJsonDocument<4096> doc;
    JsonArray arr = doc.createNestedArray("todos");
    for (int i = 0; i < todoCount; i++) {
        JsonObject o = arr.createNestedObject();
        o["id"]   = todos[i].id;
        o["text"] = todos[i].text;
        o["done"] = todos[i].done;
    }
    doc["count"] = todoCount;

    String json;
    serializeJson(doc, json);

    // NimBLE fragments large payloads automatically across ATT PDUs.
    // Web Bluetooth accumulates them — see web app for reassembly logic.
    txChar->setValue(json.c_str());
    txChar->notify();
    Serial.printf("Sent list (%d bytes)\n", json.length());
}

// ──────────────────────────────────────────────────────────
//  Command processor
// ──────────────────────────────────────────────────────────
void handleCommand(const String& cmd) {
    Serial.printf("CMD: %s\n", cmd.c_str());

    if (cmd == "SYNC") {
        sendList();
        return;
    }

    if (cmd == "CLEAR") {
        todoCount   = 0;
        scrollTop   = 0;
        selectedIdx = 0;
        saveTodos();
        sendList();
        dirty = fullRefresh = true;
        return;
    }

    if (cmd.startsWith("ADD:") && todoCount < MAX_TODOS) {
        String text = cmd.substring(4);
        text.trim();
        if (text.length() > 0) {
            todos[todoCount++] = { nextId++, text, false };
            // auto-scroll to new item
            int rows = visibleRows();
            if (todoCount > rows) scrollTop = todoCount - rows;
            selectedIdx = todoCount - 1;
            saveTodos();
            sendList();
            dirty = fullRefresh = true;
        }
        return;
    }

    if (cmd.startsWith("DONE:")) {
        int id = cmd.substring(5).toInt();
        for (int i = 0; i < todoCount; i++) {
            if (todos[i].id == id) {
                todos[i].done ^= 1;
                saveTodos();
                sendList();
                dirty       = true;
                fullRefresh = false;
                break;
            }
        }
        return;
    }

    if (cmd.startsWith("DEL:")) {
        int id = cmd.substring(4).toInt();
        for (int i = 0; i < todoCount; i++) {
            if (todos[i].id == id) {
                for (int j = i; j < todoCount - 1; j++) todos[j] = todos[j+1];
                todoCount--;
                int maxScroll = max(0, todoCount - visibleRows());
                scrollTop   = clamp(scrollTop,   0, maxScroll);
                selectedIdx = clamp(selectedIdx, 0, max(0, todoCount - 1));
                saveTodos();
                sendList();
                dirty = fullRefresh = true;
                break;
            }
        }
        return;
    }
}

// ──────────────────────────────────────────────────────────
//  Display
// ──────────────────────────────────────────────────────────
void drawItem(int idx, int y) {
    const int W = M5.Display.width();

    // Row background — highlight selected item
    bool isSelected = (idx == selectedIdx);
    canvas.fillRect(0, y, W, ITEM_H, isSelected ? SELECTED_COLOR : WHITE);
    canvas.drawFastHLine(0, y + ITEM_H - 1, W, BLACK);

    // Checkbox
    int cbX = ITEM_PAD_X;
    int cbY = y + (ITEM_H - CB_SIZE) / 2;
    canvas.drawRect(cbX, cbY, CB_SIZE, CB_SIZE, BLACK);
    if (todos[idx].done) {
        canvas.fillRect(cbX + 3, cbY + 3, CB_SIZE - 6, CB_SIZE - 6, GREEN);
        // checkmark lines
        canvas.drawLine(cbX + 4, cbY + CB_SIZE/2,
                        cbX + CB_SIZE/2 - 1, cbY + CB_SIZE - 5, GREEN);
        canvas.drawLine(cbX + CB_SIZE/2 - 1, cbY + CB_SIZE - 5,
                        cbX + CB_SIZE - 3, cbY + 3, GREEN);
    }

    // Todo text
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextDatum(middle_left);
    int tx = ITEM_PAD_X + CB_SIZE + 10;
    int ty = y + ITEM_H / 2;
    canvas.setTextColor(todos[idx].done ? GREEN : BLACK);
    canvas.drawString(todos[idx].text, tx, ty);

    // Strikethrough for done items
    if (todos[idx].done) {
        int tw = canvas.textWidth(todos[idx].text);
        canvas.drawFastHLine(tx, ty, tw, GREEN);
    }
}

void redraw() {
    const int W = M5.Display.width();
    const int H = M5.Display.height();

    // Color e-ink panels don't support true partial refresh —
    // epd_fastest causes the display to appear frozen.
    // epd_fast is the right balance of speed vs. reliability for this panel.
    M5.Display.setEpdMode(epd_mode_t::epd_fast);
    fullRefresh = false;

    canvas.fillSprite(WHITE);

    // ── Header ────────────────────────────────────────────
    canvas.fillRect(0, 0, W, HEADER_H, BLUE);

    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(WHITE);
    canvas.drawString("To-Do List", 14, HEADER_H / 2);

    // Item count badge
    if (todoCount > 0) {
        int pending = 0;
        for (int i = 0; i < todoCount; i++) if (!todos[i].done) pending++;
        canvas.setFont(&fonts::Font2);
        canvas.setTextDatum(middle_center);
        String badge = String(pending) + " left";
        int bw = canvas.textWidth(badge) + 12;
        int bx = W / 2 - bw / 2;
        canvas.fillRoundRect(bx, (HEADER_H - 18) / 2, bw, 18, 9, WHITE);
        canvas.setTextColor(BLUE);
        canvas.drawString(badge, W / 2, HEADER_H / 2);
    }

    // Battery % (top right)
    int32_t batLevel = M5.Power.getBatteryLevel();  // 0-100
    bool charging    = M5.Power.isCharging();

    // Pick color: red <20%, yellow <50%, green otherwise
    uint16_t batColor = GREEN;
    if      (batLevel < 20) batColor = RED;
    else if (batLevel < 50) batColor = YELLOW;

    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(batColor);
    String batStr = charging ? String(batLevel) + "% +" : String(batLevel) + "%";
    canvas.drawString(batStr, W - 8, HEADER_H / 2 + 1);

    // Small BLE status dot (left of battery text)
    int batTxtW = canvas.textWidth(batStr) + 12;
    canvas.fillCircle(W - batTxtW - 14, HEADER_H / 2, 6, bleConn ? GREEN : RED);

    // ── Body ──────────────────────────────────────────────
    if (todoCount == 0) {
        canvas.setFont(&fonts::FreeSans9pt7b);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(BLACK);
        int cy = HEADER_H + (H - HEADER_H - FOOTER_H) / 2;
        canvas.drawString("No tasks yet.", W / 2, cy - 20);
        canvas.drawString("Open the companion app", W / 2, cy + 8);
        canvas.drawString("to add tasks via voice or text.", W / 2, cy + 32);
    } else {
        int rows = visibleRows();
        for (int r = 0; r < rows; r++) {
            int idx = scrollTop + r;
            if (idx >= todoCount) break;
            drawItem(idx, HEADER_H + r * ITEM_H);
        }
        // bottom rule after last item
        int lastY = HEADER_H + min(rows, todoCount - scrollTop) * ITEM_H;
        canvas.drawFastHLine(0, lastY, W, BLACK);
    }

    // ── Footer ────────────────────────────────────────────
    canvas.drawFastHLine(0, H - FOOTER_H, W, BLACK);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(BLACK);
    canvas.setTextDatum(middle_left);
    canvas.drawString("A:Up  B:Dn  C:Done", 8, H - FOOTER_H / 2);

    // Scroll position
    if (todoCount > visibleRows()) {
        canvas.setTextDatum(middle_right);
        int end = min(scrollTop + visibleRows(), todoCount);
        canvas.drawString(String(scrollTop + 1) + "–" + String(end) + "/" + String(todoCount),
                          W - 8, H - FOOTER_H / 2);
    }

    canvas.pushSprite(0, 0);
}

// ──────────────────────────────────────────────────────────
//  Deep sleep — display holds image, BLE dropped
// ──────────────────────────────────────────────────────────
void goToSleep() {
    Serial.println("Going to deep sleep — wake on any button");

    // Draw sleep indicator before going under
    M5.Display.setEpdMode(epd_mode_t::epd_fast);
    canvas.fillRect(0, M5.Display.height() - FOOTER_H, M5.Display.width(), FOOTER_H, WHITE);
    canvas.drawFastHLine(0, M5.Display.height() - FOOTER_H, M5.Display.width(), BLACK);
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(BLACK);
    canvas.drawString("Press any button to wake", M5.Display.width() / 2,
                      M5.Display.height() - FOOTER_H / 2);
    canvas.pushSprite(0, 0);

    // Disconnect BLE cleanly
    if (NimBLEDevice::getServer()->getConnectedCount() > 0) {
        NimBLEDevice::getServer()->disconnect(0);
        delay(100);
    }
    NimBLEDevice::deinit(true);

    // Wake on any of the three buttons (active LOW)
    esp_sleep_enable_ext1_wakeup(
        (1ULL << BTN_A_PIN) | (1ULL << BTN_B_PIN) | (1ULL << BTN_C_PIN),
        ESP_EXT1_WAKEUP_ANY_LOW
    );

    esp_deep_sleep_start();
    // ↑ never returns — MCU resets on wake, setup() runs fresh
}

// ──────────────────────────────────────────────────────────
//  Setup
// ──────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    cfg.clear_display = false;
    M5.begin(cfg);
    Serial.begin(115200);
    Serial.println("\n=== PaperColor ToDo ===");

    // Configure button pins — active LOW with internal pullup
    pinMode(BTN_A_PIN, INPUT_PULLUP);
    pinMode(BTN_B_PIN, INPUT_PULLUP);
    pinMode(BTN_C_PIN, INPUT_PULLUP);
    // Seed last state so first read doesn't false-trigger
    btnALast = digitalRead(BTN_A_PIN);
    btnBLast = digitalRead(BTN_B_PIN);
    btnCLast = digitalRead(BTN_C_PIN);

    // ── microSD (requires M5PM1 to power the card) ────────
    if (pm1.begin(&M5.In_I2C) != M5PM1_OK) {
        Serial.println("M5PM1 init failed — SD unavailable");
    } else {
        // Power up SD card via M5PM1 GPIO expander
        pm1.setLdoEnable(true);
        pm1.pinMode(M5PM1_GPIO_NUM_0, OUTPUT);
        pm1.digitalWrite(M5PM1_GPIO_NUM_0, HIGH);  // PY_EPD_EN
        pm1.pinMode(M5PM1_GPIO_NUM_4, OUTPUT);
        pm1.digitalWrite(M5PM1_GPIO_NUM_4, HIGH);  // PY_SD_DET_EN
        pm1.pinMode(M5PM1_GPIO_NUM_3, OUTPUT);
        pm1.digitalWrite(M5PM1_GPIO_NUM_3, HIGH);  // PY_SD_PWR_EN
        pm1.pinMode(M5PM1_GPIO_NUM_1, INPUT_PULLUP);  // CARD_DET

        if (pm1.digitalRead(M5PM1_GPIO_NUM_1) != LOW) {
            Serial.println("SD card not inserted");
        } else {
            delay(50);  // let SD card power rail stabilize after M5PM1 enable
            SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
            sdReady = SD.begin(SD_SPI_CS_PIN, SPI, 25000000);
            if (!sdReady) {
                Serial.println("SD.begin() at 25MHz failed, retrying at 4MHz...");
                SD.end();
                sdReady = SD.begin(SD_SPI_CS_PIN, SPI, 4000000);
            }
            if (sdReady) {
                Serial.printf("SD ready — type: %d, size: %lluMB\n",
                    SD.cardType(), SD.cardSize() / (1024 * 1024));
                loadTodos();
            } else {
                Serial.println("SD.begin() failed — check card is FAT32 formatted");
            }
        }
    }

    // Display init
    M5.Display.setRotation(0);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    Serial.printf("Display: %d x %d\n", M5.Display.width(), M5.Display.height());
    redraw();

    // BLE init
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setMTU(512);  // request larger MTU for bigger JSON payloads

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());

    NimBLEService* service = server->createService(SERVICE_UUID);

    // RX: phone writes commands here
    NimBLECharacteristic* rxChar = service->createCharacteristic(
        CHAR_RX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    rxChar->setCallbacks(new RxCallbacks());

    // TX: device notifies phone with JSON
    txChar = service->createCharacteristic(
        CHAR_TX_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    service->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    Serial.println("BLE: got advertising object");

    // Explicitly build advertising data — NimBLE v2.x doesn't auto-include
    // the device name in the packet, which breaks Web Bluetooth name filters
    NimBLEAdvertisementData advData;
    advData.setName(BLE_DEVICE_NAME);
    advData.addServiceUUID(SERVICE_UUID);
    adv->setAdvertisementData(advData);

    Serial.println("BLE: advertising data set");
    bool advOk = adv->start();
    Serial.printf("BLE advertising start: %s\n", advOk ? "OK" : "FAILED");
    Serial.println("BLE advertising as: " BLE_DEVICE_NAME);
    lastActivityMs = millis();
    Serial.println("Setup complete");
}

// ──────────────────────────────────────────────────────────
//  Loop
// ──────────────────────────────────────────────────────────
void loop() {
    M5.update();

    // Process any pending BLE command
    if (hasCmd) {
        hasCmd = false;
        handleCommand(pendingCmd);
    }

    // Direct GPIO reads — catches presses that happen during EPD refresh
    // since we re-read pins every loop iteration regardless
    if (btnAPressed()) {
        if (todoCount > 0) {
            selectedIdx = max(0, selectedIdx - 1);
            if (selectedIdx < scrollTop) scrollTop = selectedIdx;
            dirty = true; fullRefresh = false;
        }
    }

    if (btnBPressed()) {
        if (todoCount > 0) {
            selectedIdx = min(todoCount - 1, selectedIdx + 1);
            if (selectedIdx >= scrollTop + visibleRows())
                scrollTop = selectedIdx - visibleRows() + 1;
            dirty = true; fullRefresh = false;
        }
    }

    if (btnCPressed()) {
        if (todoCount > 0) {
            selectedIdx = clamp(selectedIdx, scrollTop,
                                min(scrollTop + visibleRows() - 1, todoCount - 1));
            String cmd = "DONE:" + String(todos[selectedIdx].id);
            handleCommand(cmd);
        }
    }

    if (dirty) {
        dirty = false;
        redraw();
        // Reset activity timer AFTER the refresh completes —
        // a 20s EPD refresh would otherwise eat most of the timeout
        lastActivityMs = millis();
    }

    // Only sleep when BLE is disconnected to avoid dropping an active session
    if (!bleConn && (millis() - lastActivityMs > SLEEP_TIMEOUT_MS)) {
        goToSleep();
    }

    delay(50);
}
