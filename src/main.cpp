#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h> 
#include <WebServer.h>
#include "Audio.h" 
#include "Wire.h"
#include "es8311.h" 
#include "FS.h"
#include "SD_MMC.h"
#include <TFT_eSPI.h> 
#include <time.h> 

// La déchiqueteuse cryptographique pour hacher les mots de passe Wi-Fi en PSK WPA2
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

// ==============================================================================
// 1. VÉRITÉ MATÉRIELLE (LES PORTS PHYSIQUES DU BOARD FREENOVE)
// ==============================================================================
#define I2S_MCK   4   
#define I2S_BCK   5   
#define I2S_DINT  6   
#define I2S_DOUT  8   
#define I2S_WS    7   
#define AP_ENABLE 1   // Valve électrique de l'amplificateur (Active Low)

#define I2C_SCL   15  
#define I2C_SDA   16  
#define I2C_SPEED 400000 

#define SD_SCK 38
#define SD_CMD 40
#define SD_D0  39
#define SD_D1  41
#define SD_D2  48
#define SD_D3  47

#define TOUCH_RST 18         
#define TOUCH_I2C_ADDR 0x38  
#define BAT_ADC_PIN 9        

// ==============================================================================
// 2. VARIABLES GLOBALES ET MACHINE À ÉTATS
// ==============================================================================
WiFiMulti wifiMulti; 
String time_zone = "EST5EDT,M3.2.0,M11.1.0"; 

struct Station {
  String name;
  String url;
};
Station stations[50]; 
int stationCount = 0;
int listOffset = 0; 

bool inFavoritesMenu = false; 

String mp3Files[15]; 
int mp3Count = 0;
int currentMp3Index = 0;
bool isMp3Playing = false; 
bool autoNextTrack = false; 

String metaTitle = "Chargement...";
String metaArtist = "Recherche tags...";
String metaAlbum = "";
bool metaUpdated = false; 

enum AppState { 
  MAIN_MENU,          
  FM_CONSTRUCTION,    
  NET_MENU,           
  NET_STATIONS_LIST,  
  NET_PLAYING,        
  MP3_PLAYER,         
  SCREENSAVER,        
  SETUP_AP            // Mode Ad-Hoc de secours (Point d'accès local)
};
AppState currentState = MAIN_MENU; 
AppState previousState = MAIN_MENU; 

int currentStationIndex = -1; 
int currentVolume = 12; 

const unsigned long IDLE_TIMEOUT = 60000; 
unsigned long lastTouchTime = 0; 
unsigned long lastStatusUpdate = 0; 
unsigned long lastTimeUpdate = 0; 
unsigned long lastSaverUpdate = 0; 
unsigned long lastMp3TimeUpdate = 0; 

Audio audio;                
TFT_eSPI tft = TFT_eSPI();  
WebServer server(80);       
TaskHandle_t TaskUI_Handle; 

// ==============================================================================
// 3. CALLBACKS AUDIO (Core 1)
// ==============================================================================
void audio_id3data(const char *info) {
  // Les metadonnees arrivent par morceaux depuis le decodeur audio.
  String s = String(info);
  if (s.startsWith("Title: ")) { metaTitle = s.substring(7); metaUpdated = true; }
  else if (s.startsWith("Artist: ")) { metaArtist = s.substring(8); metaUpdated = true; }
  else if (s.startsWith("Album: ")) { metaAlbum = s.substring(7); metaUpdated = true; }
}

void audio_eof_mp3(const char *info) {
  // La tache UI lancera la piste suivante hors du callback audio.
  if (currentState == MP3_PLAYER) {
    autoNextTrack = true; 
  }
}

void resetMeta(String filename) {
  metaTitle = filename; 
  metaArtist = "Recherche tags...";
  metaAlbum = "";
  metaUpdated = true; 
}

// ==============================================================================
// 4. SYSTÈMES ET FICHIERS SD
// ==============================================================================
String readSD(const char* path) {
  // Retourne une chaine vide si le fichier n'existe pas ou ne peut pas etre ouvert.
  File file = SD_MMC.open(path);
  if (!file) return "";
  String content = file.readString();
  file.close();
  return content;
}

void writeSD(const char* path, String data) {
  File file = SD_MMC.open(path, FILE_WRITE);
  if (file) { file.print(data); file.close(); }
}

String getConfigValue(String content, String key) {
  // Le fichier de configuration est volontairement simple : une cle par ligne.
  int idx = content.indexOf(key + "=");
  if (idx == -1) return "";
  int start = idx + key.length() + 1;
  int end = content.indexOf('\n', start);
  if (end == -1) end = content.length();
  String val = content.substring(start, end);
  val.trim();
  return val;
}

