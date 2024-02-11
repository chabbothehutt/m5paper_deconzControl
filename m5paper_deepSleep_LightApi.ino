#include <M5EPD.h>
#include "WiFi.h"
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include "esp_wifi.h"

const uint64_t uS_TO_S_FACTOR = 1000000; /* Conversion factor for micro seconds to seconds */
const char timeApiPath[] PROGMEM = "https://www.timeapi.io/api/Time/current/zone?timeZone=Europe/Amsterdam";

#define _PROTOCOL http:/
#define _HOST <HOST:PORT>
#define _URIPREFIX api/<YOUR API-KEY HERE>

// Macros for creating a combined endpoint.
#define STR_HELPER(x) #x
#define STRINGER(x) STR_HELPER(x)
#define URL(PATH) STRINGER(_PROTOCOL/_HOST/_URIPREFIX/PATH)

#define REG_ENDPOINTPATH(endpointName, endpointPath) \
  const char endpointName ## Path [] PROGMEM { URL(endpointPath) };

REG_ENDPOINTPATH(getLightGroups, groups);
REG_ENDPOINTPATH(getScenesOfGrp, groups/%d/scenes);
REG_ENDPOINTPATH(putActivateLightScenes, groups/%d/scenes/%d/recall);
REG_ENDPOINTPATH(putSetGroupState, groups/%d/action);

#define INITIAL_GRP 1

const char SSID[] PROGMEM = "";
const char WIFIPWD[] PROGMEM = "";
const char BATT_WARNING[] PROGMEM = "Low battery!";
const char STATICBTNS[][8]{ { "Toggle" }, { "Back" } };
const int ANZ_STATICBTNS PROGMEM = sizeof(STATICBTNS) / sizeof(STATICBTNS[0]);

const int TEXT_SIZE = 2;
const int SCREEN_WIDTH = 540;
const int SCREEN_HEIGHT = 960;
const int SBAUD = 19200;

#define ROTATION 90
#if ROTATION == 90
#define RWIDTH SCREEN_WIDTH
#define RHEIGHT SCREEN_HEIGHT
#else
#define RWIDTH SCREEN_HEIGHT
#define RHEIGHT SCREEN_WIDTH
#endif

const int UI_BTN_HEIGHT = 69;
const int UI_BTN_YMARGIN = 20;
const int UI_BTN_XPADDING = 15;
const int UI_BTN_RADIUS = 5;
const int UI_FRAME_XOFFSET = 20;
const int UI_FRAME_YOFFSET = 15;

//assumptional max-values - adjustable to eventual needs
const int NAMELEN = 64;
const int MAX_GROUPS = 16;
const int MAX_SCENES_PER_GRP = 16;

static M5EPD_Canvas canvas(&M5.EPD);
static esp_sleep_wakeup_cause_t wakeup_reason;

static rtc_time_t RTCtime;  //initialization through M5-API (in setup), threfore no Attrubute
static rtc_date_t RTCDate;
RTC_DATA_ATTR static int s_GroupCount = 0;
RTC_DATA_ATTR static int s_GroupIds[MAX_GROUPS];
RTC_DATA_ATTR static char s_Groups[MAX_GROUPS][NAMELEN];
RTC_DATA_ATTR static char s_SelScenes[MAX_SCENES_PER_GRP][NAMELEN];
RTC_DATA_ATTR static int s_aktScenesCount = 0;
RTC_DATA_ATTR static int s_aktGrpIndex = -1;
RTC_DATA_ATTR static int SelectedGroup = -1;
RTC_DATA_ATTR static bool bIsLowBattery = false;

static int debnum = 1;
inline static void ACHTUNG(int startNum = -1) {
  if (startNum != -1)
    debnum = startNum;
  Serial.printf("ACHTUNG <%d> ACHTUNG\n", debnum);
  ++debnum;
  delay(150);
}

