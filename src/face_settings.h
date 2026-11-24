#pragma once

// Catalog IDs are a versioned BLE contract, independent of screen orientation.
enum DashboardApp : uint8_t { FaceWeather, FaceStock, FaceClock, FaceRadio, FaceFocus, FaceLumo, FaceSports, FaceNotifications };
// Separate key preserves the existing v1 settings blob and all saved choices.
static uint8_t sportsSelections[6]={0,1,2,3,4,5};
struct FaceSettings {
  uint32_t version=1;
  uint8_t clock[6]={0,1,2,3,4,5};
  uint8_t weather[6]={0,1,2,3,4,5};
  uint8_t radio[6]={0,1,2,3,4,5};
  uint8_t dashboard[6]={FaceWeather,FaceStock,FaceClock,FaceRadio,FaceFocus,FaceLumo};
  char stocks[6][13]={"TSLA","GOOGL","AMZN","MSFT","NVDA","AAPL"};
};
static FaceSettings faceSettings;
static void applyWorldFaces();
static void loadWorldFaces();
static bool dashboardHas(uint8_t app) {
  for(uint8_t face=0;face<6;++face)if(faceSettings.dashboard[face]==app)return true;
  return false;
}
static bool dashboardIs(uint8_t orientation,uint8_t app) {
  return orientation>=1 && orientation<=6 && faceSettings.dashboard[orientation-1]==app;
}
static bool validTicker(const char* symbol) {
  size_t n=strlen(symbol);if(!n || n>12)return false;
  for(size_t i=0;i<n;++i)if(!((symbol[i]>='A'&&symbol[i]<='Z') || (symbol[i]>='0'&&symbol[i]<='9') || symbol[i]=='.' || symbol[i]=='-'))return false;
  return true;
}
static bool validFaceSettings(const FaceSettings& s) {
  if(s.version!=1)return false;
  for(int i=0;i<6;++i)if(s.clock[i]>=6 || s.weather[i]>=6 || s.radio[i]>=sizeof(RADIO_CATALOG)/sizeof(RADIO_CATALOG[0]) || s.dashboard[i]>FaceNotifications ||
      !memchr(s.stocks[i],0,sizeof(s.stocks[i])) || !validTicker(s.stocks[i]))return false;
  return true;
}
static void applyFaceCatalogs() {
  for(int i=0;i<6;++i){CLOCK_REGIONS[i]=CLOCK_CATALOG[faceSettings.clock[i]];
    WEATHER_CITIES[i]=WEATHER_CATALOG[faceSettings.weather[i]];
    RADIO_STATIONS[i]=RADIO_CATALOG[faceSettings.radio[i]];
    STOCKS[i]={faceSettings.stocks[i],faceSettings.stocks[i]};}
  applyWorldFaces();
}
static void loadFaceSettings() {
  Preferences storage;FaceSettings saved;
  if(storage.begin("face_config",true)) {
    if(storage.getBytesLength("v1")==sizeof(saved) && storage.getBytes("v1",&saved,sizeof(saved))==sizeof(saved) && validFaceSettings(saved))faceSettings=saved;
    uint8_t picks[6];
    if(storage.getBytesLength("sports")==sizeof(picks) && storage.getBytes("sports",picks,sizeof(picks))==sizeof(picks)) {
      bool valid=true;for(auto pick:picks)if(pick>=6)valid=false;
      if(valid)memcpy(sportsSelections,picks,sizeof(picks));
    }
    storage.end();
  }
  applyFaceCatalogs();
  loadWorldFaces();
}
// BLE callback only validates and copies a bounded command. Main loop owns
// NVS, renderers, audio shutdown, and updates to the active settings.
static portMUX_TYPE faceSettingsMux=portMUX_INITIALIZER_UNLOCKED;
static bool faceCommandPending=false;
static char faceCommandApp[12]={};
static char faceCommandValues[6][13]={};
static bool faceCommandWrite=false;
static bool queueFaceCommand(JsonDocument& request,bool write) {
  const char* app=request["app"]|"";
  if(strcmp(app,"clock") && strcmp(app,"weather") && strcmp(app,"stock") && strcmp(app,"radio") && strcmp(app,"dashboard") && strcmp(app,"sports"))return false;
  char values[6][13]={};
  if(write) {
    JsonArray array=request["values"].as<JsonArray>();if(array.size()!=6)return false;
    for(int i=0;i<6;++i) {
      if(!strcmp(app,"stock")) {
        if(!array[i].is<const char*>() || !validTicker(array[i].as<const char*>()))return false;
        strlcpy(values[i],array[i].as<const char*>(),13);
      } else {
        int limit=!strcmp(app,"radio")?sizeof(RADIO_CATALOG)/sizeof(RADIO_CATALOG[0]):!strcmp(app,"dashboard")?FaceNotifications+1:6;
        if(!array[i].is<int>() || array[i].as<int>()<0 || array[i].as<int>()>=limit)return false;
        snprintf(values[i],13,"%d",array[i].as<int>());
      }
    }
  }
  portENTER_CRITICAL(&faceSettingsMux);
  if(faceCommandPending){portEXIT_CRITICAL(&faceSettingsMux);return false;}
  strlcpy(faceCommandApp,app,sizeof(faceCommandApp));memcpy(faceCommandValues,values,sizeof(values));faceCommandWrite=write;faceCommandPending=true;
  portEXIT_CRITICAL(&faceSettingsMux);return true;
}