bool isFavorite(String url) {
  File f = SD_MMC.open("/favoris.m3u");
  if (!f) return false;
  while(f.available()){
    String line = f.readStringUntil('\n'); line.trim();
    if(line == url) {
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

void toggleFavorite(Station s) {
  // Reconstruit le fichier M3U pour ajouter ou supprimer la station selectionnee.
  String favPath = "/favoris.m3u";
  File f = SD_MMC.open(favPath);
  bool found = false;
  String newContent = "";

  if (f) {
    String currentName = "";
    while (f.available()) {
      String line = f.readStringUntil('\n'); line.trim();
      if (line.length() == 0) continue;

      if (line.startsWith("#EXTINF")) {
        currentName = line; 
      } else if (!line.startsWith("#")) {
        if (line == s.url) {
          found = true; 
          currentName = ""; 
        } else {
          if (currentName != "") newContent += currentName + "\n";
          newContent += line + "\n";
          currentName = "";
        }
      }
    }
    f.close();
  }

  if (found) {
    writeSD(favPath.c_str(), newContent);
  } else {
    newContent += "#EXTINF:-1," + s.name + "\n" + s.url + "\n";
    writeSD(favPath.c_str(), newContent);
  }
}

String generateWPA2_PSK(String ssid, String password) {
  // Accepte aussi une PSK deja calculee afin d'eviter un hachage inutile.
  if (password.length() == 64) return password; 
  if (password.length() == 0) return "";
  
  unsigned char psk_bytes[32];
  mbedtls_md_context_t sha1_ctx;
  mbedtls_md_init(&sha1_ctx);
  mbedtls_md_setup(&sha1_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
  mbedtls_pkcs5_pbkdf2_hmac(&sha1_ctx, 
                            (const unsigned char *)password.c_str(), password.length(), 
                            (const unsigned char *)ssid.c_str(), ssid.length(), 
                            4096, 32, psk_bytes);
  mbedtls_md_free(&sha1_ctx);
  
  String pskHex = "";
  for (int i = 0; i < 32; i++) {
    char hex[3]; sprintf(hex, "%02x", psk_bytes[i]); pskHex += hex;
  }
  return pskHex;
}

void loadM3UList(const char* filename) {
  // Convertit les paires #EXTINF/URL du fichier M3U en stations affichables.
  stationCount = 0;
  listOffset = 0; 
  
  File m3uFile = SD_MMC.open(filename);
  if (m3uFile) {
    String currentName = "";
    while (m3uFile.available() && stationCount < 50) { 
      String line = m3uFile.readStringUntil('\n'); line.trim(); 
      if (line.length() == 0) continue; 
      
      if (line.startsWith("#EXTINF")) { 
        int commaIndex = line.indexOf(','); 
        if (commaIndex != -1) currentName = line.substring(commaIndex + 1); 
      } 
      else if (!line.startsWith("#")) { 
        stations[stationCount].name = currentName != "" ? currentName : "Station " + String(stationCount + 1);
        stations[stationCount].url = line;
        stationCount++; 
        currentName = ""; 
      }
    }
    m3uFile.close();
  }
}

void loadMP3Folder() {
  // Limite volontairement la liste aux 15 premiers fichiers supportes.
  mp3Count = 0; 
  File dir = SD_MMC.open("/MP3s");
  if (!dir || !dir.isDirectory()) return; 
  
  File file = dir.openNextFile(); 
  while (file && mp3Count < 15) { 
    if (!file.isDirectory()) { 
      String fileName = file.name();
      if (fileName.endsWith(".mp3") || fileName.endsWith(".MP3") || 
          fileName.endsWith(".flac") || fileName.endsWith(".FLAC")) {
        mp3Files[mp3Count] = fileName; 
        mp3Count++; 
      }
    }
    file = dir.openNextFile(); 
  }
}

// ==============================================================================
// 5. LA PAGE WEB DE CONFIGURATION
// ==============================================================================
void setupWebServer() {
  // Cette page sert uniquement a configurer la radio depuis le point d'acces.
  server.on("/", HTTP_GET, []() {
    String cfg = readSD("/config.ini");
    String s1 = getConfigValue(cfg, "SSID_1");
    String s2 = getConfigValue(cfg, "SSID_2");
    String s3 = getConfigValue(cfg, "SSID_3");

    String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>Radio Config</title>";
    html += "<style>body{font-family:sans-serif; background:#1e1e2e; color:#cdd6f4; margin:0;} input, textarea{background:#313244; color:#cdd6f4; border:1px solid #585b70; width:100%; padding:8px; border-radius:4px; box-sizing:border-box;} textarea{height:120px; font-family:monospace;} .box{max-width:600px; margin:20px auto; padding:20px; background:#181825; border-radius:8px;} label{font-weight:bold; color:#89b4fa; display:block; margin-top:10px;} .btn{padding:12px; background:#89b4fa; color:#11111b; font-weight:bold; border:none; border-radius:4px; cursor:pointer; width:100%; margin-top:20px;} small{display:block; margin-top:2px; color:#a6adc8; font-size:0.8em;}</style></head>";
    html += "<body><div class='box'><h2>Configuration de la Radio</h2>";
    html += "<form action='/save' method='POST'>"; 
    
    html += "<label>Fuseau Horaire (POSIX):</label>";
    html += "<input type='text' name='tz' value='" + time_zone + "'>";
    
    html += "<hr style='border:0; border-top:1px solid #313244; margin:20px 0;'>";
    html += "<h3>Réseaux Wi-Fi Connus</h3>";
    
    html += "<label>SSID Réseau 1:</label>";
    html += "<input type='text' name='ssid_1' value='" + s1 + "'>";
    html += "<label>Mot de passe Réseau 1 (Texte clair):</label>";
    html += "<input type='password' name='pass_1' value=''>";
    html += "<small>Laisse vide pour conserver le mot de passe actuel.</small>";
    
    html += "<label>SSID Réseau 2:</label>";
    html += "<input type='text' name='ssid_2' value='" + s2 + "'>";
    html += "<label>Mot de passe Réseau 2 (Texte clair):</label>";
    html += "<input type='password' name='pass_2' value=''>";
    html += "<small>Laisse vide pour conserver le mot de passe actuel.</small>";
    
    html += "<label>SSID Réseau 3:</label>";
    html += "<input type='text' name='ssid_3' value='" + s3 + "'>";
    html += "<label>Mot de passe Réseau 3 (Texte clair):</label>";
    html += "<input type='password' name='pass_3' value=''>";
    html += "<small>Laisse vide pour conserver le mot de passe actuel.</small>";

    html += "<hr style='border:0; border-top:1px solid #313244; margin:20px 0;'>";
    html += "<label>radiointernet.m3u:</label><textarea name='m3u'>" + readSD("/radiointernet.m3u") + "</textarea>";
    html += "<label>favoris.m3u:</label><textarea name='fav'>" + readSD("/favoris.m3u") + "</textarea>";
    
    html += "<input type='submit' class='btn' value='Ecraser et Redemarrer'>";
    html += "</form></div></body></html>";
    server.send(200, "text/html", html); 
  });

  server.on("/save", HTTP_POST, []() {
    if (server.hasArg("tz")) time_zone = server.arg("tz");

    String oldCfg = readSD("/config.ini");

    String ssid1 = server.arg("ssid_1"); ssid1.trim();
    String pass1 = server.arg("pass_1"); pass1.trim();
    String psk1 = "";
    if (pass1.length() > 0) psk1 = generateWPA2_PSK(ssid1, pass1);
    else psk1 = getConfigValue(oldCfg, "PSK_1");

    String ssid2 = server.arg("ssid_2"); ssid2.trim();
    String pass2 = server.arg("pass_2"); pass2.trim();
    String psk2 = "";
    if (pass2.length() > 0) psk2 = generateWPA2_PSK(ssid2, pass2);
    else psk2 = getConfigValue(oldCfg, "PSK_2");

    String ssid3 = server.arg("ssid_3"); ssid3.trim();
    String pass3 = server.arg("pass_3"); pass3.trim();
    String psk3 = "";
    if (pass3.length() > 0) psk3 = generateWPA2_PSK(ssid3, pass3);
    else psk3 = getConfigValue(oldCfg, "PSK_3");

    String newConfig = "[RESEAU]\n";
    if (ssid1.length() > 0) {
      newConfig += "SSID_1=" + ssid1 + "\n";
      newConfig += "PSK_1=" + psk1 + "\n\n";
    }
    if (ssid2.length() > 0) {
      newConfig += "SSID_2=" + ssid2 + "\n";
      newConfig += "PSK_2=" + psk2 + "\n\n";
    }
    if (ssid3.length() > 0) {
      newConfig += "SSID_3=" + ssid3 + "\n";
      newConfig += "PSK_3=" + psk3 + "\n\n";
    }
    
    newConfig += "[SYSTEME]\n";
    newConfig += "FUSEAU_HORAIRE=" + time_zone + "\n";
    newConfig += "VOLUME_DEFAUT=" + String(currentVolume) + "\n";

    writeSD("/config.ini", newConfig);

    if (server.hasArg("m3u")) writeSD("/radiointernet.m3u", server.arg("m3u"));
    if (server.hasArg("fav")) writeSD("/favoris.m3u", server.arg("fav"));
    
    server.send(200, "text/html", "<html><body style='background:#1e1e2e; color:#a6e3a1; text-align:center; padding:50px;'><h2>Sauvegarde terminee. Reboot...</h2></body></html>");
    delay(1000); 
    ESP.restart(); 
  });
  
  server.begin(); 
}

// ==============================================================================
// 6. SOUS-ROUTINES GRAPHIQUES ET TACTILE
// ==============================================================================
bool getTouch(uint16_t &x, uint16_t &y) {
  // Le controleur tactile renvoie 12 bits par coordonnee, dans l'ordre X puis Y.
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x02); 
  if (Wire.endTransmission() != 0) return false; 
  
  Wire.requestFrom(TOUCH_I2C_ADDR, 5);
  if (Wire.available() >= 5) {
    uint8_t touches = Wire.read();
    uint8_t xh = Wire.read(); uint8_t xl = Wire.read();
    uint8_t yh = Wire.read(); uint8_t yl = Wire.read();
    
    if (touches > 0 && touches <= 2) {
      uint16_t touchX = ((xh & 0x0F) << 8) | xl; 
      uint16_t touchY = ((yh & 0x0F) << 8) | yl;
      
      // L'ecran est monte inversement par rapport au repere du controleur.
      x = 240 - touchX; 
      y = 320 - touchY; 
      return true; 
    }
  }
  return false;
}

void drawTopStatusBar() {
  // Affiche l'etat Wi-Fi et estime le niveau de batterie a partir de l'ADC.
  tft.fillRect(0, 0, 240, 20, TFT_DARKGREY); 
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM); 
  
  long rssi = WiFi.RSSI();
  String wifiStatus = "WiFi: ";
  if (WiFi.status() == WL_CONNECTED) {
    if (rssi > -60) wifiStatus += "Ex"; else if (rssi > -75) wifiStatus += "Bon"; else wifiStatus += "Fble";
  } else wifiStatus += "Deco";
  tft.drawString(wifiStatus, 5, 2, 2);

  float voltage = analogReadMilliVolts(BAT_ADC_PIN) * 2.0 / 1000.0;
  String batStatus = "";
  if (voltage > 4.3 || voltage < 2.5) batStatus = "Pwr: USB"; 
  else {
    int pct = map(voltage * 100, 320, 420, 0, 100); pct = constrain(pct, 0, 100); 
    batStatus = "Bat: " + String(pct) + "% (" + String(voltage, 1) + "V)";
  }
  tft.setTextDatum(TR_DATUM); tft.drawString(batStatus, 235, 2, 2); 
}

void drawBottomBar() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) return; 
  tft.fillRect(0, 300, 240, 20, TFT_DARKGREY); 
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  char timeStr[30]; strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
  String bottomText = String(timeStr) + "   |   IP: " + WiFi.localIP().toString();
  tft.drawString(bottomText, 120, 310, 2); 
}