static bool bIsShowingBattWarning = false;
static void updateBattWarning() {
  if (bIsLowBattery == false) {
    int battV = M5.getBatteryVoltage();
    if (battV < 3400) {
      Serial.printf("at %dmV - showing batt-warning\n", battV);
      bIsLowBattery = true;
    }
  }
  if (bIsLowBattery) {
    canvas.setTextSize(TEXT_SIZE + 1);  //Set that text size.
    const int textWidth = canvas.textWidth(BATT_WARNING);
    const int xStart = (RWIDTH - textWidth) / 2;
    canvas.drawString(BATT_WARNING, xStart, 900);
    canvas.setTextSize(TEXT_SIZE);
    canvas.setFreeFont(&FreeSansBoldOblique12pt7b);
    bIsShowingBattWarning = true;
    M5.EPD.WriteFullGram4bpp((uint8_t*)canvas.frameBuffer());
    M5.EPD.UpdateArea(xStart - 15, 875, textWidth + 30, 60, UPDATE_MODE_DU4);
  }
}
#define isUndef(reason) (reason < 2 || reason > 6)
static void print_wakeup_reason(esp_sleep_wakeup_cause_t& reason) {
  reason = esp_sleep_get_wakeup_cause();
  switch (reason) {
    case ESP_SLEEP_WAKEUP_EXT0: Serial.println("Wakeup caused by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1: Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Wakeup caused by timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("Wakeup caused by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP: Serial.println("Wakeup caused by ULP program"); break;
    default: Serial.printf("Wakeup was not caused by deep sleep: %d\n", reason); break;
  }
}

static void initClock() {
  int timeapiResponse = 0;
  HTTPClient http;
  if (http.begin(timeApiPath)) {
    timeapiResponse = http.GET();
    if (timeapiResponse == 200) {
      JSONVar timapiResult = JSON.parse(http.getString());
      RTCtime.hour = (int)timapiResult["hour"];
      RTCtime.min = (int)timapiResult["minute"];
      RTCtime.sec = (int)timapiResult["seconds"] + 1;
      RTCDate.year = (int)timapiResult["year"];
      RTCDate.mon = (int)timapiResult["month"];
      RTCDate.day = (int)timapiResult["day"];
    }
  }
  if (timeapiResponse != 200) {
    RTCtime.hour = 00;
    RTCtime.min = 00;
    RTCtime.sec = 00;
    RTCDate.year = 1990;
    RTCDate.mon = 01;
    RTCDate.day = 01;
  }
  M5.RTC.setDate(&RTCDate);
  M5.RTC.setTime(&RTCtime);
}

RTC_DATA_ATTR static int sleepInfoXYWH[4];

static void removeSleepInfo() {
  Serial.println("Remove \"zZZ\"");
  M5.EPD.WriteFullGram4bpp((uint8_t*)canvas.frameBuffer());
  M5.EPD.UpdateArea(sleepInfoXYWH[0], sleepInfoXYWH[1], sleepInfoXYWH[2], sleepInfoXYWH[3], UPDATE_MODE_DU);
  delay(300);
}

static void initSleep(const int& seconds, const bool& bDrawzZZ = true, const bool& flush_screen = false) {
  char sleepInfoBuffer[13];
  M5.RTC.getTime(&RTCtime);
  M5.RTC.getDate(&RTCDate);

  sprintf(sleepInfoBuffer, "%02d.%02d. %02d:%02d", RTCDate.day, RTCDate.mon, RTCtime.hour, RTCtime.min);
  canvas.setFreeFont(&FreeSansOblique12pt7b);
  canvas.setTextSize(TEXT_SIZE - 1);
  const int txtWidth = canvas.textWidth(sleepInfoBuffer);
  const int txtHeight = canvas.fontHeight();
  int zzzHeight = txtHeight;

  const int txtXOffset = RWIDTH  - (txtWidth + UI_FRAME_XOFFSET);
  const int timeYOffset= RHEIGHT - UI_BTN_YMARGIN - txtHeight;

  canvas.drawString(sleepInfoBuffer, txtXOffset, timeYOffset);
  int zzzYOffset = 0;
  if (bDrawzZZ || flush_screen)
  {
    canvas.setFreeFont(&FreeSansBoldOblique12pt7b);
    zzzHeight = canvas.fontHeight();
    zzzYOffset = timeYOffset - zzzHeight - 3;
    canvas.drawString("z Z Z", txtXOffset, zzzYOffset);
  }
  
  canvas.setTextSize(TEXT_SIZE);
  canvas.setFreeFont(&FreeSans12pt7b);
  
  M5.enableEPDPower();
  if (flush_screen) {
    canvas.pushCanvas(0, 0, UPDATE_MODE_DU4);
    delay(500);
  } else {
    const int RenderXOffset = 3;
    const int RenderYOffset = 1;
    M5.EPD.WriteFullGram4bpp((uint8_t*)canvas.frameBuffer());
    if (bDrawzZZ)
      M5.EPD.UpdateArea(txtXOffset - RenderXOffset, zzzYOffset - RenderYOffset, txtWidth + RenderXOffset, zzzHeight + 3 + RenderYOffset, UPDATE_MODE_GLR16);
    M5.EPD.UpdateArea(txtXOffset - RenderXOffset, timeYOffset - RenderYOffset, txtWidth + RenderXOffset, txtHeight + RenderYOffset, UPDATE_MODE_GLR16);
    sleepInfoXYWH[0] = txtXOffset - RenderXOffset;
    sleepInfoXYWH[1] = zzzYOffset - RenderYOffset;
    sleepInfoXYWH[2] = txtWidth + RenderXOffset;
    sleepInfoXYWH[3] = zzzHeight + 3 + txtHeight + RenderYOffset;
  }
  delay(600);
  Serial.printf("%s%s%s\n", flush_screen ? "-flush- " : "", bDrawzZZ ? "zZZ " : "", sleepInfoBuffer);
  M5.disableEPDPower();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, LOW);  // TOUCH_INT
  esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);
  gpio_hold_en(GPIO_NUM_2);  // M5EPD_MAIN_PWR_PIN
  gpio_deep_sleep_hold_en();

  esp_deep_sleep_start();
}

