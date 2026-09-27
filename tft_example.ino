#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "background.cpp"

// --- HARDVER KONFIGURÁCIÓ (Változatlan S3 + ILI9341 felépítés) ---
class LGFX_ESP32S3_ILI9341 : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
public:
  LGFX_ESP32S3_ILI9341() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 9;
      cfg.pin_dc   = 13;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 10;
      cfg.pin_rst          = 14;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.readable         = false;
      cfg.invert           = false;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX_ESP32S3_ILI9341 tft;
LGFX_Sprite canvas(&tft); // Fő játék-puffer a villódzásmentességért

// 'background', 320x240px


// Array of all bitmaps for convenience. (Total bytes used to store images in PROGMEM = 76816)
const int epd_bitmap_allArray_LEN = 1;
const uint16_t* epd_bitmap_allArray[1] = {
  epd_bitmap_background
};


// --- JÁTÉK VÁLTOZÓK ---
int playerX = 160;        // Az űrhajó X pozíciója
const int playerY = 200;  // Fix Y pozíció az űrhajónak
const int playerWidth = 20;
const int playerHeight = 15;

int enemyX = 160;         // Asteroid X pozíciója
float enemyY = 0;         // Asteroid Y pozíciója
float enemySpeed = 3.0;   // Kezdő sebesség

int score = 0;
bool gameOver = false;

void resetGame() {
  playerX = 160;
  enemyX = random(20, 300);
  enemyY = 0;
  enemySpeed = 3.0;
  score = 0;
  gameOver = false;
}

void setup() {
  Serial.begin(115200); // Soros kommunikáció a billentyűzethez
  tft.init();
  tft.setRotation(1);   // Fekvő mód (320x240)
  canvas.createSprite(320, 240);
  canvas.setSwapBytes(true);
  resetGame();
  
  Serial.println("--- RETRO DODGER JÁTÉK ---");
  Serial.println("Irányítás a Serial Monitorról:");
  Serial.println("'a' + Enter -> Balra");
  Serial.println("'d' + Enter -> Jobbra");
  Serial.println("'r' + Enter -> Újrajáték (ha meghaltál)");
}

void loop() {
  // 1. INPUT KEZELÉS: Olvassuk a PC billentyűzetét a Soros Porton keresztül
  if (Serial.available() > 0) {
    char input = Serial.read();
    
    if (!gameOver) {
      if (input == 'a' || input == 'A') {
        playerX -= 15; // Mozgás balra
      }
      if (input == 'd' || input == 'D') {
        playerX += 15; // Mozgás jobbra
      }
      // Határok betartása
      if (playerX < 10) playerX = 10;
      if (playerX > 310 - playerWidth) playerX = 310 - playerWidth;
    } else {
      if (input == 'r' || input == 'R') {
        resetGame(); // Újraindítás
      }
    }
  }

  // JÁTÉK LOGIKA
  if (!gameOver) {
    // Ellenség (Aszteroida) mozgatása lefelé
    enemyY += enemySpeed;
    
    // Ha az ellenség leért a képernyőről
    if (enemyY > 240) {
      enemyY = 0;
      enemyX = random(20, 300); // Új pozíció felül
      score++;
      enemySpeed += 0.2; // Fokozatosan nehezedik a játék
    }

    // ÜTKÖZÉS DETEKTÁLÁS (AABB Collision)
    if (enemyX < playerX + playerWidth &&
        enemyX + 15 > playerX &&
        enemyY < playerY + playerHeight &&
        enemyY + 15 > playerY) {
      gameOver = true;
      Serial.println("GAME OVER! Nyomj 'r'-t az új játékhoz.");
    }
  }

  // --- GRAFIKA KIRAJZOLÁSA A PUFFERBE ---
  canvas.pushImage(0, 0, 320, 240, epd_bitmap_background); 

  if (!gameOver) {
    // A fix csillagok (canvas.drawPixel) sorait törölheted, mert már ott a háttérképed!

    // Zöld űrhajó megrajzolása (Háromszög / Trapéz forma)
    canvas.fillTriangle(playerX + (playerWidth/2), playerY, playerX, playerY + playerHeight, playerX + playerWidth, playerY + playerHeight, TFT_GREEN);
    canvas.fillRect(playerX + (playerWidth/4), playerY + playerHeight - 4, playerWidth/2, 4, TFT_DARKGREEN);

    // Piros Aszteroida megrajzolása
    canvas.fillCircle(enemyX, (int)enemyY, 8, TFT_RED);
    canvas.fillCircle(enemyX - 2, (int)enemyY - 2, 2, TFT_ORANGE); // Kicsi kráter részlet

    // Pontszám kiírása
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(TFT_WHITE);
    canvas.setCursor(10, 10);
    canvas.printf("SCORE: %d", score);
  } 
  else {
    // GAME OVER Képernyő (A háttérképre fog ráégni a felirat)
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(TFT_RED);
    canvas.drawCenterString("GAME OVER", 160, 80);
    
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(TFT_YELLOW);
    canvas.drawCenterString("Elért pontszám: " + String(score), 160, 130);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawCenterString("Nyomj 'r'-t a PC-n az uj játékhoz", 160, 160);
  }

  // Puffer kiküldése a kijelzőre
  canvas.pushSprite(0, 0);

  delay(20); // Kb. 50 FPS frissítési limit
}