void updateVolumeBar(int startY) {
  int barWidth = 90; int barHeight = 16;
  int startX = 75; 
  
  tft.drawRect(startX - 2, startY - 2, barWidth + 4, barHeight + 4, TFT_WHITE);
  int fillWidth = map(currentVolume, 0, 21, 0, barWidth);
  if (fillWidth > 0) tft.fillRect(startX, startY, fillWidth, barHeight, TFT_GREEN);
  if (fillWidth < barWidth) tft.fillRect(startX + fillWidth, startY, barWidth - fillWidth, barHeight, TFT_DARKGREY);
}

void drawMp3MetaBar() {
  tft.fillRect(0, 20, 240, 65, TFT_NAVY); 
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  
  String dTitle = metaTitle;
  if(dTitle.length() > 20) dTitle = dTitle.substring(0, 17) + "...";
  tft.drawString(dTitle, 120, 40, 2);
  
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  String subText = metaArtist;
  if (metaAlbum != "") subText += " - " + metaAlbum;
  if (subText.length() > 28) subText = subText.substring(0, 25) + "...";
  
  tft.drawString(subText, 120, 65, 1);
  metaUpdated = false; 
}

void updatePlaybackTime() {
  if (!isMp3Playing || currentState != MP3_PLAYER) return; 
  uint32_t currentSec = audio.getAudioCurrentTime();
  uint32_t totalSec = audio.getAudioFileDuration();
  
  char timeStr[20];
  sprintf(timeStr, "%02d:%02d / %02d:%02d", currentSec/60, currentSec%60, totalSec/60, totalSec%60);
  
  tft.fillRect(40, 93, 160, 15, TFT_BLACK); 
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(timeStr, 120, 100, 2);
}