static void initWifi() {
  if (WiFi.status() == WL_CONNECTED)
    return;
  Serial.println("connecting wifi");
  WiFi.begin(SSID, WIFIPWD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  Serial.println("connected");
}
static void disconnectWifi() {
  Serial.println("Disconnecting Wifi");
  WiFi.disconnect();
}

static int addGroup(const int& id, const char* sGrpNam) {
  if (s_GroupCount == MAX_GROUPS)
    return -1;
  s_GroupIds[s_GroupCount] = id;
  strlcpy(s_Groups[s_GroupCount], sGrpNam, strlen(sGrpNam) + 1);
  ++s_GroupCount;
  return s_GroupCount - 1;
}

static char s_Scenes[MAX_GROUPS][MAX_SCENES_PER_GRP][NAMELEN];
static int s_ScenesCount[MAX_GROUPS];

static bool s_scenesAvailableForGrp[MAX_GROUPS];
static bool addScene(int GrpIndex, const char* sSceneNam) {
  if (s_ScenesCount[GrpIndex] == MAX_SCENES_PER_GRP)
    return false;

  s_scenesAvailableForGrp[GrpIndex] = true;
  strlcpy(s_Scenes[GrpIndex][s_ScenesCount[GrpIndex]], sSceneNam, strlen(sSceneNam) + 1);
  ++s_ScenesCount[GrpIndex];
  return true;
}

static int lastTouch[2];

static bool check_touchUpdate(tp_finger_t& FingerItem) {
  while (M5.TP.available() == false) {
    delay(50);
  }
  M5.TP.update();
  FingerItem = M5.TP.readFinger(0);
  return (lastTouch[0] != FingerItem.x || lastTouch[1] != FingerItem.y);
}
/* always contains screen-updates */
static void FingerCallback_Grps(const int& clickIndex) {
  if (clickIndex > s_GroupCount - 1)
    SelectedGroup = s_GroupCount - 1;
  else
    SelectedGroup = clickIndex;
  Serial.printf("=> SelGrp: %d\n", SelectedGroup);
  if (s_scenesAvailableForGrp[SelectedGroup] == false) {
    Serial.println("fetching Scenes");
    initWifi();
    fetchScenes(SelectedGroup);
    disconnectWifi();
  }
  drawData();
}

/* returns wether there was a screen-update as a result of finger-interaction */
static bool FingerCallback_Scns(const int& clickIndex) {
  Serial.printf("=> Selected Index: %d - Contentcount: %d\n", clickIndex, s_ScenesCount[SelectedGroup]);
  if (clickIndex >= s_ScenesCount[SelectedGroup] + 1)  // "BACK"
  {
    SelectedGroup = -1;
    drawData();
    return true;
  }
  else if (clickIndex < 0)
    return false;

  initWifi();
  HTTPClient http;
  if (clickIndex == s_ScenesCount[SelectedGroup])  //TOGGLE
  {
    char togglePathBuffer[78];
    sprintf_P(togglePathBuffer, putSetGroupStatePath, s_GroupIds[SelectedGroup]);
    Serial.println(togglePathBuffer);
    if (http.begin(togglePathBuffer)) {
      int putResult = http.PUT("{ \"toggle\" : true }");
      Serial.printf("toggle status: %d\n", putResult);
    }
    else
    {
      Serial.printf("couldnt open connection to %s => d = %d\n", putSetGroupStatePath, s_GroupIds[SelectedGroup]);
    }
  } else {
    char pathBuffer[78];
    sprintf_P(pathBuffer, putActivateLightScenesPath, s_GroupIds[SelectedGroup], clickIndex + 1);
    Serial.println(pathBuffer);
    if (http.begin(pathBuffer)) {
      int putResult = http.PUT("");
      Serial.printf("recall scene status: %d\n", putResult);
    }
    else
    {
      Serial.printf("couldnt open connection to %s => d1 = %d, d2 = %d\n", putActivateLightScenesPath, s_GroupIds[SelectedGroup], clickIndex + 1);
    }
  }
  disconnectWifi();
  return false;
}

/* returns wether there was a screen-update as a result of finger-interaction */
static bool FingerCallback(tp_finger_t& FingerItem)
{
  const int indexClicked = (int)floor((FingerItem.y - UI_FRAME_YOFFSET) / (UI_BTN_HEIGHT + UI_BTN_YMARGIN));
  Serial.printf("Click-X: %d Y: %d sel-index: %d\n", FingerItem.x, FingerItem.y, indexClicked);

  lastTouch[0] = FingerItem.x;
  lastTouch[1] = FingerItem.y;

  if (SelectedGroup == -1) 
  {
    FingerCallback_Grps(indexClicked);
    return true;
  } 
  else
    return FingerCallback_Scns(indexClicked - 1); //The header is in place of the first button and therefore needs to be subtracted.
}

static bool lightSleepWaitForTouch(const int& waitSeconds = 15) {
  Serial.println("trying to doze off!");
  delay(50);

  esp_sleep_enable_timer_wakeup(waitSeconds * uS_TO_S_FACTOR);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, LOW);  // TOUCH_INT

  esp_wifi_stop();
  esp_light_sleep_start();
  esp_wifi_start();

  print_wakeup_reason(wakeup_reason);
  return wakeup_reason != ESP_SLEEP_WAKEUP_TIMER;
}