void drawMainMenu() {
  tft.fillRect(0, 20, 240, 280, TFT_BLACK); 
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM); 
  tft.drawString("MENU PRINCIPAL", 120, 45, 2); 
  
  tft.fillRoundRect(20, 80, 200, 50, 4, TFT_DARKCYAN); 
  tft.drawString("RADIO FM", 120, 105, 2);   
  
  tft.fillRoundRect(20, 150, 200, 50, 4, TFT_DARKCYAN); 
  tft.drawString("RADIO INTERNET", 120, 175, 2);   
  
  tft.fillRoundRect(20, 220, 200, 50, 4, TFT_DARKCYAN); 
  tft.drawString("LECTEUR MP3", 120, 245, 2);   
}

void drawFMConstruction() {
  tft.fillRect(0, 20, 240, 280, TFT_BLACK);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("MODULE FM", 120, 100, 4);
  tft.drawString("En construction...", 120, 140, 2);
  
  tft.fillRoundRect(40, 240, 160, 40, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("RETOUR", 120, 260, 2);
}

void drawNetMenu() {
  tft.fillRect(0, 20, 240, 280, TFT_BLACK); 
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM); 
  tft.drawString("SOURCES INTERNET", 120, 50, 2); 
  
  tft.fillRoundRect(20, 90, 200, 50, 4, TFT_PURPLE); 
  tft.drawString("FAVORIS", 120, 115, 2);   
  
  tft.fillRoundRect(20, 160, 200, 50, 4, TFT_DARKCYAN); 
  tft.drawString("AUTRES STATIONS", 120, 185, 2);   
  
  tft.fillRoundRect(40, 240, 160, 40, 4, TFT_DARKGREY);
  tft.drawString("RETOUR", 120, 260, 2);
}

void drawStationList() {
  // Quatre stations sont visibles ; listOffset determine la page courante.
  tft.fillRect(0, 20, 240, 280, TFT_BLACK); 
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM); 
  tft.drawString("SELECTIONNER", 120, 35, 2); 
  
  for (int i = 0; i < 4; i++) {
    int actualIndex = listOffset + i;
    if (actualIndex >= stationCount) break; 
    
    int yPos = 60 + (i * 45); 
    
    tft.drawRoundRect(10, yPos, 175, 40, 4, TFT_DARKCYAN); 
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    
    String dName = stations[actualIndex].name;
    if(dName.length() > 14) dName = dName.substring(0, 11) + "...";
    tft.drawString(dName, 97, yPos + 20, 2);   

    bool fav = isFavorite(stations[actualIndex].url);
    tft.fillRoundRect(190, yPos, 40, 40, 4, fav ? TFT_GOLD : TFT_DARKGREY); 
    tft.setTextColor(fav ? TFT_BLACK : TFT_WHITE, fav ? TFT_GOLD : TFT_DARKGREY);
    tft.drawString("FAV", 210, yPos + 20, 1);
  }
  
  tft.fillRoundRect(10, 245, 60, 35, 4, TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.drawString("HAUT", 40, 262, 2);
  
  tft.fillRoundRect(80, 245, 80, 35, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("RETOUR", 120, 262, 2);
  
  tft.fillRoundRect(170, 245, 60, 35, 4, TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.drawString("BAS", 200, 262, 2);
}

void drawPlayer() {
  tft.fillRect(0, 20, 240, 280, TFT_BLACK);
  tft.fillRect(0, 20, 240, 60, TFT_NAVY);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  
  String sName = stations[currentStationIndex].name;
  if(sName.length() > 16) sName = sName.substring(0, 13) + "...";
  tft.drawString(sName, 120, 50, 4);

  tft.fillRoundRect(10, 100, 60, 40, 4, TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("VOL -", 40, 120, 2);

  tft.fillRoundRect(170, 100, 60, 40, 4, TFT_DARKGREEN);
  tft.drawString("VOL +", 200, 120, 2);

  updateVolumeBar(112); 

  bool fav = isFavorite(stations[currentStationIndex].url);
  tft.fillRoundRect(60, 170, 120, 40, 4, fav ? TFT_GOLD : TFT_DARKGREY);
  tft.setTextColor(fav ? TFT_BLACK : TFT_WHITE, fav ? TFT_GOLD : TFT_DARKGREY);
  tft.drawString(fav ? "- FAVORIS" : "+ FAVORIS", 120, 190, 2);

  tft.fillRoundRect(20, 230, 200, 40, 4, TFT_MAROON);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.drawString("ARRETER ET RETOUR", 120, 250, 2);
}

void drawMP3Player() {
  tft.fillRect(0, 20, 240, 280, TFT_BLACK);
  
  if (mp3Count == 0) resetMeta("Dossier Vide");
  drawMp3MetaBar(); 

  tft.fillRoundRect(10, 125, 60, 40, 4, TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("VOL -", 40, 145, 2);
  
  tft.fillRoundRect(170, 125, 60, 40, 4, TFT_DARKGREEN);
  tft.drawString("VOL +", 200, 145, 2);
  
  updateVolumeBar(137); 

  tft.fillRoundRect(10, 190, 60, 40, 4, TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.drawString("PREC", 40, 210, 2);

  tft.fillRoundRect(170, 190, 60, 40, 4, TFT_BLUE);
  tft.drawString("SUIV", 200, 210, 2);

  tft.fillRoundRect(80, 190, 80, 40, 4, isMp3Playing ? TFT_MAROON : TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, isMp3Playing ? TFT_MAROON : TFT_DARKGREEN);
  tft.drawString(isMp3Playing ? "STOP" : "JOUER", 120, 210, 2);

  updatePlaybackTime(); 

  tft.fillRoundRect(40, 250, 160, 40, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("RETOUR", 120, 270, 2);
}

void drawScreensaver() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) return; 
  tft.fillScreen(TFT_BLACK); 
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK); 
  tft.setTextDatum(MC_DATUM);
  char timeStr[10]; strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
  char dateStr[30]; strftime(dateStr, sizeof(dateStr), "%d/%m/%Y", &timeinfo);
  tft.setTextSize(2); tft.drawString(String(timeStr), 120, 130, 4); 
  tft.setTextSize(1); tft.drawString(String(dateStr), 120, 200, 4);
}

void refreshScreen() {
  // Redessine l'interface commune puis l'ecran correspondant a l'etat courant.
  drawTopStatusBar();
  drawBottomBar();
  switch(currentState) {
    case MAIN_MENU: drawMainMenu(); break;
    case FM_CONSTRUCTION: drawFMConstruction(); break;
    case NET_MENU: drawNetMenu(); break;
    case NET_STATIONS_LIST: drawStationList(); break;
    case NET_PLAYING: drawPlayer(); break;
    case MP3_PLAYER: drawMP3Player(); break;
    case SCREENSAVER: drawScreensaver(); break;
    default: break;
  }
}

// ==============================================================================
// 7. MULTITHREADING : TÂCHE ASYNCHRONE (Core 0)
// ==============================================================================
void TaskUI_Code(void *pvParameters) {
  // Cette tache centralise les acces a l'ecran, au tactile et au serveur web.
  for (;;) {
    server.handleClient(); 
    
    if (currentState == SETUP_AP) { vTaskDelay(10 / portTICK_PERIOD_MS); continue; }
    
    // Passe en veille apres une minute sans interaction.
    if (currentState != SCREENSAVER && (millis() - lastTouchTime > IDLE_TIMEOUT)) {
      previousState = currentState; 
      currentState = SCREENSAVER;
      drawScreensaver();
    }
    
    if (currentState != SCREENSAVER) {
      if (millis() - lastStatusUpdate > 5000) { drawTopStatusBar(); lastStatusUpdate = millis(); }
      if (millis() - lastTimeUpdate > 30000) { drawBottomBar(); lastTimeUpdate = millis(); }
      
      if (currentState == MP3_PLAYER) {
        if (autoNextTrack) {
          // Le callback EOF ne fait que poser un drapeau pour rester non bloquant.
          autoNextTrack = false; 
          if (mp3Count > 0) {
            currentMp3Index = (currentMp3Index + 1) % mp3Count; 
            resetMeta(mp3Files[currentMp3Index]); 
            String path = "/MP3s/" + mp3Files[currentMp3Index]; 
            
            digitalWrite(AP_ENABLE, HIGH); 
            audio.connecttoFS(SD_MMC, path.c_str()); 
            digitalWrite(AP_ENABLE, LOW); 
            
            isMp3Playing = true;
            refreshScreen(); 
          }
        }
        if (metaUpdated) drawMp3MetaBar(); 
        if (millis() - lastMp3TimeUpdate > 1000) { 
          updatePlaybackTime();
          lastMp3TimeUpdate = millis();
        }
      }
    } else {
      if (millis() - lastSaverUpdate > 60000) { drawScreensaver(); lastSaverUpdate = millis(); }
    }

    uint16_t tx, ty; 
    // Ignore les contacts trop rapproches pour eviter les doubles clics.
    if (getTouch(tx, ty) && millis() - lastTouchTime > 200) {
      lastTouchTime = millis(); 
      
      if (currentState == SCREENSAVER) {
        // Le premier contact reveille l'ecran sans declencher de commande.
        currentState = previousState; 
        refreshScreen();
        continue; 
      }
      
      if (currentState == MAIN_MENU) {
        if (ty > 80 && ty < 130) { currentState = FM_CONSTRUCTION; refreshScreen(); }      
        else if (ty > 150 && ty < 200) { currentState = NET_MENU; refreshScreen(); }       
        else if (ty > 220 && ty < 270) { 
          digitalWrite(AP_ENABLE, HIGH); 
          audio.stopSong(); 
          isMp3Playing = false;
          loadMP3Folder(); 
          currentState = MP3_PLAYER; 
          refreshScreen(); 
        } 
      }
      else if (currentState == FM_CONSTRUCTION) {
        if (ty > 240 && ty < 280 && tx > 40 && tx < 200) { currentState = MAIN_MENU; refreshScreen(); } 
      }
      else if (currentState == NET_MENU) {
        if (ty > 90 && ty < 140) { 
          inFavoritesMenu = true; 
          loadM3UList("/favoris.m3u"); 
          currentState = NET_STATIONS_LIST; 
          refreshScreen(); 
        } 
        else if (ty > 160 && ty < 210) { 
          inFavoritesMenu = false; 
          loadM3UList("/radiointernet.m3u"); 
          currentState = NET_STATIONS_LIST; 
          refreshScreen(); 
        } 
        else if (ty > 240 && ty < 280 && tx > 40 && tx < 200) { currentState = MAIN_MENU; refreshScreen(); } 
      }
      else if (currentState == NET_STATIONS_LIST) {
        if (ty >= 60 && ty <= 235) { 
          int clickedSlot = (ty - 60) / 45; 
          int buttonY = 60 + (clickedSlot * 45);
          
          if (ty >= buttonY && ty <= buttonY + 40) {
            int realIndex = listOffset + clickedSlot; 
            if (realIndex >= 0 && realIndex < stationCount) {
              if (tx > 185) { 
                // La colonne de droite est reservee au bouton favoris.
                toggleFavorite(stations[realIndex]);
                if (inFavoritesMenu) {
                  loadM3UList("/favoris.m3u");
                  if (listOffset >= stationCount && listOffset > 0) listOffset -= 4;
                }
                refreshScreen();
              } else {
                currentStationIndex = realIndex; currentState = NET_PLAYING; refreshScreen();                
                digitalWrite(AP_ENABLE, HIGH); 
                audio.connecttohost(stations[currentStationIndex].url.c_str()); 
                digitalWrite(AP_ENABLE, LOW); 
              }
            }
          }
        }
        else if (ty > 245 && ty < 280) { 
          if (tx < 70) { 
            if (listOffset > 0) { 
              listOffset -= 4; 
              if (listOffset < 0) listOffset = 0; 
              refreshScreen(); 
            }
          }
          else if (tx > 170) { 
            if (listOffset + 4 < stationCount) { 
              listOffset += 4; 
              refreshScreen(); 
            }
          }
          else if (tx >= 80 && tx <= 160) { currentState = NET_MENU; refreshScreen(); } 
        }
      }
      else if (currentState == NET_PLAYING) {
        if (ty > 170 && ty < 210 && tx > 60 && tx < 180) { 
          toggleFavorite(stations[currentStationIndex]);
          refreshScreen(); 
        }
        else if (ty > 230 && ty < 270 && tx > 20 && tx < 220) { 
          digitalWrite(AP_ENABLE, HIGH); 
          audio.stopSong(); 
          currentState = NET_MENU; 
          refreshScreen(); 
        } 
        else if (ty > 100 && ty < 140 && tx < 70) { if (currentVolume > 0) currentVolume--; audio.setVolume(currentVolume); updateVolumeBar(112); } 
        else if (ty > 100 && ty < 140 && tx > 170) { if (currentVolume < 21) currentVolume++; audio.setVolume(currentVolume); updateVolumeBar(112); } 
      }
      else if (currentState == MP3_PLAYER) {
        if (ty > 125 && ty < 165 && tx < 70) { if (currentVolume > 0) currentVolume--; audio.setVolume(currentVolume); updateVolumeBar(137); }
        else if (ty > 125 && ty < 165 && tx > 170) { if (currentVolume < 21) currentVolume++; audio.setVolume(currentVolume); updateVolumeBar(137); }
        
        else if (ty > 190 && ty < 230 && tx < 70) { 
          if (mp3Count > 0) {
            currentMp3Index = (currentMp3Index - 1 + mp3Count) % mp3Count; 
            if (isMp3Playing) { 
              digitalWrite(AP_ENABLE, HIGH); 
              audio.stopSong(); resetMeta(mp3Files[currentMp3Index]);
              String path = "/MP3s/" + mp3Files[currentMp3Index]; 
              audio.connecttoFS(SD_MMC, path.c_str()); 
              digitalWrite(AP_ENABLE, LOW); 
            }
            refreshScreen();
          }
        }
        else if (ty > 190 && ty < 230 && tx > 170) { 
          if (mp3Count > 0) {
            currentMp3Index = (currentMp3Index + 1) % mp3Count; 
            if (isMp3Playing) { 
              digitalWrite(AP_ENABLE, HIGH); 
              audio.stopSong(); resetMeta(mp3Files[currentMp3Index]);
              String path = "/MP3s/" + mp3Files[currentMp3Index]; 
              audio.connecttoFS(SD_MMC, path.c_str()); 
              digitalWrite(AP_ENABLE, LOW); 
            }
            refreshScreen();
          }
        }
        else if (ty > 190 && ty < 230 && tx >= 80 && tx <= 160) { 
          if (mp3Count > 0) {
            if (isMp3Playing) { 
              digitalWrite(AP_ENABLE, HIGH); 
              audio.stopSong(); 
              isMp3Playing = false; 
            }
            else { 
              resetMeta(mp3Files[currentMp3Index]);
              String path = "/MP3s/" + mp3Files[currentMp3Index]; 
              digitalWrite(AP_ENABLE, HIGH); 
              audio.connecttoFS(SD_MMC, path.c_str()); 
              digitalWrite(AP_ENABLE, LOW); 
              isMp3Playing = true; 
            }
            refreshScreen();
          }
        }
        else if (ty > 250 && ty < 290 && tx > 40 && tx < 200) { 
          digitalWrite(AP_ENABLE, HIGH); 
          audio.stopSong(); 
          isMp3Playing = false; 
          currentState = MAIN_MENU; 
          refreshScreen();
        }
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); 
  }
}

// ==============================================================================
// 8. SETUP INITIAL (Exécuté au démarrage sur le Core 1)
// ==============================================================================
void setup() {
  // Initialise le materiel, charge la configuration, puis choisit STA ou AP.
  Serial.begin(115200);
  pinMode(BAT_ADC_PIN, INPUT);

  tft.init(); tft.setRotation(2); 
  tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM); 
  
  tft.drawString("Boot Materiel...", 120, 160, 2);

  pinMode(AP_ENABLE, OUTPUT); 
  digitalWrite(AP_ENABLE, HIGH); 
  
  pinMode(TOUCH_RST, OUTPUT); digitalWrite(TOUCH_RST, LOW); delay(10); 
  digitalWrite(TOUCH_RST, HIGH); delay(50);
  Wire.begin(I2C_SDA, I2C_SCL, I2C_SPEED);

  if (es8311_codec_init() != ESP_OK) return; 
  es8311_handle_t mon_codec = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  es8311_voice_volume_set(mon_codec, 75, NULL); 

  String configuredSsids[3] = {"", "", ""};
  String configuredPsks[3] = {"", "", ""};

  // La carte SD contient les reseaux, le fuseau horaire et les listes M3U.
  if (SD_MMC.setPins(SD_SCK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3) && SD_MMC.begin()) {
    File configFile = SD_MMC.open("/config.ini");
    if (configFile) {
      while (configFile.available()) {
        String line = configFile.readStringUntil('\n'); line.trim();
        if (line.length() == 0 || line.startsWith("[") || line.startsWith("#")) continue; 
        
        int splitIndex = line.indexOf('=');
        if (splitIndex > 0) {
          String key = line.substring(0, splitIndex); String value = line.substring(splitIndex + 1);
          key.trim(); value.trim();
          
          if (key == "FUSEAU_HORAIRE") time_zone = value;
          else if (key == "VOLUME_DEFAUT") currentVolume = value.toInt();
          else if (key == "SSID_1") { configuredSsids[0] = value; }
          else if (key == "PSK_1")  { configuredPsks[0] = value; }
          else if (key == "SSID_2") { configuredSsids[1] = value; }
          else if (key == "PSK_2")  { configuredPsks[1] = value; }
          else if (key == "SSID_3") { configuredSsids[2] = value; }
          else if (key == "PSK_3")  { configuredPsks[2] = value; }
        }
      }
      configFile.close();
    }
  }

  tft.fillScreen(TFT_BLACK);
  tft.drawString("Scan des réseaux...", 120, 160, 2); 
  WiFi.mode(WIFI_STA); 
  WiFi.setTxPower(WIFI_POWER_5dBm); 
  WiFi.setSleep(false);             

  // On scanne d'abord pour eviter d'attendre dix tentatives si aucun reseau connu n'est present.
  int n = WiFi.scanNetworks(false, true); 
  bool foundMatchingNetwork = false;

  for (int i = 0; i < n; ++i) {
    String scannedSsid = WiFi.SSID(i);
    for (int j = 0; j < 3; j++) {
      if (configuredSsids[j].length() > 0 && scannedSsid == configuredSsids[j]) {
        foundMatchingNetwork = true;
        break;
      }
    }
    if (foundMatchingNetwork) break;
  }
  WiFi.scanDelete(); 

  bool connected = false;

  // Sans reseau disponible, la radio devient un point d'acces de configuration.
  if (!foundMatchingNetwork) {
    connected = false; 
  } else {
    for (int j = 0; j < 3; j++) {
      if (configuredSsids[j].length() > 0 && configuredPsks[j].length() > 0) {
        wifiMulti.addAP(configuredSsids[j].c_str(), configuredPsks[j].c_str());
      }
    }

    tft.fillScreen(TFT_BLACK);
    tft.drawString("Connexion en cours...", 120, 160, 2);

    int attempts = 0;
    while (wifiMulti.run() != WL_CONNECTED && attempts < 10) { 
      delay(500); 
      attempts++; 
    }
    if (WiFi.status() == WL_CONNECTED) connected = true;
  }

  setupWebServer(); 

  if (!connected) {
    currentState = SETUP_AP;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Radio_Config"); 
    
    tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Config Réseau Requise", 120, 60, 2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Connecte-toi au Wi-Fi:", 120, 120, 2);
    tft.drawString("Radio_Config", 120, 150, 2); 
    tft.drawString("Va a l'adresse:", 120, 200, 2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("192.168.4.1", 120, 230, 2);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Sync Horloge NTP...", 120, 160, 2); 
    configTzTime(time_zone.c_str(), "pool.ntp.org"); delay(1000); 

    audio.setPinout(I2S_BCK, I2S_WS, I2S_DOUT, I2S_MCK);
    audio.setVolume(currentVolume); 

    refreshScreen(); 
  }

  xTaskCreatePinnedToCore(TaskUI_Code, "TaskUI", 10000, NULL, 1, &TaskUI_Handle, 0);
}

// ==============================================================================
// 9. BOUCLE PRINCIPALE (Core 1)
// ==============================================================================
void loop() {
  // La lecture audio doit tourner en continu sur le coeur principal.
  if (currentState != SETUP_AP) audio.loop();
}