static void fetchGroups() {
  HTTPClient http;
  Serial.println("query-path: ");
  Serial.println(getLightGroupsPath);
  if (http.begin(getLightGroupsPath)) {
    int ligtApiResponse = http.GET();
    Serial.printf("light-api response: %d\n", ligtApiResponse);
    if (ligtApiResponse == 200) {
      JSONVar lightApiResult = JSON.parse(http.getString());
      JSONVar apiResult_KeySet = lightApiResult.keys();
      Serial.printf("response-key-count: %d\n", apiResult_KeySet.length());
      for (int i = 0; i < apiResult_KeySet.length(); ++i) {
        const char* keyNam = apiResult_KeySet[i];
        Serial.printf("idx <%s> ", keyNam);
        JSONVar group = lightApiResult[keyNam];
        if (group["lights"].length() == 0) {
          Serial.println();
          continue;
        }
        const char* grpNam = group["name"];
        Serial.printf("name <%s>\n", grpNam);
        int iGrpIdx = addGroup(atoi(keyNam), grpNam);
        if (iGrpIdx == -1)
          break;  //MAX_GROUPS hit -> stop there
        JSONVar scenes = group["scenes"];
        for (int p = 0; p < scenes.length(); ++p) {
          JSONVar scene = scenes[p];
          const char* sceneNam = scene["name"];
          Serial.printf("sc %d name <%s>\n", p + 1, sceneNam);
          if (addScene(iGrpIdx, sceneNam) == false)  //MAX_SCENES hit -> next group
            break;
        }
      }
    }
  }
}

static void fetchScenes(int GrpIndex) {
  char pathBuffer[64];
  sprintf_P(pathBuffer, getScenesOfGrpPath, GrpIndex + 1);
  HTTPClient http;
  Serial.println(pathBuffer);
  if (http.begin(pathBuffer)) {
    int respCode = http.GET();
    Serial.printf("GET Scenes response: %d\n", respCode);
    if (respCode == 200) {
      String respSTr = http.getString();
      Serial.println(respSTr);
      JSONVar lightApiResult = JSON.parse(respSTr);
      if (lightApiResult.length() == 0)
        return;
      JSONVar apiResult_KeySet = lightApiResult.keys();
      Serial.printf("found %d scenes\n", apiResult_KeySet.length());
      for (int i = 0; i < apiResult_KeySet.length(); ++i) {
        const char* sceneId = apiResult_KeySet[i];
        JSONVar scene = lightApiResult[sceneId];
        const char* sceneNam = scene["name"];
        Serial.printf("adding %s\n", sceneNam);
        if (addScene(GrpIndex, sceneNam) == false)
          break;
      }
    }
  }
}

static void drawButton(const char* BtnLabelTxt, int& yOffset) {
  Serial.println(BtnLabelTxt);
  int txtWidth = canvas.drawString(BtnLabelTxt, UI_FRAME_XOFFSET + UI_BTN_XPADDING, yOffset + UI_BTN_HEIGHT / 4);
  canvas.drawRoundRect(UI_FRAME_XOFFSET, yOffset, UI_FRAME_XOFFSET + txtWidth + UI_BTN_XPADDING, UI_BTN_HEIGHT, UI_BTN_RADIUS, 15);
  yOffset += UI_BTN_HEIGHT + UI_BTN_YMARGIN;
}

static void drawGroups() {
  canvas.fillCanvas(0);
  if (bIsShowingBattWarning)
    bIsShowingBattWarning = false;

  Serial.print("Groups: ");
  Serial.println(s_GroupCount);
  int yOffset = UI_FRAME_YOFFSET;
  for (int iGrpIdx = 0; iGrpIdx != s_GroupCount; ++iGrpIdx) {
    drawButton(s_Groups[iGrpIdx], yOffset);
  }
}

static void pushActiveGroup(const int& GrpIndex)
{
  s_aktScenesCount = s_ScenesCount[GrpIndex];
  s_aktGrpIndex = GrpIndex;
  for (int i = 0; i < s_ScenesCount[GrpIndex]; ++i) {
    strlcpy(s_SelScenes[i], s_Scenes[GrpIndex][i], strlen(s_Scenes[GrpIndex][i]) + 1);
  }
}

static void drawScenes(const int& GrpIndex) {
  if (s_aktGrpIndex != GrpIndex) {  //put selected Scene into RtcCache
    pushActiveGroup(GrpIndex);
  }

  canvas.fillCanvas(0);
  if (bIsShowingBattWarning)
    bIsShowingBattWarning = false;

  char pgTitleBuffer[strlen(s_Groups[GrpIndex]) + 2];
  sprintf(pgTitleBuffer, "%s :", s_Groups[GrpIndex]);
  canvas.setFreeFont(&FreeSansBold12pt7b);
  canvas.drawString(pgTitleBuffer, UI_FRAME_XOFFSET, UI_FRAME_YOFFSET);
  canvas.setFreeFont(&FreeSans12pt7b);
  Serial.printf("%s - Scenarios: %d\n", s_Groups[GrpIndex], s_ScenesCount[GrpIndex]);
  
  int yOffset = UI_FRAME_YOFFSET + UI_BTN_HEIGHT + UI_BTN_YMARGIN; //skip space of first button for header-line
  for (int iScnIdx = 0; iScnIdx != s_ScenesCount[GrpIndex]; ++iScnIdx) {
    drawButton(s_SelScenes[iScnIdx], yOffset);
  }

  for (int iBtnIndex = 0; iBtnIndex != ANZ_STATICBTNS; ++iBtnIndex) {
    drawButton(STATICBTNS[iBtnIndex], yOffset);
  }
}

static void drawData() {
  M5.enableEPDPower();
  if (SelectedGroup == -1) {
    drawGroups();
  } else {
    drawScenes(SelectedGroup);
  }
  if (!bIsShowingBattWarning) {
    updateBattWarning();
  }
  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);  //Update that screen.
  delay(700);
  M5.disableEPDPower();
}

void initDevice() {
  M5.begin();  //Init M5Paper.
  Serial.begin(SBAUD);
  Serial.println("Hello Serial ");

  M5.EPD.SetRotation(ROTATION);  //Set the rotation of the display.
  M5.TP.SetRotation(ROTATION);
  canvas.createCanvas(RWIDTH, RHEIGHT);  //Create a canvas.
  canvas.setTextSize(TEXT_SIZE);         //Set that text size.
  canvas.setFreeFont(&FreeSans12pt7b);
  M5.RTC.begin();
  WiFi.mode(WIFI_STA);
  print_wakeup_reason(wakeup_reason);

  for (int i = 0; i < MAX_GROUPS; ++i) {
    s_scenesAvailableForGrp[i] = false;
  }
}

void setup() {
  initDevice();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER)
  {
    updateBattWarning();
    initSleep(5400, false);
  }
  else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0)
    removeSleepInfo();
  M5.disableEPDPower();
  Serial.printf("Group-Anz: %d Selected: %d\n", s_GroupCount, SelectedGroup);
  tp_finger_t fingerItm;
  bool bScreenUpdate = false;
  if (s_GroupCount == 0)  //First boot
  {
    initWifi();
    initClock();
    fetchGroups();
    disconnectWifi();
#ifdef INITIAL_GRP
    SelectedGroup = INITIAL_GRP;
#endif
    drawData();
  } 
  else
  {
    if (s_aktGrpIndex != -1 && s_aktScenesCount != 0)  //later-boots
    {
      for (int i = 0; i < s_aktScenesCount; ++i) 
      {
        Serial.printf("scene from rtc: %s\n", s_SelScenes[i]);
        addScene(s_aktGrpIndex, s_SelScenes[i]); //from rtc to ram
      }
    }
    if (check_touchUpdate(fingerItm)) 
      bScreenUpdate = FingerCallback(fingerItm);
  }

  for (int i = 0; i < 5; ++i)
  {
    if (lightSleepWaitForTouch(45)) 
    {
      if (check_touchUpdate(fingerItm))
      {
        bScreenUpdate = FingerCallback(fingerItm);
        i = -1;
      }
    }
    else
      break;
  }
  Serial.println("should go sleeping");
  initSleep(5400, true, false);
}

void loop() {}