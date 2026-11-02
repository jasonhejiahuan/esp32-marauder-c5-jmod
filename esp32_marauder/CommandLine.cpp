#include "CsiMotionMonitor.h"
#include "HostConnection.h"
#include "StorageService.h"
#include "StorageCommands.h"
#include "UsbOta.h"
#include "CommandLine.h"
#include "DeviceObservability.h"

// GCOVR_EXCL_START -- serial protocol output depends on Arduino Serial.
namespace {
  uint32_t commandRebootAt = 0;
  bool parseFiniteNumber(const String& value, double& parsed) {
    char* end = nullptr;
    parsed = strtod(value.c_str(), &end);
    return end != value.c_str() && *end == '\0' && isfinite(parsed);
  }
  bool validTransactionId(const String& transaction_id) {
    if (transaction_id.length() == 0 || transaction_id.length() > 40)
      return false;

    for (size_t i = 0; i < transaction_id.length(); i++) {
      char c = transaction_id.charAt(i);
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.'))
        return false;
    }
    return true;
  }

  void machineResult(
    const String& transaction_id,
    const char* command,
    const char* status,
    const char* code,
    size_t files = 0,
    size_t bytes = 0,
    bool rebooting = false
  ) {
    host_connection::console().printf(
      "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"%s\","
      "\"status\":\"%s\",\"code\":\"%s\",\"files\":%u,\"bytes\":%u,"
      "\"rebooting\":%s}\n",
      transaction_id.c_str(), command, status, code,
      (unsigned)files, (unsigned)bytes, rebooting ? "true" : "false"
    );
  }

  const char* storageErrorCode(uint8_t error, const char* fallback) {
    if (error == 1)
      return "SD_NOT_READY";
    if (error == 2)
      return "BACKUP_NOT_FOUND";
    return fallback;
  }
}
// GCOVR_EXCL_STOP

// Brightness functions defined in esp32_marauder.ino
#ifndef HAS_MINI_SCREEN
  extern void brightnessCycle();
  extern uint8_t getBrightnessLevel();
#endif

void CommandLine::RunSetup() {
  #ifndef MARAUDER_V8
    host_connection::console().println(this->ascii_art);
  #endif

  host_connection::console().println(F("\n\n--------------------------------\n"));
  host_connection::console().println(F("         ESP32 Marauder      \n"));
  host_connection::console().println("            " + version_number + "\n");
  host_connection::console().println(F("       By: justcallmekoko\n"));
  host_connection::console().println(F("--------------------------------\n\n"));
  
  host_connection::console().print("> ");
}

String CommandLine::getSerialInput() {
  static String pending;static bool discard=false;
  for(unsigned n=0;n<256&&Serial.available();n++){
    char c=Serial.read();if(c=='\r')continue;
    if(c=='\n'){String line=discard?String():pending;pending="";discard=false;line.trim();return line;}
    if(uint8_t(c)<32||c==127||pending.length()>=2048){discard=true;continue;}
    if(!discard)pending+=c;
  }
  return String();
}
void CommandLine::main(uint32_t currentTime) {
  if(commandRebootAt && int32_t(millis()-commandRebootAt)>=0)ESP.restart();
#ifdef HOST_CONNECTION_ENABLED
  if(host_connection::initialized())host_connection::execute();
  else {String input=getSerialInput();runCommand(input);if(input.length())host_connection::console().print("> ");}
#else
  String input=getSerialInput();runCommand(input);if(input.length())host_connection::console().print("> ");
#endif
}

LinkedList<String> CommandLine::parseCommand(String input, char* delim) {
  LinkedList<String> cmd_args;

  bool inQuote = false;
  bool inApostrophe = false;
  String buffer = "";
  bool tokenStarted=false;

  for (int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);

    if (c == '"') {
      tokenStarted=true;
      // Check if the quote is within an apostrophe
      if (inApostrophe) {
        buffer += c;
      } else {
        inQuote = !inQuote;
      }
    } else if (c == '\'') {
      tokenStarted=true;
      // Check if the apostrophe is within a quote
      if (inQuote) {
        buffer += c;
      } else {
        inApostrophe = !inApostrophe;
      }
    } else if (!inQuote && !inApostrophe && strchr(delim, c) != NULL) {
      if (tokenStarted) cmd_args.add(buffer);
      buffer = "";tokenStarted=false;
    } else {
      buffer += c;tokenStarted=true;
    }
  }

  // Add the last argument
  if (tokenStarted) {
    cmd_args.add(buffer);
  }

  return cmd_args;
}

int CommandLine::argSearch(LinkedList<String>* cmd_args_list, const char* key) {
  if (!cmd_args_list || !key)
    return -1;

  for (int i = 0; i < cmd_args_list->size(); i++) {
    if (strcmp(cmd_args_list->get(i).c_str(), key) == 0)
      return i;
  }

  return -1;
}

bool CommandLine::checkValueExists(LinkedList<String>* cmd_args_list, int index) {
  if (index < cmd_args_list->size() - 1)
    return true;
    
  return false;
}

bool CommandLine::inRange(int max, int index) {
  if ((index >= 0) && (index < max))
    return true;

  return false;
}

/*bool CommandLine::apSelected() {
  for (int i = 0; i < access_points->size(); i++) {
    if (access_points->get(i).selected)
      return true;
  }

  return false;
}*/

bool CommandLine::hasSSIDs() {
  if (ssids->size() == 0)
    return false;

  return true;
}

void CommandLine::showCounts(int selected, int unselected) {
  host_connection::console().print((String) selected + " selected");
  
  if (unselected != -1) 
    host_connection::console().print(", " + (String) unselected + " unselected");
  
  host_connection::console().println("");
}

String CommandLine::toLowerCase(String str) {
  String result = str;
  for (int i = 0; i < str.length(); i++) {
    int charValue = str.charAt(i);
    if (charValue >= 65 && charValue <= 90) { // ASCII codes for uppercase letters
      charValue += 32;
      result.setCharAt(i, char(charValue));
    }
  }
  return result;
}

void CommandLine::filterAccessPoints(String filter) {
  int count_selected = 0;
  int count_unselected = 0;

  // Split the filter string into individual filters
  LinkedList<String> filters;
  int start = 0;
  int end = filter.indexOf(" or ");
  while (end != -1) {
    filters.add(filter.substring(start, end));
    start = end + 4;
    end = filter.indexOf(" or ", start);
  }
  filters.add(filter.substring(start));

  // Loop over each access point and check if it matches any of the filters
  for (int i = 0; i < access_points->size(); i++) {
    AccessPoint access_point = access_points->get(i);
    bool matchesFilter = false;
    for (int j = 0; j < filters.size(); j++) {
      String f = toLowerCase(filters.get(j));
      if (f.substring(0, 7) == "equals ") {
        String ssidEquals = f.substring(7);
        if ((ssidEquals.charAt(0) == '\"' && ssidEquals.charAt(ssidEquals.length() - 1) == '\"' && ssidEquals.length() > 1) ||
            (ssidEquals.charAt(0) == '\'' && ssidEquals.charAt(ssidEquals.length() - 1) == '\'' && ssidEquals.length() > 1)) {
          ssidEquals = ssidEquals.substring(1, ssidEquals.length() - 1);
        }
        if (access_point.essid.equalsIgnoreCase(ssidEquals)) {
          matchesFilter = true;
          break;
        }
      } else if (f.substring(0, 9) == "contains ") {
        String ssidContains = f.substring(9);
        if ((ssidContains.charAt(0) == '\"' && ssidContains.charAt(ssidContains.length() - 1) == '\"' && ssidContains.length() > 1) ||
            (ssidContains.charAt(0) == '\'' && ssidContains.charAt(ssidContains.length() - 1) == '\'' && ssidContains.length() > 1)) {
          ssidContains = ssidContains.substring(1, ssidContains.length() - 1);
        }
        String essid = toLowerCase(access_point.essid);
        if (essid.indexOf(ssidContains) != -1) {
          matchesFilter = true;
          break;
        }
      }
    }
    // Toggles the selected state of the AP
    access_point.selected = matchesFilter;
    access_points->set(i, access_point);

    if (matchesFilter) {
      count_selected++;
    } else {
      count_unselected++;
    }
  }

  this->showCounts(count_selected, count_unselected);
}

void CommandLine::startScanFromCLI(int scan_mode, uint16_t color, const char* scan_name) {
  host_connection::console().print(F("Starting "));
  host_connection::console().print(scan_name);
  host_connection::console().print(F(". Stop with "));
  host_connection::console().println(STOPSCAN_CMD);
  #ifdef HAS_SCREEN
    display_obj.clearScreen();
    menu_function_obj.drawStatusBar();
  #endif
  wifi_scan_obj.StartScan(scan_mode, color);
}


namespace {
struct CommandUsage {const char* name;const char* usage;};
const CommandUsage commandUsage[]={
{"csimotion", "csimotion start <AP index>|status|recalibrate; stopscan to stop"},
{"channel", HELP_CH_CMD},
{"clearlist", HELP_CLEARAP_CMD_A},
{"reboot", HELP_REBOOT_CMD},
{"update", HELP_UPDATE_CMD_A},
{"help", "help"},
{"settings", HELP_SETTINGS_CMD},
{"geofence", HELP_GEOFENCE_CMD},
{"ls", HELP_LS_CMD},
{"protocolinfo", HELP_PROTOCOL_INFO_CMD},
{"backupspiffs", HELP_BACKUP_SPIFFS_CMD},
{"backupstatus", HELP_BACKUP_STATUS_CMD},
{"restorespiffs", HELP_RESTORE_SPIFFS_CMD},
{"led", HELP_LED_CMD},
{"gpsdata", HELP_GPS_DATA_CMD},
{"gps", HELP_GPS_CMD},
{"nmea", HELP_NMEA_CMD},
{"gpspoi", HELP_GPS_POI_CMD},
{"gpstracker", HELP_GPS_TRACKER_CMD},
{"recon", HELP_RECON_CMD},
{"evilportal", HELP_EVIL_PORTAL_CMD},
{"karma", HELP_KARMA_CMD},
{"packetcount", HELP_PACKET_COUNT_CMD},
{"foxhunt", HELP_SIGSTREN_CMD},
{"scanall", HELP_SCAN_ALL_CMD},
{"sniffraw", HELP_SNIFF_RAW_CMD},
{"sniffbeacon", HELP_SNIFF_BEACON_CMD},
{"sniffprobe", HELP_SNIFF_PROBE_CMD},
{"sniffpwn", HELP_SNIFF_PWN_CMD},
{"sniffpinescan", HELP_SNIFF_PINESCAN_CMD},
{"sniffmultissid", HELP_SNIFF_MULTISSID_CMD},
{"sniffdeauth", HELP_SNIFF_DEAUTH_CMD},
{"sniffpmkid", HELP_SNIFF_PMKID_CMD},
{"stopscan", HELP_STOPSCAN_CMD},
{"wardrive", HELP_WARDRIVE_CMD},
{"pingscan", HELP_PING_CMD},
{"portscan", HELP_PORT_SCAN_CMD},
{"arpscan", HELP_ARP_SCAN_CMD},
{"mactrack", HELP_MAC_TRACK_CMD},
{"sniffsae", HELP_SNIFF_SAE_CMD},
{"remoteid", HELP_REMOTE_ID_CMD},
{"attack", HELP_ATTACK_CMD},
{"list", HELP_LIST_AP_CMD_A},
{"info", HELP_INFO_CMD},
{"select", HELP_SEL_CMD_A},
{"ssid", HELP_SSID_CMD_A},
{"save", HELP_SAVE_CMD},
{"load", HELP_LOAD_CMD},
{"join", HELP_JOIN_CMD},
{"randapmac", HELP_MAC_CMD_A},
{"randstamac", HELP_MAC_CMD_B},
{"cloneapmac", HELP_MAC_CMD_C},
{"clonestamac", HELP_MAC_CMD_D},
{"add", HELP_ADD_CMD_A},
{"upload", HELP_UPLOAD_CMD},
{"blespam", HELP_BT_SPAM_CMD},
{"findmy", HELP_BT_FINDMY_CMD},
{"sniffbt", HELP_BT_SNIFF_CMD},
{"spoofat", HELP_BT_SPOOFAT_CMD},
{"sniffskim", HELP_BT_SKIM_CMD},
{"wardrivepoi", HELP_WARDRIVEPOI_CMD},
{"channel [-s <channel>]", "channel [-s <channel>]"},
{"reboot", "reboot"},
{"settings [-s <setting> enable/disable>]/[-r]", "settings [-s <setting> enable/disable>]/[-r]"},
{"ls <directory>", "ls <directory>"},
{"protocolinfo [--machine <transaction-id>]", "protocolinfo [--machine <transaction-id>]"},
{"backupspiffs [--machine <transaction-id>] - copy SPIFFS to /spiffs on application storage", "backupspiffs [--machine <transaction-id>] - copy SPIFFS to /spiffs on application storage"},
{"backupstatus [--machine <transaction-id>] - inspect /spiffs on application storage", "backupstatus [--machine <transaction-id>] - inspect /spiffs on application storage"},
{"restorespiffs [--machine <transaction-id>] - restore SPIFFS from /spiffs on application storage", "restorespiffs [--machine <transaction-id>] - restore SPIFFS from /spiffs on application storage"},
{"led -s <hex color>/-p <rainbow>", "led -s <hex color>/-p <rainbow>"},
{"gpsdata", "gpsdata"},
{"gps [-t] [-g] <fix/sat/lon/lat/alt/date/accuracy/text/nmea>\r\n    [-n] <native/all/gps/glonass/galileo/navic/qzss/beidou>\r\n         [-b = use BD vs GB for beidou]", "gps [-t] [-g] <fix/sat/lon/lat/alt/date/accuracy/text/nmea>\r\n    [-n] <native/all/gps/glonass/galileo/navic/qzss/beidou>\r\n         [-b = use BD vs GB for beidou]"},
{"gpspoi -s/-m/-e", "gpspoi -s/-m/-e"},
{"gpstracker -c <start/stop>", "gpstracker -c <start/stop>"},
{"recon wifi|ble|status|stop", "recon wifi|ble|status|stop"},
{"nmea", "nmea"},
{"evilportal [-c start [-w html.html]/sethtml <html.html>]", "evilportal [-c start [-w html.html]/sethtml <html.html>]"},
{"karma -p <index>", "karma -p <index>"},
{"packetcount", "packetcount"},
{"foxhunt -w <ap>/-s <ap> <station>/-b <ble>/-t <findmy>/-f <flipper>/-p <pineapple>/-m <multissid>", "foxhunt -w <ap>/-s <ap> <station>/-b <ble>/-t <findmy>/-f <flipper>/-p <pineapple>/-m <multissid>"},
{"scanall", "scanall"},
{"stopscan [-f]", "stopscan [-f]"},
{"wardrive", "wardrive"},
{"pingscan", "pingscan"},
{"portscan [-a -t <ip index>]/[-s <ssh/telnet/dns/http/smtp/https/rdp>]", "portscan [-a -t <ip index>]/[-s <ssh/telnet/dns/http/smtp/https/rdp>]"},
{"arpscan [-f]", "arpscan [-f]"},
{"mactrack", "mactrack"},
{"remoteid <scan/list/track/clear> [index]", "remoteid <scan/list/track/clear> [index]"},
{"attack -t <quiet/csa/sae/beacon [-l/-r/-a]/deauth [-c]/[-s <src mac>] [-d <dst mac>]/probe/rickroll/badmsg [-c]/sleep [-c]>", "attack -t <quiet/csa/sae/beacon [-l/-r/-a]/deauth [-c]/[-s <src mac>] [-d <dst mac>]/probe/rickroll/badmsg [-c]/sleep [-c]>"},
{"info [-a <index>]", "info [-a <index>]"},
{"save -a/-s", "save -a/-s"},
{"load -a/-s", "load -a/-s"},
{"join -a <index> -p <password>/-s", "join -a <index> -p <password>/-s"},
{"upload -d <wdg/wigle/both>", "upload -d <wdg/wigle/both>"},
{"sniffbt [-t] <airtag/ibeacon/flipper/flock/meta>", "sniffbt [-t] <airtag/ibeacon/flipper/flock/meta>"},
{"findmy -t <index>", "findmy -t <index>"},
{"blespam -t <sourapple/applejuice/google/samsung/windows/flipper/all>", "blespam -t <sourapple/applejuice/google/samsung/windows/flipper/all>"},
{"spoofat -t <index>", "spoofat -t <index>"},
{"sniffskim", "sniffskim"},
{"brightness", HELP_BRIGHTNESS_CMD},
{"brightness [-c cycle] [-s <0-9>]", "brightness [-c cycle] [-s <0-9>]"},
{"wardrivepoi [label] - Tag a GPS POI during wardrive", "wardrivepoi [label] - Tag a GPS POI during wardrive"},
{"scancombined","scancombined [--host-only]"},
{"scanap","scanap"},{"scansta","scansta"},{"sigmon","sigmon"},
{"storageinfo","storageinfo"}, {"deviceinfo","deviceinfo [--machine <transaction>]"},
{"bleobserve","bleobserve start|stop|status|clear"}, {"file","file list|stat|read|rename|mkdir|trash|restore|delete|emptytrash"},
{"ota","ota status|begin|write|commit|confirm|abort"}
};
const char* usageFor(const String& name){for(const auto& entry:commandUsage)if(name==entry.name)return entry.usage;return nullptr;}
#ifdef HOST_CONNECTION_ENABLED
struct CommandFeedback {
 String name;uint32_t start;
 CommandFeedback(const String& n):name(n),start(host_connection::console().commandBytes()){}
 ~CommandFeedback(){if(host_connection::console().commandBytes()==start){
  host_connection::console().println("Error: no action for " + name + ": check the arguments and target index; use list to inspect available records.");
  if(const char* usage=usageFor(name))host_connection::console().println(usage);
 }}
};
#endif
}

void CommandLine::runCommand(String input) {
  input.trim();
  if (input == "") return;
#ifdef HOST_CONNECTION_ENABLED
  if(input.indexOf("--host-only")>=0){host_connection::console().println("--host-only requires Dashboard Host_Connection native sniff service");return;}
#endif
#ifdef HOST_CONNECTION_ENABLED
  // Same operation exclusions for CLI_EXEC and external CLI as for typed requests.
  if(host_connection::capturing() && input!="stopscan" && input!="help" && input!="info" && !input.startsWith("protocolinfo") && !input.startsWith("deviceinfo")){host_connection::console().println("Host_Connection stream active; use stopscan before another operation");return;}
  if(storage_service::leased() && input!="help" && !input.startsWith("protocolinfo") && !input.startsWith("deviceinfo") && !input.startsWith("info")) {host_connection::console().println("File transfer active");return;}
#endif
  #if defined(MARAUDER_C5) && (defined(MARAUDER_WAVESHARE_C5_RGB) || defined(MARAUDER_XIAO_C5)) && defined(MARAUDER_INTERNAL_STORAGE)
  if (input=="ota" || input.startsWith("ota ")) {usb_ota::command(input);return;}
  if (usb_ota::active()) {host_connection::console().println("OTA transfer active; use ota status or ota abort");return;}
  #endif

  if(wifi_scan_obj.scanning() && wifi_scan_obj.currentScanMode == WIFI_SCAN_GPS_NMEA){
    if(input != STOPSCAN_CMD) return;    
  }
  else
    host_connection::console().println("#" + input);

  LinkedList<String> cmd_args = this->parseCommand(input, " ");
  if(!cmd_args.size())return;
  // Historical wiki spellings retain the current upstream implementation.
  String name=cmd_args.get(0);

  if(name=="btwardrive") {
#ifndef HAS_GPS
    host_connection::console().println("deprecated on this hardware profile: no GPS receiver; use sniffbt for BLE observation.");return;
#endif
  }
  if(!usageFor(name)){host_connection::console().println("Unknown command: "+name+". Run help for this firmware's syntax.");return;}
#ifdef HOST_CONNECTION_ENABLED
  CommandFeedback feedback(name);
#endif
#ifndef HAS_GPS
  if(name=="gps"||name=="gpsdata"||name=="nmea"||name=="gpspoi"||name=="gpstracker"||name=="wardrive"||name=="btwardrive"||name=="wardrivepoi"){
#ifdef MARAUDER_XIAO_C5
    host_connection::console().println("No external GPS configured. XIAO native pin definitions are preserved; an optional GPS build can use explicitly selected D0/D1/D2 pads.");return;
#else
    host_connection::console().println("deprecated on this hardware profile: GPS receiver unavailable; GPIO13/14 are assigned to native USB. Use non-GPS scanall, sniffbt or recon wifi/ble for observation.");return;
#endif
  }
#endif
#ifndef HAS_SCREEN
  if(name=="brightness"){
#ifdef MARAUDER_XIAO_C5
    host_connection::console().println("No display/backlight fitted. The onboard yellow LED is controlled with led -s FFFFFF / led -s 000000.");
#else
    host_connection::console().println("deprecated on this hardware profile: no display/backlight is fitted. RGB feedback remains available through led -s <hex color> or led -p rainbow.");
#endif
    return;
  }
#endif
  const bool networkScan=name=="pingscan"||name=="arpscan"||name=="portscan";
  if(networkScan&&!wifi_scan_obj.wifi_connected){host_connection::console().println("WiFi is not connected. Run scanall, stopscan, list -a, then join -a <index> -p <password> (or join -s); retry after Connected.");return;}
  const bool radioStart=name.startsWith("sniff")||name=="scanap"||name=="scansta"||name=="sigmon"||name=="scanall"||name=="scancombined"||name=="foxhunt"||name=="packetcount"||name=="mactrack"||name=="attack"||name=="blespam"||name=="spoofat"||name=="findmy"||name=="karma"||name=="evilportal"||networkScan||(name=="remoteid"&&cmd_args.size()>1&&(cmd_args.get(1)=="scan"||cmd_args.get(1)=="track"));
  if(radioStart&&wifi_scan_obj.scanning()&&wifi_scan_obj.currentScanMode!=WIFI_CONNECTED){host_connection::console().println("A scan is already running. Run stopscan, then retry "+name+"; use info/deviceinfo to inspect the device.");return;}
  // Required option values must not silently turn an empty string into index 0.
  for(int i=1;i<cmd_args.size();i++){
    String option=cmd_args.get(i);
    bool required=(option=="-t"&&(name=="sniffbt"||name=="blespam"||name=="spoofat"||name=="findmy"||name=="foxhunt"||name=="portscan"));
    required=required||(option=="-w"&&(name=="foxhunt"||name=="evilportal"))||((option=="-n"||option=="-g")&&(name=="ssid"||name=="gps"))||((option=="-ap"||option=="-ch"||option=="-e")&&name=="add");
    required=required||((name=="select"||name=="join"||name=="info")&&(option=="-a"||option=="-c"||option=="-p"||option=="-f"||(name=="select"&&option=="-s")));
    required=required||((name=="channel"||name=="settings"||name=="led"||name=="portscan")&&option=="-s");
    required=required||((name=="foxhunt"||name=="add")&&(option=="-b"||option=="-s"||option=="-f"||option=="-p"||option=="-m"));
    if(required&&(i+1>=cmd_args.size()||cmd_args.get(i+1).startsWith("-"))){host_connection::console().println("Missing value for "+option);host_connection::console().println(usageFor(name));return;}
  }
  if(name=="spoofat"||name=="findmy"){
    int flag=this->argSearch(&cmd_args,"-t");double index;
    if(flag<0){host_connection::console().println(usageFor(name));return;}
    if(!parseFiniteNumber(cmd_args.get(flag+1),index)||floor(index)!=index||index<0||index>=airtags->size()){
      host_connection::console().printf("Invalid AirTag index: %s; %d records available. Run sniffbt -t airtag, stopscan, list -t, then %s -t <listed index>.\n",cmd_args.get(flag+1).c_str(),airtags->size(),name.c_str());return;
    }
  }
  bool packetTargets=wifi_scan_obj.filterActive();
  if(name=="packetcount")for(int i=0;i<stations->size();++i)packetTargets|=stations->get(i).selected;
  if(name=="packetcount"&&!packetTargets){host_connection::console().println("No targets selected. Run scanall, stopscan, list -a/-c, select -a/-c <index>, then packetcount.");return;}
#ifdef HAS_CSI_MOTION
  if(name=="csimotion"){
    String action=cmd_args.size()>1?cmd_args.get(1):"";
    if(action=="status"&&cmd_args.size()==2){
      if(csi_motion_obj.active())csi_motion_obj.printStatus();
      else host_connection::console().println("CSI motion stopped. Use scanap, stopscan, list -a, then csimotion start <AP index>.");
    }else if(action=="recalibrate"&&cmd_args.size()==2){
      if(!csi_motion_obj.active())host_connection::console().println("CSI motion is not running. Start with csimotion start <AP index>.");
      else {csi_motion_obj.recalibrate();host_connection::console().println("CSI recalibration started; keep the device still for 30 seconds.");}
    }else if(action=="start"&&cmd_args.size()==3){
      double index;
      if(!parseFiniteNumber(cmd_args.get(2),index)||floor(index)!=index||index<0||index>=access_points->size())
        host_connection::console().println("Invalid AP index. Run scanap, stopscan, list -a, then csimotion start <listed index>.");
      else if(wifi_scan_obj.scanning()||device_observation::bleActive())
        host_connection::console().println("Radio is busy. Run stopscan before csimotion start.");
      else {AccessPoint ap=access_points->get((int)index);
        if(!csi_motion_obj.start(ap.bssid,ap.essid,ap.channel))host_connection::console().println("CSI start failed: radio initialization or memory allocation failed. Run deviceinfo, stopscan, then retry with a valid AP channel.");}
    }else host_connection::console().println("Usage: csimotion start <AP index>|status|recalibrate; stopscan to stop.");
    return;
  }
#endif
  if(name=="scanap"){this->startScanFromCLI(WIFI_SCAN_ALL,TFT_MAGENTA," AP scan");return;}
  if(name=="scansta"){
    if(!access_points->size()){host_connection::console().println("AP list is empty. Run scanap, stopscan and list -a before scansta.");return;}
    this->startScanFromCLI(WIFI_SCAN_STATION,TFT_CYAN," Station scan");return;
  }
  if(name=="sigmon"){
    bool selected=false;for(int i=0;i<access_points->size();i++)selected|=access_points->get(i).selected;
    if(!selected){host_connection::console().println("No AP selected. Run scanap, stopscan, list -a, select -a <index>, then sigmon.");return;}
    wifi_scan_obj.monitor_selected_aps=true;this->startScanFromCLI(WIFI_SCAN_SIG_STREN,TFT_GREEN," Signal monitor");return;
  }
  if(name=="foxhunt")wifi_scan_obj.monitor_selected_aps=false;
  if(name=="gpstracker") {cmd_args.set(0,String(GPS_CMD));cmd_args.add("-t");}
  if(name=="info"&&this->argSearch(&cmd_args,"-a")>=0){int i=this->argSearch(&cmd_args,"-a");double value;if(!parseFiniteNumber(cmd_args.get(i+1),value)||value<0||value>=access_points->size()||floor(value)!=value){host_connection::console().println("Invalid AP index. Run list -a; scanall then stopscan if empty.");return;}}
  if(name=="join"&&this->argSearch(&cmd_args,"-a")>=0){int i=this->argSearch(&cmd_args,"-a");double value;if(!parseFiniteNumber(cmd_args.get(i+1),value)||value<0||value>=access_points->size()||floor(value)!=value){host_connection::console().println("Invalid AP index. Run list -a; scanall then stopscan if empty.");return;}}

  
  #ifdef MARAUDER_C5
  const String first = cmd_args.get(0);
  if (device_observation::bleActive() && first != "bleobserve" && first != "deviceinfo" && first != "stopscan" && first != "help" && first != "protocolinfo" && first != "storageinfo" && first != "info" && first != "reboot") {
    host_connection::console().println("BLE observation active; use bleobserve stop before another operation"); return;
  }
  if (first == "deviceinfo") {
    int m=this->argSearch(&cmd_args,"--machine");String tx=m>=0 && m+1<cmd_args.size()?cmd_args.get(m+1):"manual";
    if(!validTransactionId(tx)){host_connection::console().println("deviceinfo error: invalid transaction");return;}
    device_observation::report(tx.c_str());return;
  }
  if (first == "bleobserve") {
    String action=cmd_args.size()>1?cmd_args.get(1):"status";device_observation::bleCommand(action);
    if(action=="status")host_connection::console().println(device_observation::bleActive()?"BLE observation: active; use bleobserve stop to stop":"BLE observation: stopped; use bleobserve start to discover devices");
    if(action=="clear"&&!device_observation::bleActive())host_connection::console().println("BLE observation list cleared");
    return;
  }
  #endif
  #ifdef MARAUDER_INTERNAL_STORAGE
  if (cmd_args.get(0) == "file") {storage_commands::command(input);return;}
  #endif
  //// Admin commands
  // Help
  if (cmd_args.get(0) == HELP_CMD) {
    host_connection::console().println(HELP_HEAD);
#ifdef HOST_CONNECTION_ENABLED
    host_connection::console().println("host_connection upgrade 1 <nonce> - negotiate Host_Connection");
#endif
    host_connection::console().println(HELP_CH_CMD);
    host_connection::console().println(HELP_SETTINGS_CMD);
    host_connection::console().println(HELP_GEOFENCE_CMD);
    host_connection::console().println(HELP_CLEARAP_CMD_A);
    host_connection::console().println(HELP_REBOOT_CMD);
    host_connection::console().println(HELP_UPDATE_CMD_A);
    host_connection::console().println(HELP_LS_CMD);
    host_connection::console().println("storageinfo - report storage backend and capacity");
    #if defined(MARAUDER_C5) && (defined(MARAUDER_WAVESHARE_C5_RGB) || defined(MARAUDER_XIAO_C5)) && defined(MARAUDER_INTERNAL_STORAGE)
    host_connection::console().println("ota status|begin|write|commit|confirm|abort <transaction> [parameters] - USB dual-slot firmware update");
    #endif
    #ifdef MARAUDER_INTERNAL_STORAGE
    host_connection::console().println("file list|stat|read|rename|mkdir|trash|restore|delete|emptytrash <transaction> <hex-path> [offset|hex-destination] [length] - internal file manager");
    #endif
    // GCOVR_EXCL_START -- hardware-only command help entry.
    host_connection::console().println(HELP_PROTOCOL_INFO_CMD);
    #ifdef MARAUDER_C5
    host_connection::console().println("deviceinfo [--machine <transaction-id>] - device temperature, memory and storage");
    host_connection::console().println("bleobserve start|stop|status|clear - passive BLE advertisement observer");
    #endif
    #ifdef HAS_STORAGE
      host_connection::console().println(HELP_BACKUP_SPIFFS_CMD);
      host_connection::console().println(HELP_BACKUP_STATUS_CMD);
      host_connection::console().println(HELP_RESTORE_SPIFFS_CMD);
    #endif
    // GCOVR_EXCL_STOP
    host_connection::console().println(HELP_LED_CMD);
#ifdef HAS_GPS
    host_connection::console().println(HELP_GPS_DATA_CMD);
    host_connection::console().println(HELP_GPS_CMD);
    host_connection::console().println(HELP_NMEA_CMD);
    host_connection::console().println(HELP_GPS_POI_CMD);
    host_connection::console().println(HELP_GPS_TRACKER_CMD);
#else
    for(const char* cmd:{"gps","gpsdata","nmea","gpspoi","gpstracker","wardrive","btwardrive","wardrivepoi"})
#ifdef MARAUDER_XIAO_C5
      host_connection::console().println(String(cmd)+" - requires an external GPS and explicit free-pad configuration");
#else
      host_connection::console().println(String(cmd)+" - deprecated: no GPS receiver; GPIO13/14 used by USB");
#endif
#endif
    host_connection::console().println(HELP_RECON_CMD);
    
    // WiFi sniff/scan
    host_connection::console().println(HELP_EVIL_PORTAL_CMD);
    host_connection::console().println(HELP_KARMA_CMD);
    host_connection::console().println(HELP_PACKET_COUNT_CMD);
    host_connection::console().println(HELP_PING_CMD);
    host_connection::console().println(HELP_ARP_SCAN_CMD);
    host_connection::console().println(HELP_PORT_SCAN_CMD);
    host_connection::console().println(HELP_SIGSTREN_CMD);
    host_connection::console().println(String(HELP_SCAN_ALL_CMD) + HOST_SNIFF_HELP);
    host_connection::console().println("scancombined [--host-only] (AP + STA + Beacon; discovery-v1)");
    //host_connection::console().println(HELP_SCANSTA_CMD);
    host_connection::console().println(HELP_SNIFF_RAW_CMD);
    host_connection::console().println(HELP_SNIFF_BEACON_CMD);
    host_connection::console().println(HELP_SNIFF_PROBE_CMD);
    host_connection::console().println(HELP_SNIFF_PWN_CMD);
    host_connection::console().println(HELP_SNIFF_PINESCAN_CMD);
    host_connection::console().println(HELP_SNIFF_MULTISSID_CMD);
    host_connection::console().println(HELP_SNIFF_DEAUTH_CMD);
    host_connection::console().println(HELP_SNIFF_PMKID_CMD);
    host_connection::console().println(HELP_SNIFF_SAE_CMD);
    host_connection::console().println(HELP_REMOTE_ID_CMD);
    host_connection::console().println(HELP_STOPSCAN_CMD);
    #ifdef HAS_GPS
      host_connection::console().println(HELP_WARDRIVE_CMD);
      host_connection::console().println(HELP_WARDRIVEPOI_CMD);
    #endif
    host_connection::console().println(HELP_MAC_TRACK_CMD);
    
    // WiFi attack
    host_connection::console().println(HELP_ATTACK_CMD);
    
    // WiFi Aux
    host_connection::console().println(HELP_INFO_CMD);
    host_connection::console().println(HELP_LIST_AP_CMD_A);
    host_connection::console().println(HELP_LIST_AP_CMD_B);
    host_connection::console().println(HELP_LIST_AP_CMD_C);
    host_connection::console().println(HELP_LIST_AP_CMD_D);
    host_connection::console().println(HELP_LIST_AP_CMD_E);
    host_connection::console().println(HELP_LIST_AP_CMD_F);
    host_connection::console().println(HELP_LIST_AP_CMD_G);
    host_connection::console().println(HELP_LIST_AP_CMD_H);
    host_connection::console().println(HELP_LIST_AP_CMD_I);
    host_connection::console().println(HELP_LIST_AP_CMD_J);
    host_connection::console().println(HELP_SEL_CMD_A);
    host_connection::console().println(HELP_SSID_CMD_A);
    host_connection::console().println(HELP_SSID_CMD_B);
    host_connection::console().println(HELP_SAVE_CMD);
    host_connection::console().println(HELP_LOAD_CMD);
    host_connection::console().println(HELP_JOIN_CMD);
    host_connection::console().println(HELP_MAC_CMD_A);
    host_connection::console().println(HELP_MAC_CMD_B);
    host_connection::console().println(HELP_MAC_CMD_C);
    host_connection::console().println(HELP_MAC_CMD_D);
    host_connection::console().println(HELP_ADD_CMD_A);
    host_connection::console().println(HELP_ADD_CMD_B);
    host_connection::console().println(HELP_UPLOAD_CMD);

    // Bluetooth sniff/scan
    #ifdef HAS_BT
      host_connection::console().println(HELP_BT_SNIFF_CMD);
      host_connection::console().println(HELP_BT_SPAM_CMD);
      host_connection::console().println(HELP_BT_FINDMY_CMD);
      host_connection::console().println(HELP_BT_SPOOFAT_CMD);
      host_connection::console().println(HELP_BT_SKIM_CMD);
    #endif
#ifdef HAS_SCREEN
    host_connection::console().println(HELP_BRIGHTNESS_CMD);
#else
    host_connection::console().println("brightness - deprecated: no display/backlight; use led for RGB");
#endif
    host_connection::console().println("scanap - discover APs and retain their native list indices");
    host_connection::console().println("scansta - discover stations associated with the existing AP list");
    host_connection::console().println("sigmon - monitor selected APs; report RSSI changes of at least 5 dBm");
    host_connection::console().println("csimotion start <AP index>|status|recalibrate - passive CSI motion monitor; stopscan to stop");
    host_connection::console().println(HELP_FOOT);
    return;
  }

  // Stop Scan
  if (cmd_args.get(0) == STOPSCAN_CMD) {
    int f_arg = this->argSearch(&cmd_args, "-f");
    
    uint8_t old_scan_mode=wifi_scan_obj.currentScanMode;

    if (f_arg != -1) {
      WiFi.disconnect(true);
      delay(100);
    }

    #ifdef MARAUDER_C5
    device_observation::stop();
    #endif
    wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
    recon_obj.stop();
    buffer_obj.save(true);

    if(old_scan_mode == WIFI_SCAN_GPS_NMEA)
      host_connection::console().println(F("END OF NMEA STREAM"));
    else if(old_scan_mode == WIFI_SCAN_GPS_DATA)
      host_connection::console().println(F("Stopping GPS data updates"));
    else
      host_connection::console().println(F("Stopping WiFi tran/recv"));

    // If we don't do this, the text and button coordinates will be off
    #ifdef HAS_SCREEN
      display_obj.init();
      menu_function_obj.changeMenu(menu_function_obj.current_menu);
    #endif
  }
  else if (cmd_args.get(0) == RECON_CMD) {
    if (cmd_args.size() < 2 || cmd_args.get(1) == "status") {
      host_connection::console().println(recon_obj.active() ? F("active") : F("stopped"));
    }
    else if (cmd_args.get(1) == "stop") {
      recon_obj.stop();
      wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
      host_connection::console().println(F("stopped"));
    }
    else {
      if (wifi_scan_obj.scanning()) {
        host_connection::console().println(F("Stop the current scan before starting Recon"));
      }
      else if (cmd_args.get(1) == "wifi") {
        if (recon_obj.start(ReconMode::WIFI_RECON)) host_connection::console().println(F("active"));
      }
      #ifdef HAS_BT
        else if (cmd_args.get(1) == "ble") {
          if (recon_obj.start(ReconMode::BLE_RECON)) host_connection::console().println(F("active"));
        }
      #endif
      else host_connection::console().println(HELP_RECON_CMD);
    }
  }
  else if (cmd_args.get(0) == GPS_DATA_CMD) {
    #ifdef HAS_GPS
      if (gps_obj.getGpsModuleStatus()) {
        host_connection::console().print(F("Getting GPS Data. Stop with "));
        host_connection::console().println((String)STOPSCAN_CMD);
        wifi_scan_obj.currentScanMode = WIFI_SCAN_GPS_DATA;
        #ifdef HAS_SCREEN
          menu_function_obj.changeMenu(&menu_function_obj.gpsInfoMenu);
        #endif
        wifi_scan_obj.StartScan(WIFI_SCAN_GPS_DATA, TFT_CYAN);
      }
    #endif
  }
  else if (cmd_args.get(0) == GPS_CMD) {
    #ifdef HAS_GPS
      if (gps_obj.getGpsModuleStatus()) {
        int get_arg = this->argSearch(&cmd_args, "-g");
        int track_arg = this->argSearch(&cmd_args, "-t");
        int nmea_arg = this->argSearch(&cmd_args, "-n");

        if (get_arg != -1) {
          String gps_info = cmd_args.get(get_arg + 1);

          if (gps_info == "fix")
            host_connection::console().println("Fix: " + gps_obj.getFixStatusAsString());
          else if (gps_info == "sat")
            host_connection::console().println("Sats: " + gps_obj.getNumSatsString());
          else if (gps_info == "lat")
            host_connection::console().println("Lat: " + gps_obj.getLat());
          else if (gps_info == "lon")
            host_connection::console().println("Lon: " + gps_obj.getLon());
          else if (gps_info == "alt")
            host_connection::console().println("Alt: " + (String)gps_obj.getAlt());
          else if (gps_info == "accuracy")
            host_connection::console().println("Accuracy: " + (String)gps_obj.getAccuracy());
          else if (gps_info == "date")
            host_connection::console().println("Date/Time: " + gps_obj.getDatetime());
          else if (gps_info == "text"){
            host_connection::console().println(gps_obj.getText());
          }
          else if (gps_info == "nmea"){
            int notparsed_arg = this->argSearch(&cmd_args, "-p");
            int notimp_arg = this->argSearch(&cmd_args, "-i");
            int recd_arg = this->argSearch(&cmd_args, "-r");
            if(notparsed_arg == -1 && notimp_arg == -1 && recd_arg == -1){
              gps_obj.sendSentence(Serial, gps_obj.generateGXgga().c_str());
              gps_obj.sendSentence(Serial, gps_obj.generateGXrmc().c_str());
            }
            else if(notparsed_arg == -1 && notimp_arg == -1)
              host_connection::console().println(gps_obj.getNmea());
            else if(notparsed_arg == -1)
              host_connection::console().println(gps_obj.getNmeaNotimp());
            else
              host_connection::console().println(gps_obj.getNmeaNotparsed());
          }
          else
            host_connection::console().println(F("You did not provide a valid argument"));
        }
        else if(nmea_arg != -1){
          String nmea_type = cmd_args.get(nmea_arg + 1);

          if (nmea_type == "native" || nmea_type == "all" || nmea_type == "gps" || nmea_type == "glonass"
              || nmea_type == "galileo" || nmea_type == "navic" || nmea_type == "qzss" || nmea_type == "beidou"){
            if(nmea_type == "beidou"){
              int beidou_bd_arg = this->argSearch(&cmd_args, "-b");
              if(beidou_bd_arg != -1)
                nmea_type="beidou_bd";
            }
            gps_obj.setType(nmea_type);
            host_connection::console().print(F("GPS Output Type Set To: "));
            host_connection::console().println(nmea_type);
          }
        }
        else if (track_arg != -1) {
          wifi_scan_obj.currentScanMode = GPS_TRACKER;
          #ifdef HAS_SCREEN
            menu_function_obj.changeMenu(&menu_function_obj.gpsInfoMenu);
          #endif
          wifi_scan_obj.StartScan(GPS_TRACKER, TFT_CYAN);
        }
        else if(cmd_args.size()>1)
          host_connection::console().println(F("You did not provide a valid flag"));
      }
    #endif
  }
  else if (cmd_args.get(0) == NMEA_CMD) {
    #ifdef HAS_GPS
      if (gps_obj.getGpsModuleStatus()) {
        #ifdef HAS_SCREEN
          menu_function_obj.changeMenu(&menu_function_obj.gpsInfoMenu);
        #endif
        wifi_scan_obj.currentScanMode = WIFI_SCAN_GPS_NMEA;
        wifi_scan_obj.StartScan(WIFI_SCAN_GPS_NMEA, TFT_CYAN);
      }
    #endif
  }
  // LED command
  else if (cmd_args.get(0) == LED_CMD) {
    int hex_arg = this->argSearch(&cmd_args, "-s");
    int pat_arg = this->argSearch(&cmd_args, "-p");
    #if (defined(PIN) && defined(HAS_NEOPIXEL_LED)) || defined(HAS_STATUS_LED)
      if (hex_arg != -1) {
        String hexstring = cmd_args.get(hex_arg + 1);
        if(hexstring.startsWith("#"))hexstring.remove(0,1);
        bool valid=hexstring.length()==6;
        for(size_t i=0;i<hexstring.length();++i)if(!isxdigit((unsigned char)hexstring[i]))valid=false;
        if(!valid){host_connection::console().println("Error: expected led -s #RRGGBB (six hexadecimal digits)");return;}
        int number = (int)strtol(hexstring.c_str(), NULL, 16);
        int r = number >> 16;
        int g = number >> 8 & 0xFF;
        int b = number & 0xFF;
        //host_connection::console().println(r);
        //host_connection::console().println(g);
        //host_connection::console().println(b);
        led_obj.setColor(r, g, b);
        led_obj.setMode(MODE_CUSTOM);
        #ifdef HAS_STATUS_LED
        host_connection::console().println((r || g || b) ? "Single-color LED on" : "Single-color LED off");
        #else
        host_connection::console().println("LED color set: #"+hexstring);
        #endif
      }
      else if (pat_arg != -1) {
        String pat_name = cmd_args.get(pat_arg + 1);
        pat_name.toLowerCase();
        if (pat_name == "rainbow") {
          #ifdef HAS_STATUS_LED
          host_connection::console().println("Rainbow unavailable: this board has a single-color LED");
          #else
          led_obj.setMode(MODE_RAINBOW);
          host_connection::console().println("LED pattern set: rainbow");
          #endif
        }
      }
    #endif
  }
  // ls command
  else if (cmd_args.get(0) == LS_CMD) {
    #ifdef HAS_STORAGE
      sd_obj.listDir(cmd_args.size() > 1 ? cmd_args.get(1) : "/");
    #endif
  }
  // GCOVR_EXCL_START -- requires mounted SPIFFS and SD filesystems.
  else if (cmd_args.get(0) == PROTOCOL_INFO_CMD) {
    int machine_arg = this->argSearch(&cmd_args, "--machine");
    String transaction_id = machine_arg >= 0 && machine_arg + 1 < cmd_args.size()
      ? cmd_args.get(machine_arg + 1) : "";
    if (machine_arg >= 0 && !validTransactionId(transaction_id))
      machineResult(transaction_id, PROTOCOL_INFO_CMD, "error", "INVALID_TRANSACTION");
    else if (machine_arg < 0) {
#ifdef HOST_CONNECTION_ENABLED
      host_connection::discovery("manual");
#else
      host_connection::console().println("Protocol: 1; run protocolinfo --machine <transaction> for capabilities");
#endif
    }
    else if (machine_arg >= 0) {
      #ifdef HOST_CONNECTION_ENABLED
      host_connection::discovery(transaction_id.c_str());
      #else
      #ifdef HAS_STORAGE
        host_connection::console().printf(
          "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"protocolinfo\","
          "\"status\":\"success\",\"code\":\"OK\",\"firmware\":\"%s\","
          "\"capabilities\":[\"usb-ota-v1\",\"files-v1\","
          #ifdef MARAUDER_INTERNAL_STORAGE
          "\"files-delete-v1\","
          #endif
          "\"device-metrics-v1\",\"ble-observer-v1\",\"discovery-v1\",\"spiffs-backup\",\"spiffs-backup-status\","
          "\"spiffs-restore\"],\"backupPath\":\"/spiffs\"}\n",
          transaction_id.c_str(), version_number.c_str()
        );
      #else
        host_connection::console().printf(
          "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"protocolinfo\","
          "\"status\":\"success\",\"code\":\"OK\",\"firmware\":\"%s\","
          "\"capabilities\":[\"usb-ota-v1\",\"files-v1\","
          #ifdef MARAUDER_INTERNAL_STORAGE
          "\"files-delete-v1\","
          #endif
          "\"device-metrics-v1\",\"ble-observer-v1\",\"discovery-v1\"]}\n",
          transaction_id.c_str(), version_number.c_str()
        );
      #endif
      #endif
    }
  }
  else if (cmd_args.get(0) == BACKUP_SPIFFS_CMD ||
           cmd_args.get(0) == BACKUP_STATUS_CMD ||
           cmd_args.get(0) == RESTORE_SPIFFS_CMD) {
    uint8_t operation = cmd_args.get(0) == BACKUP_SPIFFS_CMD ? 0 :
                        cmd_args.get(0) == BACKUP_STATUS_CMD ? 1 : 2;
    const char* command = operation == 0 ? BACKUP_SPIFFS_CMD :
                          operation == 1 ? BACKUP_STATUS_CMD : RESTORE_SPIFFS_CMD;
    int machine_arg = this->argSearch(&cmd_args, "--machine");
    String transaction_id = machine_arg >= 0 && machine_arg + 1 < cmd_args.size()
      ? cmd_args.get(machine_arg + 1) : "";
    bool machine = machine_arg >= 0;
    if (machine && !validTransactionId(transaction_id)) {
      machineResult(transaction_id, command, "error", "INVALID_TRANSACTION");
      return;
    }
    #ifdef HAS_STORAGE
      size_t files = 0;
      size_t bytes = 0;
      uint8_t error = 0;
      if (machine && operation != 1)
        machineResult(transaction_id, command, "started", "OK");
      else if (!machine && operation != 1)
        host_connection::console().printf("SPIFFS %s started\n", operation == 0 ? "backup" : "restore");

      bool success = sd_obj.migrateSPIFFS(operation, files, bytes, error);
      if (machine) {
        if (success)
          machineResult(transaction_id, command, "success", "OK", files, bytes, operation == 2);
        else {
          const char* fallback = operation == 0 ? "BACKUP_FAILED" :
                                 operation == 1 ? "BACKUP_INSPECTION_FAILED" : "RESTORE_FAILED";
          machineResult(transaction_id, command, "error", storageErrorCode(error, fallback));
        }
      }
      else if (success) {
        const char* action = operation == 0 ? "backup complete" :
                             operation == 1 ? "backup status" : "restore complete";
        host_connection::console().printf("SPIFFS %s: %u files, %u bytes%s\n", action,
                      static_cast<unsigned int>(files),
                      static_cast<unsigned int>(bytes),
                      operation == 2 ? "; rebooting" : "");
      }
      else {
        const char* fallback = operation == 0 ? "BACKUP_FAILED" :
                               operation == 1 ? "BACKUP_INSPECTION_FAILED" : "RESTORE_FAILED";
        host_connection::console().printf("SPIFFS %s failed: %s\n",
                      operation == 0 ? "backup" : operation == 1 ? "backup status" : "restore",
                      storageErrorCode(error, fallback));
        if (error == 2)
          host_connection::console().println("No backup at /spiffs. Run backupspiffs to create one from current SPIFFS, then backupstatus; restore requires an existing backup.");
        else if (error == 1)
          host_connection::console().println("Run storageinfo to check whether application storage is mounted. Stop active operations and retry; preserve existing files before storage repair.");
        else
          host_connection::console().println("Run storageinfo to check free space and ls /spiffs to inspect the backup. Preserve existing files before retrying; do not format storage to resolve this error.");
      }
      if (success && operation == 2) {
        commandRebootAt = millis()+1000;
      }
    #else
      if (machine)
        machineResult(transaction_id, command, "error", "SD_NOT_SUPPORTED");
      else
        host_connection::console().println(F("Storage unavailable"));
    #endif
  }
  // GCOVR_EXCL_STOP

  #ifdef HAS_STORAGE
  else if (cmd_args.get(0) == "storageinfo") {
    host_connection::console().printf("Storage: backend=%s mounted=%s total=%u used=%u free=%u bytes\n",
      storageBackend(), sd_obj.supported ? "yes" : "no", (unsigned)storageTotalBytes(),
      (unsigned)storageUsedBytes(), (unsigned)(storageTotalBytes()-storageUsedBytes()));
  }
  #endif

  // Channel command
  else if (cmd_args.get(0) == CH_CMD) {
    // Search for channel set arg
    int ch_set = this->argSearch(&cmd_args, "-s");

    if (ch_set != -1) {
      double channel;
      if(!parseFiniteNumber(cmd_args.get(ch_set+1),channel)||floor(channel)!=channel||channel<1||channel>177){host_connection::console().println("Invalid channel. Use channel -s <valid regulatory channel>.");return;}
      wifi_scan_obj.set_channel = int(channel);
      wifi_scan_obj.changeChannel();
      host_connection::console().println(wifi_scan_obj.set_channel);
    }
    host_connection::console().println(wifi_scan_obj.set_channel);
  }
  // Clear APs
  else if (cmd_args.get(0) == CLEARAP_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a"); // APs
    int ss_sw = this->argSearch(&cmd_args, "-s"); // SSIDs
    int cl_sw = this->argSearch(&cmd_args, "-c"); // Stations

    if (ap_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.clearAPsMenu);
      #endif
      wifi_scan_obj.RunClearAPs();
    }

    if (ss_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.clearSSIDsMenu);
      #endif
      wifi_scan_obj.RunClearSSIDs();
    }

    if (cl_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.clearAPsMenu);
      #endif
      wifi_scan_obj.RunClearStations();
    }
  }

  else if (cmd_args.get(0) == UPLOAD_CMD) {
    #ifdef HAS_DIRECT_UPLOAD
      int dest_sw = this->argSearch(&cmd_args, "-d");
      String upload_dest_arg = cmd_args.get(dest_sw + 1);
      int upload_dest = -1;

      if (upload_dest_arg == "wdg")
        upload_dest = WDG_UPLOAD;
      else if (upload_dest_arg == "wigle")
        upload_dest = WIGLE_UPLOAD;
      else if (upload_dest_arg == "both")
        upload_dest = BOTH_UPLOAD;

      if (upload_dest > -1) {
        if (!wifi_scan_obj.joinSavedWiFi(false)) {
          host_connection::console().println(F("Failed to connect to saved WiFi"));
          return;
        }
        delay(1000);
        for (int i = 0; i < sd_obj.sd_files->size(); i++) {
          if (sd_obj.sd_files->get(i).startsWith("wardrive_") || sd_obj.sd_files->get(i).startsWith("wigle-")) {
            if (!sd_obj.sd_files->get(i).endsWith(".wigle") && !sd_obj.sd_files->get(i).endsWith(".wdg") && !sd_obj.sd_files->get(i).endsWith(".gpx")) {
              host_connection::console().println("Uploading " + sd_obj.sd_files->get(i) + "...");
              if (wifi_scan_obj.uploadFile("/" + sd_obj.sd_files->get(i), true, upload_dest)) {
                host_connection::console().println("Upload OK");
              } else {
                host_connection::console().println("WiGLE failed");
              }
            }
          }
        }
        WiFi.disconnect(true);
        delay(100);
        wifi_scan_obj.StartScan(WIFI_SCAN_OFF, TFT_RED);

        host_connection::console().println("Upload complete");
      }
    #else
      host_connection::console().println("Direct upload not supported");
    #endif
  }

  else if (cmd_args.get(0) == SETTINGS_CMD) {
    int ss_sw = this->argSearch(&cmd_args, "-s"); // Set setting
    int re_sw = this->argSearch(&cmd_args, "-r"); // Reset setting
    int en_sw = this->argSearch(&cmd_args, "enable"); // enable setting
    int da_sw = this->argSearch(&cmd_args, "disable"); // disable setting

    if (re_sw != -1) {
      settings_obj.createDefaultSettings(SPIFFS);
      host_connection::console().println("Settings reset to defaults");
      return;
    }

    if (ss_sw == -1) {
      settings_obj.printJsonSettings(settings_obj.getSettingsString());
    }
    else {
      bool result = false;
      String setting_name = cmd_args.get(ss_sw + 1);
      if (en_sw != -1)
        result = settings_obj.saveSetting<bool>(setting_name.c_str(), true);
      else if (da_sw != -1)
        result = settings_obj.saveSetting<bool>(setting_name.c_str(), false);
      else {host_connection::console().println(HELP_SETTINGS_CMD);return;}

      if (!result) {
        host_connection::console().print(F("Could not successfully update setting \""));
        host_connection::console().println(setting_name + "\"");
        return;
      }
      host_connection::console().println("Updated setting: " + setting_name);
    }
  }

  else if (cmd_args.get(0) == REBOOT_CMD) {
    host_connection::console().println("Reboot scheduled; reconnect after the device restarts.");
    commandRebootAt = millis()+1000;
    return;
  }

  //// WiFi/Bluetooth Scan/Attack commands
  if (!wifi_scan_obj.scanning() || wifi_scan_obj.currentScanMode==WIFI_CONNECTED || (name=="remoteid"&&cmd_args.size()>1&&(cmd_args.get(1)=="list"||cmd_args.get(1)=="clear"))) {
    // Dump pcap/log to serial too, valid for all scan/attack commands
    wifi_scan_obj.save_serial = this->argSearch(&cmd_args, "-serial") != -1;

    // Signal strength scan
    if (cmd_args.get(0) == SIGSTREN_CMD) {
      int bt_sw = this->argSearch(&cmd_args, "-b");
      int wf_sw = this->argSearch(&cmd_args, "-w");
      int st_sw = this->argSearch(&cmd_args, "-s");
      int at_sw = this->argSearch(&cmd_args, "-t");
      int fl_sw = this->argSearch(&cmd_args, "-f");
      int pn_sw = this->argSearch(&cmd_args, "-p");
      int ms_sw = this->argSearch(&cmd_args, "-m");
      if ((wf_sw > -1) && this->checkValueExists(&cmd_args, wf_sw)) {
        int targ_index = cmd_args.get(wf_sw + 1).toInt();
        if ((targ_index >= 0) && (targ_index < access_points->size())) {
          const AccessPoint& target = access_points->get(targ_index);
          wifi_scan_obj.setFoxHuntTarget(target.bssid, target.essid, target.rssi, target.channel, false);
          this->startScanFromCLI(WIFI_SCAN_SIG_STREN, TFT_GREEN, "Fox Hunt");
        }
      }
      else if ((st_sw > -1) && this->checkValueExists(&cmd_args, st_sw + 1)) {
        int ap_index = cmd_args.get(st_sw + 1).toInt();
        int station_index = cmd_args.get(st_sw + 2).toInt();
        if ((ap_index >= 0) && (ap_index < access_points->size()) &&
            (station_index >= 0) && (station_index < stations->size())) {
          const AccessPoint& target_ap = access_points->get(ap_index);
          bool belongs_to_ap = false;
          for (int i = 0; i < target_ap.stations->size(); i++) {
            if (target_ap.stations->get(i) == station_index) {
              belongs_to_ap = true;
              break;
            }
          }
          if (belongs_to_ap) {
            const Station& target = stations->get(station_index);
            wifi_scan_obj.setFoxHuntTarget(target.mac, macToString(target.mac), -128, target_ap.channel, false);
            this->startScanFromCLI(WIFI_SCAN_SIG_STREN, TFT_GREEN, "Station Fox Hunt");
          }
        }
      }
      else if ((bt_sw > -1) && this->checkValueExists(&cmd_args, bt_sw)) {
        int targ_index = cmd_args.get(bt_sw + 1).toInt();
        if ((targ_index >= 0) && (targ_index < ble_devices->size())) {
          const BleDevice& target = ble_devices->get(targ_index);
          wifi_scan_obj.setFoxHuntTarget(target.mac, target.name, target.rssi, 0, true, macToString(target.mac));
          this->startScanFromCLI(BT_SCAN_FOX_HUNT, TFT_CYAN, "Bluetooth Fox Hunt");
        }
      }
      else if ((at_sw > -1) && this->checkValueExists(&cmd_args, at_sw)) {
        int targ_index = cmd_args.get(at_sw + 1).toInt();
        if ((targ_index >= 0) && (targ_index < airtags->size())) {
          const AirTag& target = airtags->get(targ_index);
          uint8_t mac[6];
          convertMacStringToUint8(target.mac, mac);
          wifi_scan_obj.setFoxHuntTarget(mac, target.mac, target.rssi, 0, true, target.mac);
          this->startScanFromCLI(BT_SCAN_FOX_HUNT, TFT_CYAN, "FindMy Fox Hunt");
        }
      }
      else if ((fl_sw > -1) && this->checkValueExists(&cmd_args, fl_sw)) {
        int targ_index = cmd_args.get(fl_sw + 1).toInt();
        if ((targ_index >= 0) && (targ_index < flippers->size())) {
          const Flipper& target = flippers->get(targ_index);
          uint8_t mac[6];
          convertMacStringToUint8(target.mac, mac);
          wifi_scan_obj.setFoxHuntTarget(mac, target.name.length() ? target.name : target.mac, -128, 0, true, target.mac);
          this->startScanFromCLI(BT_SCAN_FOX_HUNT, TFT_CYAN, "Flipper Zero Fox Hunt");
        }
      }
      else if ((pn_sw > -1) && this->checkValueExists(&cmd_args, pn_sw)) {
        int targ_index = cmd_args.get(pn_sw + 1).toInt();
        if ((targ_index >= 0) && wifi_scan_obj.selectPineScanFoxTarget(targ_index))
          this->startScanFromCLI(WIFI_SCAN_SIG_STREN, TFT_GREEN, "WiFi Pineapple Fox Hunt");
      }
      else if ((ms_sw > -1) && this->checkValueExists(&cmd_args, ms_sw)) {
        int targ_index = cmd_args.get(ms_sw + 1).toInt();
        if ((targ_index >= 0) && wifi_scan_obj.selectMultiSSIDFoxTarget(targ_index))
          this->startScanFromCLI(WIFI_SCAN_SIG_STREN, TFT_GREEN, "MultiSSID Fox Hunt");
      }
      else
        host_connection::console().println(HELP_SIGSTREN_CMD);
    }
    // Packet count
    else if (cmd_args.get(0) == PACKET_COUNT_CMD) {
      this->startScanFromCLI(WIFI_SCAN_PACKET_RATE, TFT_ORANGE, "Packet Count Scan");
    }
    // Geofence configuration
    else if (cmd_args.get(0) == GEOFENCE_CMD) {
      if (cmd_args.size() == 2 && cmd_args.get(1) == "list") {
        bool any = false;
        for (uint8_t i = 0; i < MAX_GEOFENCES; i++) {
          GeofenceConfig fence;
          if (!settings_obj.loadGeofence(i, fence)) continue;
          any = true;
          host_connection::console().printf("%u: %s | %.6f, %.6f | %.2f mi\n", i + 1, fence.name.c_str(), fence.latitude, fence.longitude, fence.radiusMiles);
        }
        if (!any) host_connection::console().println(F("No geofences configured"));
      }
      else if (cmd_args.size() == 3 && cmd_args.get(1) == "clear") {
        const int slot = cmd_args.get(2).toInt();
        if (slot < 1 || slot > MAX_GEOFENCES) host_connection::console().println(F("Invalid slot; use 1-5"));
        else {
          settings_obj.clearGeofence(slot - 1);
          wifi_scan_obj.reloadGeofences();
          host_connection::console().printf("Geofence %d cleared\n", slot);
        }
      }
      else if (cmd_args.size() == 7 && cmd_args.get(1) == "set") {
        const int slot = cmd_args.get(2).toInt();
        double lat, lon, radius;
        GeofenceConfig fence;
        fence.enabled = true;
        fence.name = cmd_args.get(6);
        if (slot < 1 || slot > MAX_GEOFENCES || !parseFiniteNumber(cmd_args.get(3), lat) ||
            !parseFiniteNumber(cmd_args.get(4), lon) || !parseFiniteNumber(cmd_args.get(5), radius)) {
          host_connection::console().println(HELP_GEOFENCE_CMD);
        } else {
          fence.latitude = lat;
          fence.longitude = lon;
          fence.radiusMiles = radius;
          if (!settings_obj.saveGeofence(slot - 1, fence)) host_connection::console().println(F("Invalid geofence: check name, coordinates, and radius"));
          else {
            wifi_scan_obj.reloadGeofences();
            host_connection::console().printf("Geofence %d saved\n", slot);
          }
        }
      }
      else host_connection::console().println(HELP_GEOFENCE_CMD);
    }
    // Wardrive
    else if (cmd_args.get(0) == WARDRIVE_CMD) {
      #ifdef HAS_GPS
        if (gps_obj.getGpsModuleStatus()) {
          //int sta_sw = this->argSearch(&cmd_args, "-s");
          this->startScanFromCLI(WIFI_SCAN_WAR_DRIVE, TFT_GREEN, "Wardrive");
        }
      #else
        host_connection::console().println(F("GPS not supported"));
      #endif
    }
    // Karma
    else if (cmd_args.get(0) == KARMA_CMD) {
      int pr_sw = this->argSearch(&cmd_args, "-p");

      if (pr_sw == -1) {
        return;
      }

      int pr_index = cmd_args.get(pr_sw + 1).toInt();

      if ((pr_index < 0) || (pr_index > probe_req_ssids->size() - 1)) {
        return;
      }

      if (evil_portal_obj.setAP(probe_req_ssids->get(pr_index).essid)) {
        this->startScanFromCLI(WIFI_SCAN_EVIL_PORTAL, TFT_ORANGE, "Karma Attack");
        /*host_connection::console().println(STOPSCAN_CMD);
        #ifdef HAS_SCREEN
          display_obj.clearScreen();
          menu_function_obj.drawStatusBar();
        #endif
        wifi_scan_obj.StartScan(WIFI_SCAN_EVIL_PORTAL, TFT_ORANGE);*/
        wifi_scan_obj.setMac();
      }
      else {
        host_connection::console().println(F("Unable to set AP ESSID"));
        return;
      }

    }
    // AP Scan
    else if (cmd_args.get(0) == EVIL_PORTAL_CMD) {
      int cmd_sw = this->argSearch(&cmd_args, "-c");
      int html_sw = this->argSearch(&cmd_args, "-w");

      if (cmd_sw != -1) {
        String et_command = cmd_args.get(cmd_sw + 1);
        if (et_command == "start") {
          host_connection::console().print(F("Starting Evil Portal. Stop with "));
          host_connection::console().println(STOPSCAN_CMD);
          #ifdef HAS_SCREEN
            display_obj.clearScreen();
            menu_function_obj.drawStatusBar();
          #endif
          if (html_sw != -1) {
            String target_html_name = cmd_args.get(html_sw + 1);
            evil_portal_obj.target_html_name = target_html_name;
            evil_portal_obj.using_serial_html = false;
            host_connection::console().print(F("Set html file as "));
            host_connection::console().println(evil_portal_obj.target_html_name);
          }
          //else {
          //  evil_portal_obj.target_html_name = "index.html";
          //}
          wifi_scan_obj.StartScan(WIFI_SCAN_EVIL_PORTAL, TFT_MAGENTA);
        }
        else if (et_command == "reset") {
          
        }
        else if (et_command == "ack") {
          
        }
        else if (et_command == "sethtml") {
          String target_html_name = cmd_args.get(cmd_sw + 2);
          evil_portal_obj.target_html_name = target_html_name;
          evil_portal_obj.using_serial_html = false;
          host_connection::console().print(F("Set html file as "));
          host_connection::console().println(evil_portal_obj.target_html_name);
        }
        else if (et_command == "sethtmlstr") {
          evil_portal_obj.setHtmlFromSerial();
        }
        else if (et_command == "setap") {
          int target_ap_index = cmd_args.get(cmd_sw + 2).toInt();
          if ((target_ap_index >= 0) && (target_ap_index < access_points->size())) {
            evil_portal_obj.setAP(access_points->get(target_ap_index).essid);
            AccessPoint new_ap = access_points->get(target_ap_index);
            new_ap.selected = true;
            access_points->set(target_ap_index, new_ap);

            evil_portal_obj.ap_index = target_ap_index;
          }
        }
      }
    }
    else if (cmd_args.get(0) == "scancombined") {
      wifi_scan_obj.StartScan(WIFI_SCAN_AP_STA_BEACON, TFT_CYAN);
    }
    else if (cmd_args.get(0) == SCAN_ALL_CMD) {
      host_connection::console().print(F("Scanning for APs and Stations. Stop with "));
      host_connection::console().println(STOPSCAN_CMD);
      wifi_scan_obj.StartScan(WIFI_SCAN_AP_STA, TFT_MAGENTA);
    }
    // Raw sniff
    else if (cmd_args.get(0) == SNIFF_RAW_CMD)
      this->startScanFromCLI(WIFI_SCAN_RAW_CAPTURE, TFT_WHITE, "Raw sniff");
    // Beacon sniff
    else if (cmd_args.get(0) == SNIFF_BEACON_CMD) {
      this->startScanFromCLI(WIFI_SCAN_AP, TFT_MAGENTA, "Beacon sniff");
    }
    // SAE sniff
    else if (cmd_args.get(0) == SNIFF_SAE_CMD) {
      this->startScanFromCLI(WIFI_SCAN_SAE_COMMIT, TFT_MAGENTA, "Commit sniff");
    }
    // Probe sniff
    else if (cmd_args.get(0) == SNIFF_PROBE_CMD) {
      this->startScanFromCLI(WIFI_SCAN_PROBE, TFT_MAGENTA, "Probe sniff");
    }
    // Deauth sniff
    else if (cmd_args.get(0) == SNIFF_DEAUTH_CMD) {
      this->startScanFromCLI(WIFI_SCAN_DEAUTH, TFT_RED, "Deauth sniff");
    }
    // Pwn sniff
    else if (cmd_args.get(0) == SNIFF_PWN_CMD) {
      this->startScanFromCLI(WIFI_SCAN_PWN, TFT_MAGENTA, "Pwnagotchi sniff");
    }
    // PineScan sniff
    else if (cmd_args.get(0) == SNIFF_PINESCAN_CMD) {
      this->startScanFromCLI(WIFI_SCAN_PINESCAN, TFT_MAGENTA, "Pinescan sniff");
    }
    // MultiSSID sniff
    else if (cmd_args.get(0) == SNIFF_MULTISSID_CMD) {
      this->startScanFromCLI(WIFI_SCAN_MULTISSID, TFT_MAGENTA, "MultiSSID sniff");
    }
    // PMKID sniff
    else if (cmd_args.get(0) == SNIFF_PMKID_CMD) {
      int ch_sw = this->argSearch(&cmd_args, "-c");
      int d_sw = this->argSearch(&cmd_args, "-d"); // Deauth for pmkid
      int l_sw = this->argSearch(&cmd_args, "-l"); // Only run on list

      if (l_sw != -1) {
        if (!wifi_scan_obj.filterActive()) {
          host_connection::console().println("You don't have any targets selected. Use " + (String)SEL_CMD);
          return;
        }
      }
      
      if (ch_sw != -1) {
        wifi_scan_obj.set_channel = cmd_args.get(ch_sw + 1).toInt();
        wifi_scan_obj.changeChannel();
        host_connection::console().println("Set channel: " + (String)wifi_scan_obj.set_channel);
        
      }

      if (d_sw == -1) {
        host_connection::console().println("Starting PMKID sniff on channel " + (String)wifi_scan_obj.set_channel + ". Stop with " + (String)STOPSCAN_CMD);
        wifi_scan_obj.StartScan(WIFI_SCAN_EAPOL, TFT_VIOLET);
      }
      else if ((d_sw != -1) && (l_sw != -1)) {
        host_connection::console().println("Starting TARGETED PMKID sniff with deauthentication on channel " + (String)wifi_scan_obj.set_channel + ". Stop with " + (String)STOPSCAN_CMD);
        wifi_scan_obj.StartScan(WIFI_SCAN_ACTIVE_LIST_EAPOL, TFT_VIOLET);
      }
      else {
        host_connection::console().println("Starting PMKID sniff with deauthentication on channel " + (String)wifi_scan_obj.set_channel + ". Stop with " + (String)STOPSCAN_CMD);
        wifi_scan_obj.StartScan(WIFI_SCAN_ACTIVE_EAPOL, TFT_VIOLET);
      }
    }    
    // MAC Tracking
    else if (cmd_args.get(0) == MAC_TRACK_CMD) {
      this->startScanFromCLI(WIFI_SCAN_DETECT_FOLLOW, TFT_MAGENTA, "MAC Tracker");
      /*host_connection::console().println(STOPSCAN_CMD);
      #ifdef HAS_SCREEN
        display_obj.clearScreen();
        menu_function_obj.drawStatusBar();
      #endif
      wifi_scan_obj.StartScan(WIFI_SCAN_DETECT_FOLLOW, TFT_MAGENTA);*/
    }


  //// MAC Address commands    (Added by H4W9_4)
	// Generate random MAC for AP
    if (cmd_args.get(0) == MAC_CMD_A) {
      #ifdef HAS_SCREEN
        display_obj.clearScreen();
        menu_function_obj.drawStatusBar();
      #endif
      wifi_scan_obj.RunGenerateRandomMac(true);
    }

	  // Generate random MAC for STA
	  else if (cmd_args.get(0) == MAC_CMD_B) {
      //host_connection::console().println("Setting STA MAC: " + macToString(this->sta_mac));
      #ifdef HAS_SCREEN
        display_obj.clearScreen();
        menu_function_obj.drawStatusBar();
      #endif
      wifi_scan_obj.RunGenerateRandomMac(false);
    }

	  // Clone MAC for AP
	  else if (cmd_args.get(0) == MAC_CMD_C) {
      int ap_sw = this->argSearch(&cmd_args, "-a"); // APs
      
      if (ap_sw == -1) {
        return;
      }

      int ap_index = cmd_args.get(ap_sw + 1).toInt();

      if ((ap_index < 0) || (ap_index > access_points->size() - 1)) {
        return;
      }
      
      if (ap_sw != -1) {
        #ifdef HAS_SCREEN
          display_obj.clearScreen();
          menu_function_obj.drawStatusBar();
        #endif
        int filter_ap = cmd_args.get(ap_sw + 1).toInt();
        wifi_scan_obj.RunSetMac(access_points->get(filter_ap).bssid, true);
      }
    }

    // Clone MAC for STA
	  else if (cmd_args.get(0) == MAC_CMD_D) {
      int cl_sw = this->argSearch(&cmd_args, "-s"); // Stations
            
      if (cl_sw == -1)
        return;

      int sta_index = cmd_args.get(cl_sw + 1).toInt();

      if ((sta_index < 0) || (sta_index > stations->size() - 1)) {
        return;
      }

      if (cl_sw != -1) {
        #ifdef HAS_SCREEN
          display_obj.clearScreen();
          menu_function_obj.drawStatusBar();
        #endif
        int filter_sta = cmd_args.get(cl_sw + 1).toInt();
        wifi_scan_obj.RunSetMac(stations->get(filter_sta).mac, false);
      }
    }
    //// End MAC Address commands    (Added by H4W9_4)

    
    //// WiFi attack commands
    // attack
    if (cmd_args.get(0) == ATTACK_CMD) {
      int attack_type_switch = this->argSearch(&cmd_args, "-t"); // Required
      int list_beacon_sw = this->argSearch(&cmd_args, "-l");
      int rand_beacon_sw = this->argSearch(&cmd_args, "-r");
      int ap_beacon_sw = this->argSearch(&cmd_args, "-a");
      int src_addr_sw = this->argSearch(&cmd_args, "-s");
      int dst_addr_sw = this->argSearch(&cmd_args, "-d");
      int targ_sw = this->argSearch(&cmd_args, "-c");
  
      if (attack_type_switch == -1)
        return;
      else {
        String attack_type = cmd_args.get(attack_type_switch + 1);
  
        // Branch on attack type
        if (attack_type == ATTACK_TYPE_DEAUTH) {
          // Default to broadcast
          if ((dst_addr_sw == -1) && (targ_sw == -1)) {
            host_connection::console().println(F("Sending to broadcast..."));
            wifi_scan_obj.dst_mac = "ff:ff:ff:ff:ff:ff";
          }
          // Dest addr specified
          else if (dst_addr_sw != -1) {
            wifi_scan_obj.dst_mac = cmd_args.get(dst_addr_sw + 1);
            host_connection::console().println("Sending to " + wifi_scan_obj.dst_mac + "...");
          }
          // Station list specified
          else if (targ_sw != -1)
            host_connection::console().println(F("Sending to Station list"));

          // Source addr not specified
          if (src_addr_sw == -1) {
            if (!wifi_scan_obj.filterActive()) {
              host_connection::console().println("You don't have any targets selected. Use " + (String)SEL_CMD);
              return;
            }
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            host_connection::console().println("Starting Deauthentication attack. Stop with " + (String)STOPSCAN_CMD);
            // Station list not specified
            if (targ_sw == -1)
              wifi_scan_obj.StartScan(WIFI_ATTACK_DEAUTH, TFT_RED);
            // Station list specified
            else
              wifi_scan_obj.StartScan(WIFI_ATTACK_DEAUTH_TARGETED, TFT_ORANGE);
          }
          // Source addr specified
          else {
            String src_mac_str = cmd_args.get(src_addr_sw + 1);
            sscanf(src_mac_str.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", 
              &wifi_scan_obj.src_mac[0], &wifi_scan_obj.src_mac[1], &wifi_scan_obj.src_mac[2], &wifi_scan_obj.src_mac[3], &wifi_scan_obj.src_mac[4], &wifi_scan_obj.src_mac[5]);

            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            host_connection::console().println("Starting Manual Deauthentication attack. Stop with " + (String)STOPSCAN_CMD);
            wifi_scan_obj.StartScan(WIFI_ATTACK_DEAUTH_MANUAL, TFT_RED);            
          }
        }
        else if (attack_type == ATTACK_TYPE_BM) {
          // Attack all
          if (targ_sw == -1) {
            this->startScanFromCLI(WIFI_ATTACK_BAD_MSG, TFT_RED, "Bad Msg attack against all stations");
            /*Serial.prinln((String)STOPSCAN_CMD);
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            wifi_scan_obj.StartScan(WIFI_ATTACK_BAD_MSG, TFT_RED);*/
          }
          // Target clients
          else {
            this->startScanFromCLI(WIFI_ATTACK_BAD_MSG_TARGETED, TFT_YELLOW, "targeted Bad Msg attack");
            /*host_connection::console().println(STOPSCAN_CMD);
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            wifi_scan_obj.StartScan(WIFI_ATTACK_BAD_MSG_TARGETED, TFT_YELLOW);*/
          }
        }
        else if (attack_type == ATTACK_TYPE_S) {
          // Attack all
          if (targ_sw == -1) {
            this->startScanFromCLI(WIFI_ATTACK_SLEEP, TFT_RED, "Sleep attack against all stations");
            /*host_connection::console().println(STOPSCAN_CMD);
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            wifi_scan_obj.StartScan(WIFI_ATTACK_SLEEP, TFT_RED);*/
          }
          // Target clients
          else {
            this->startScanFromCLI(WIFI_ATTACK_SLEEP_TARGETED, TFT_MAGENTA, "targeted Sleep attack");
            /*host_connection::console().println(STOPSCAN_CMD);
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            wifi_scan_obj.StartScan(WIFI_ATTACK_SLEEP_TARGETED, TFT_MAGENTA);*/
          }
        }
        else if (attack_type == ATTACK_TYPE_BEACON) {
          // spam by list
          if (list_beacon_sw != -1) {
            if (!this->hasSSIDs()) {
              host_connection::console().println("You don't have any SSIDs in your list. Use " + (String)SSID_CMD);
              return;
            }
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            host_connection::console().println("Starting Beacon list spam. Stop with " + (String)STOPSCAN_CMD);
            wifi_scan_obj.StartScan(WIFI_ATTACK_BEACON_LIST, TFT_RED);
          }
          // spam with random
          else if (rand_beacon_sw != -1) {
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            host_connection::console().println("Starting random Beacon spam. Stop with " + (String)STOPSCAN_CMD);
            wifi_scan_obj.StartScan(WIFI_ATTACK_BEACON_SPAM, TFT_ORANGE);
          }
          // Spam from AP list
          else if (ap_beacon_sw != -1) {
            if (!wifi_scan_obj.filterActive()) {
              host_connection::console().println("You don't have any targets selected. Use " + (String)SEL_CMD);
              return;
            }
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            host_connection::console().println("Starting Targeted AP Beacon spam. Stop with " + (String)STOPSCAN_CMD);
            wifi_scan_obj.StartScan(WIFI_ATTACK_AP_SPAM, TFT_MAGENTA);
          }
        }
        else if (attack_type == ATTACK_TYPE_PROBE) {
          if (!wifi_scan_obj.filterActive()) {
            host_connection::console().println("You don't have any targets selected. Use " + (String)SEL_CMD);
            return;
          }
          this->startScanFromCLI(WIFI_ATTACK_AUTH, TFT_RED, "Probe spam");
        }
        else if (attack_type == ATTACK_TYPE_RR) {
          this->startScanFromCLI(WIFI_ATTACK_RICK_ROLL, TFT_YELLOW, "Rick Roll Beacon spam");
        }
        else if (attack_type == ATTACK_TYPE_FUNNY) {
          this->startScanFromCLI(WIFI_ATTACK_FUNNY_BEACON, TFT_CYAN, "Funny SSID Beacon spam");
        }
        else if (attack_type == ATTACK_TYPE_SAE) {
          this->startScanFromCLI(WIFI_ATTACK_SAE_COMMIT, TFT_CYAN, "SAE Commit spam");
        }
        else if (attack_type == ATTACK_TYPE_CSA) {
          this->startScanFromCLI(WIFI_ATTACK_CSA, TFT_CYAN, "Channel Switch Announcement attack");
        }
        else if (attack_type == ATTACK_TYPE_QUIET) {
          this->startScanFromCLI(WIFI_ATTACK_QUIET, TFT_CYAN, "Quite Time attack");
        }
        else {
          return;
        }
      }
    }

    if (cmd_args.get(0) == REMOTE_ID_CMD) {
      String action = cmd_args.size() > 1 ? cmd_args.get(1) : "scan";
      action.toLowerCase();
      if (action == "scan") {
        this->startScanFromCLI(REMOTE_ID_SCAN_ALL, TFT_CYAN, " Remote ID scan");
      }
      else if (action == "list") {
        host_connection::console().println(F("Remote ID devices:"));
        for (size_t i = 0; i < wifi_scan_obj.remoteIdCount(); ++i)
          host_connection::console().println(String(i) + " " + wifi_scan_obj.remoteIdLabel(i));
      }
      else if (action == "track" && cmd_args.size() > 2) {
        const int index = cmd_args.get(2).toInt();
        if (index < 0 || !wifi_scan_obj.selectRemoteIdTarget(index)) {
          host_connection::console().println(F("Invalid Remote ID index"));
        } else {
          this->startScanFromCLI(REMOTE_ID_SCAN_TARGET, TFT_MAGENTA,
                                 " Remote ID target scan");
        }
      }
      else if (action == "clear") {
        wifi_scan_obj.clearRemoteIds();
        host_connection::console().println(F("Remote ID list cleared"));
      }
      else {
        host_connection::console().println(HELP_REMOTE_ID_CMD);
      }
    }

    //// Bluetooth scan/attack commands
    // Bluetooth scan
    if (cmd_args.get(0) == BT_SNIFF_CMD) {
      #ifdef HAS_BT
        int bt_type_sw = this->argSearch(&cmd_args, "-t");

        // Specifying type of bluetooth sniff
        if (bt_type_sw != -1) {
          String bt_type = cmd_args.get(bt_type_sw + 1);

          bt_type.toLowerCase();

          // Airtag sniff
          if (bt_type == "airtag") {
            this->startScanFromCLI(BT_SCAN_AIRTAG, TFT_WHITE, "Airtag sniff");
          }
          else if (bt_type == "ibeacon") {
            this->startScanFromCLI(BT_SCAN_IBEACON, TFT_CYAN, "iBeacon sniff");
          }
          else if (bt_type == "flipper") {
            this->startScanFromCLI(BT_SCAN_FLIPPER, TFT_ORANGE, "Flipper sniff");
          }
          else if (bt_type == "flock") {
            this->startScanFromCLI(BT_SCAN_FLOCK, TFT_ORANGE, "Flock sniff");
          }
          else if (bt_type == "meta") {
            this->startScanFromCLI(BT_SCAN_RAYBAN, TFT_ORANGE, "Meta sniff");
          }
        }
        // General bluetooth sniff
        else {
          this->startScanFromCLI(BT_SCAN_ALL, TFT_GREEN, "Bluetooth scan");
        }
      #else
        host_connection::console().println(F("Bluetooth not supported"));
      #endif
    }
    else if (cmd_args.get(0) == BT_SPOOFAT_CMD) {
      int at_sw = this->argSearch(&cmd_args, "-t");
      if (at_sw != -1) {
        #ifdef HAS_BT
          int target_mac = cmd_args.get(at_sw + 1).toInt();
          if (target_mac < airtags->size()) {
            for (int i = 0; i < airtags->size(); i++) {
              AirTag at = airtags->get(i);
              if (i == target_mac)
                at.selected = true;
              else
                at.selected = false;
              airtags->set(i, at);
            }
            this->startScanFromCLI(BT_SPOOF_AIRTAG, TFT_WHITE, "Spoofing Airtag");
          }
          else {
            return;
          }
        #endif
      }
    }
    else if (cmd_args.get(0) == BT_FINDMY_CMD) {
      #ifdef HAS_NIMBLE_2
      int index_sw = this->argSearch(&cmd_args, "-t");

      if (index_sw != -1) {
        int targ_index = cmd_args.get(index_sw + 1).toInt();
        if (targ_index < airtags->size()) {
          for (int x = 0; x < airtags->size(); x++) {
            AirTag new_atx = airtags->get(x);
            if (x != targ_index)
              new_atx.selected = false;
            else
              new_atx.selected = true;
            airtags->set(x, new_atx);
          }

          wifi_scan_obj.executeFindMySound(false);
        }
      }
      #endif
    }
    else if (cmd_args.get(0) == BT_SPAM_CMD) {
      int bt_type_sw = this->argSearch(&cmd_args, "-t");
      if (bt_type_sw != -1) {
        String bt_type = cmd_args.get(bt_type_sw + 1);

        #ifdef HAS_BT
          if (bt_type == "sourapple") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_SOUR_APPLE, TFT_GREEN, "Sour Apple attack");
            #endif
          }
          else if (bt_type == "applejuice") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_APPLE_JUICE, TFT_GREEN, "Apple Juice attack");
            #endif
          }
          else if (bt_type == "windows") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_SWIFTPAIR_SPAM, TFT_CYAN, "Swiftpair Spam attack");
            #endif
          }
          else if (bt_type == "samsung") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_SAMSUNG_SPAM, TFT_CYAN, "Samsung Spam attack");
            #endif
          }
          else if (bt_type == "google") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_GOOGLE_SPAM, TFT_CYAN, "Google Spam attack");
            #endif
          }
          else if (bt_type == "flipper") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_FLIPPER_SPAM, TFT_ORANGE, "Flipper Spam attack");
            #endif
          }
          else if (bt_type == "all") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_SPAM_ALL, TFT_MAGENTA, "BT Spam All attack");
            #endif
          }
        #else
          host_connection::console().println(F("Bluetooth not supported"));
        #endif
      }
    }
    // Bluetooth CC Skimmer scan
    else if (cmd_args.get(0) == BT_SKIM_CMD) {
      #ifdef HAS_BT
        this->startScanFromCLI(BT_SCAN_SKIMMERS, TFT_MAGENTA, "Bluetooth CC Skimmer scan");
      #else
        host_connection::console().println(F("Bluetooth not supported"));
      #endif
    }

    // Brightness command
    else if (cmd_args.get(0) == BRIGHTNESS_CMD) {
      #ifndef HAS_MINI_SCREEN
        int c_arg = this->argSearch(&cmd_args, "-c");
        int s_arg = this->argSearch(&cmd_args, "-s");
        if (c_arg != -1) {
          brightnessCycle();
        } else if (s_arg != -1) {
          uint8_t lvl = cmd_args.get(s_arg + 1).toInt();
          if (lvl < 10) {
            extern void brightnessSave(uint8_t level);
            brightnessSave(lvl);
            host_connection::console().print(F("[Brightness] Set to level "));
            host_connection::console().println(lvl);
          } else {
            host_connection::console().println(F("Level must be 0-9"));
          }
        } else {
          host_connection::console().print(F("[Brightness] Current level: "));
          host_connection::console().println(getBrightnessLevel());
        }
      #endif
    }

    // Wardrive POI command
    else if (cmd_args.get(0) == WARDRIVEPOI_CMD) {
      if (wifi_scan_obj.currentScanMode == WIFI_SCAN_WAR_DRIVE ||
          wifi_scan_obj.currentScanMode == WIFI_SCAN_STATION_WAR_DRIVE) {
        if (cmd_args.size() > 1) {
          // Join remaining args as label
          String label = "";
          for (int i = 1; i < cmd_args.size(); i++) {
            if (i > 1) label += " ";
            label += cmd_args.get(i);
          }
          wifi_scan_obj.tagPOI(label.c_str());
        } else {
          wifi_scan_obj.tagPOI(nullptr);
        }
      } 
      //else {
      //  host_connection::console().println(F("No active wardrive. Start wardrive first."));
      //}
    }

    // Update command
    if (cmd_args.get(0) == UPDATE_CMD) {
      int sd_sw = this->argSearch(&cmd_args, "-s"); // SD Update
      if (sd_sw != -1) {
        #ifdef HAS_STORAGE
          if (!sd_obj.supported) {
            host_connection::console().println(F("Storage is not mounted."));
            return;
          }
          wifi_scan_obj.currentScanMode = OTA_UPDATE;
          sd_obj.runUpdate();
          // Successful updates restart. A returned failure must release busy state.
          wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
        #endif
      } else {
        host_connection::console().println("Usage: update -s; place a compatible verified image at /update.bin, or use Dashboard USB OTA. -w is not an upstream CLI update option.");
      }
    }
  }

  if (wifi_scan_obj.wifi_connected) {
    // Ping Scan
    if (cmd_args.get(0) == PING_CMD) {
      this->startScanFromCLI(WIFI_PING_SCAN, TFT_GREEN, "Ping Scan");
    }

    // ARP discovery uses the active station netif on both legacy and C5
    // dual-band hardware.
    if (cmd_args.get(0) == ARP_SCAN_CMD) {
      this->startScanFromCLI(WIFI_ARP_SCAN, TFT_CYAN, "ARP Scan");
    }

    // GPS POI
    if (cmd_args.get(0) == GPS_POI_CMD) {
      #ifdef HAS_GPS
        int start_sw = this->argSearch(&cmd_args, "-s");
        int mark_sw = this->argSearch(&cmd_args, "-m");
        int end_sw = this->argSearch(&cmd_args, "-e");

        if (start_sw != -1) {
          wifi_scan_obj.StartScan(GPS_POI, TFT_CYAN);
          wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
          #ifdef HAS_SCREEN
            menu_function_obj.changeMenu(&menu_function_obj.gpsPOIMenu);
          #endif
        }
        else if (mark_sw != -1) {
          wifi_scan_obj.currentScanMode = GPS_POI;
          #ifdef HAS_SCREEN
            display_obj.tft.setCursor(0, TFT_HEIGHT / 2);
            display_obj.clearScreen();
          #endif
          if (wifi_scan_obj.RunGPSInfo(true, false, true)) {
            #ifdef HAS_SCREEN
              display_obj.showCenterText("POI Logged", TFT_HEIGHT / 2);
            #endif
          }
          else {
            #ifdef HAS_SCREEN
              display_obj.showCenterText("POI Log Failed", TFT_HEIGHT / 2);
            #endif
          }
          wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
          delay(2000);
          //wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
          #ifdef HAS_SCREEN
            menu_function_obj.changeMenu(&menu_function_obj.gpsPOIMenu);
          #endif
        }
        else if (end_sw != -1) {
          wifi_scan_obj.currentScanMode = GPS_POI;
          wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
          #ifdef HAS_SCREEN
            menu_function_obj.changeMenu(menu_function_obj.gpsPOIMenu.parentMenu);
          #endif
        }
      #endif
    }

    // Port Scan
    if (cmd_args.get(0) == PORT_SCAN_CMD) {
      int all_sw = this->argSearch(&cmd_args, "-a");
      int ip_sw = this->argSearch(&cmd_args, "-t");
      int port_sw = this->argSearch(&cmd_args, "-s");

      // Check they specified ip index
      if (ip_sw != -1) {
        int ip_index = cmd_args.get(ip_sw + 1).toInt();

        // Check provided index is in list
        if (ip_index < ipList->size()) {

          // Full port scan
          if (all_sw != -1) {
            wifi_scan_obj.current_scan_ip = ipList->get(ip_index);
            String msg = "Selected: " + ipList->get(ip_index).toString();
            this->startScanFromCLI(WIFI_PORT_SCAN_ALL, TFT_BLUE, msg.c_str());
          }
        }
        else {
          return;
        }
      }
      else if (port_sw != -1) {
        String port_name = cmd_args.get(port_sw + 1);
        port_name.toUpperCase();
        uint8_t target_mode = 0;
        if (port_name == "SSH")
          target_mode = WIFI_SCAN_SSH;
        else if (port_name == "TELNET")
          target_mode = WIFI_SCAN_TELNET;
        else if (port_name == "DNS")
          target_mode = WIFI_SCAN_DNS;
        else if (port_name == "HTTP")
          target_mode = WIFI_SCAN_HTTP;
        else if (port_name == "SMTP")
          target_mode = WIFI_SCAN_SMTP;
        else if (port_name == "HTTPS")
          target_mode = WIFI_SCAN_HTTPS;
        else if (port_name == "RDP")
          target_mode = WIFI_SCAN_RDP;

        if (target_mode != 0) {
          this->startScanFromCLI(target_mode, TFT_CYAN, "port scan");
        }
      }
    }
  }


  int count_selected = 0;
  //// WiFi aux commands
  // List access points
  if (cmd_args.get(0) == LIST_AP_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int ss_sw = this->argSearch(&cmd_args, "-s");
    int cl_sw = this->argSearch(&cmd_args, "-c");
    int at_sw = this->argSearch(&cmd_args, "-t");
    int ip_sw = this->argSearch(&cmd_args, "-i");
    int pr_sw = this->argSearch(&cmd_args, "-p");
    int bt_sw = this->argSearch(&cmd_args, "-b");
    int fl_sw = this->argSearch(&cmd_args, "-f");
    int pn_sw = this->argSearch(&cmd_args, "-x");
    int ms_sw = this->argSearch(&cmd_args, "-m");

    // List APs
    if (ap_sw != -1) {
      for (int i = 0; i < access_points->size(); i++) {
        AccessPoint access_point = access_points->get(i);
        if (access_point.selected) {
          host_connection::console().println("[" + (String)i + "][CH:" + (String)access_point.channel + "] " + access_point.essid + " " + (String)access_point.rssi + " (selected)");
          count_selected += 1;
        } 
        else
          host_connection::console().println("[" + (String)i + "][CH:" + (String)access_point.channel + "] " + access_point.essid + " " + (String)access_point.rssi);
      }
      this->showCounts(count_selected);
    }
    else if (bt_sw != -1) {
      if(!ble_devices->size())host_connection::console().println("0 records");
      for (int i = 0; i < ble_devices->size(); i++) {
        BleDevice ble_device = ble_devices->get(i);
        host_connection::console().println("[" + (String)i + "][RSSI:" + (String)ble_device.rssi + "] " + ble_device.name);
      }
    }
    else if (fl_sw != -1) {
      if(!flippers->size())host_connection::console().println("0 records");
      for (int i = 0; i < flippers->size(); i++) {
        host_connection::console().println("[" + (String)i + "]MAC: " + flippers->get(i).mac + " " + flippers->get(i).name);
      }
    }
    else if (pn_sw != -1) {
      if(!wifi_scan_obj.getPineScanCount())host_connection::console().println("0 records");
      for (size_t i = 0; i < wifi_scan_obj.getPineScanCount(); i++)
        host_connection::console().println("[" + (String)i + "] " + wifi_scan_obj.getPineScanLabel(i));
    }
    else if (ms_sw != -1) {
      if(!wifi_scan_obj.getMultiSSIDCount())host_connection::console().println("0 records");
      for (size_t i = 0; i < wifi_scan_obj.getMultiSSIDCount(); i++)
        host_connection::console().println("[" + (String)i + "] " + wifi_scan_obj.getMultiSSIDLabel(i));
    }
    // List IPs
    else if (ip_sw != -1) {
      if(!ipList->size())host_connection::console().println("0 records");
      for (int i = 0; i < ipList->size(); i++) {
        host_connection::console().println("[" + (String)i + "] " + ipList->get(i).toString());
      }
    }
    // List Probes
    else if (pr_sw != -1) {
      if(!probe_req_ssids->size())host_connection::console().println("0 records");
      for (int i = 0; i < probe_req_ssids->size(); i++) {
        host_connection::console().println("[" + (String)i + "] " + probe_req_ssids->get(i).essid);
      }
    }
    // List SSIDs
    else if (ss_sw != -1) {
      for (int i = 0; i < ssids->size(); i++) {
        if (ssids->get(i).selected) {
          host_connection::console().println("[" + (String)i + "] " + ssids->get(i).essid + " (selected)");
          count_selected += 1;
        } 
        else
          host_connection::console().println("[" + (String)i + "] " + ssids->get(i).essid);
      }
      this->showCounts(count_selected);
    }
    // List Stations
    else if (cl_sw != -1) {
      char sta_mac[] = "00:00:00:00:00:00";
      for (int x = 0; x < access_points->size(); x++) {
        AccessPoint access_point = access_points->get(x);
        host_connection::console().println("[" + (String)x + "] " + access_point.essid + " " + (String)access_point.rssi + ":");
        for (int i = 0; i < access_point.stations->size(); i++) {
          Station station = stations->get(access_point.stations->get(i));
          wifi_scan_obj.getMAC(sta_mac, station.mac, 0);
          if (station.selected) {
            host_connection::console().print("  [" + (String)access_point.stations->get(i) + "] ");
            host_connection::console().print(sta_mac);
            host_connection::console().println(F(" (selected)"));
            count_selected += 1;
          }
          else {
            host_connection::console().print("  [" + (String)access_point.stations->get(i) + "] ");
            host_connection::console().println(sta_mac);
          }
        }
      }
      this->showCounts(count_selected);
    }
    // List airtags
    else if (at_sw != -1) {
      if(!airtags->size())host_connection::console().println("0 records");
      for (int i = 0; i < airtags->size(); i++) {
        host_connection::console().println("[" + (String)i + "]MAC: " + airtags->get(i).mac);
      }
    }
  }
  else if (cmd_args.get(0) == INFO_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");

    if (ap_sw != -1) {
      int filter_ap = cmd_args.get(ap_sw + 1).toInt();
      wifi_scan_obj.RunAPInfo(filter_ap, false);
    }
    else {
      #ifdef HAS_SCREEN
        wifi_scan_obj.currentScanMode = SHOW_INFO;
        menu_function_obj.changeMenu(&menu_function_obj.infoMenu);
      #endif
      wifi_scan_obj.RunInfo();
    }
  }
  else if (cmd_args.get(0) == JOIN_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int pw_sw = this->argSearch(&cmd_args, "-p");
    int s_sw  = this->argSearch(&cmd_args, "-s");

    if ((ap_sw != -1) && (pw_sw != -1)) {
      int index = cmd_args.get(ap_sw + 1).toInt();
      String password = cmd_args.get(pw_sw + 1);
      AccessPoint access_point = access_points->get(index);
      host_connection::console().println("Using SSID: " + (String)access_point.essid);
      //wifi_scan_obj.currentScanMode = LV_JOIN_WIFI;
      //wifi_scan_obj.StartScan(LV_JOIN_WIFI, TFT_YELLOW); 
      wifi_scan_obj.joinWiFi(access_point.essid, password, false);
      #ifdef HAS_SCREEN
        #ifdef HAS_MINI_KB
          menu_function_obj.changeMenu(menu_function_obj.current_menu);
        #endif
      #endif
    }
    else if (s_sw != -1) {
      if (settings_obj.getSavedWifiCount() > 0) {
        wifi_scan_obj.joinSavedWiFi(false);
        #ifdef HAS_SCREEN
          menu_function_obj.changeMenu(menu_function_obj.current_menu);
        #endif
      }
      else {
        host_connection::console().println(F("There are no saved WiFi credentials"));
      }
    }
    else {
      host_connection::console().println(F("You did not provide the proper args"));
      return;
    }
  }
  // Select access points or stations
  else if (cmd_args.get(0) == SEL_CMD) {
    // Get switches
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int ss_sw = this->argSearch(&cmd_args, "-s");
    int cl_sw = this->argSearch(&cmd_args, "-c");
    int filter_sw = this->argSearch(&cmd_args, "-f");

    count_selected = 0;
    int count_unselected = 0;
    // select Access points
    if (ap_sw != -1) {

      // If the filters parameter was specified
      if (filter_sw != -1) {
        String filter_ap = cmd_args.get(filter_sw + 1);
        this->filterAccessPoints(filter_ap);
      } else {
        // Get list of indices
        LinkedList<String> ap_index = this->parseCommand(cmd_args.get(ap_sw + 1), ",");

        // Select ALL APs
        if (cmd_args.get(ap_sw + 1) == "all") {
          for (int i = 0; i < access_points->size(); i++) {
            AccessPoint access_point = access_points->get(i);
            if (access_point.selected) {
              // Unselect "selected" ap
              AccessPoint new_ap = access_point;
              new_ap.selected = false;
              access_points->set(i, new_ap);
              count_unselected += 1;
            }
            else {
              // Select "unselected" ap
              AccessPoint new_ap = access_point;
              new_ap.selected = true;
              access_points->set(i, new_ap);
              count_selected += 1;
            }
          }
          this->showCounts(count_selected, count_unselected);
        }
        // Select specific APs
        else {
          // Mark APs as selected
          for (int i = 0; i < ap_index.size(); i++) {
            int index = ap_index.get(i).toInt();
            AccessPoint access_point = access_points->get(index);
            if (!this->inRange(access_points->size(), index)) {
              host_connection::console().print(F("Index not in range: "));
              host_connection::console().println(index);
              continue;
            }
            if (access_point.selected) {
              // Unselect "selected" ap
              AccessPoint new_ap = access_point;
              new_ap.selected = false;
              access_points->set(index, new_ap);
              count_unselected += 1;
            }
            else {
              // Select "unselected" ap
              AccessPoint new_ap = access_point;
              new_ap.selected = true;
              access_points->set(index, new_ap);
              count_selected += 1;
            }
          }
          this->showCounts(count_selected, count_unselected);
        }
      }
    }
    else if (cl_sw != -1) {
      LinkedList<String> sta_index = this->parseCommand(cmd_args.get(cl_sw + 1), ",");
      
      // Select all Stations
      if (cmd_args.get(cl_sw + 1) == "all") {
        for (int i = 0; i < stations->size(); i++) {
          Station station = stations->get(i);
          if (station.selected) {
            // Unselect "selected" ap
            Station new_sta = station;
            new_sta.selected = false;
            stations->set(i, new_sta);
            count_unselected += 1;
          }
          else {
            // Select "unselected" ap
            Station new_sta = station;
            new_sta.selected = true;
            stations->set(i, new_sta);
            count_selected += 1;
          }
        }
        this->showCounts(count_selected, count_unselected);
      }
      // Select specific Stations
      else {
        // Mark Stations as selected
        for (int i = 0; i < sta_index.size(); i++) {
          int index = sta_index.get(i).toInt();
          Station station = stations->get(index);
          if (!this->inRange(stations->size(), index)) {
            host_connection::console().print(F("Index not in range: "));
            host_connection::console().println(index);
            continue;
          }
          if (station.selected) {
            // Unselect "selected" ap
            Station new_sta = station;
            new_sta.selected = false;
            stations->set(index, new_sta);
            count_unselected += 1;
          }
          else {
            // Select "unselected" ap
            Station new_sta = station;
            new_sta.selected = true;
            stations->set(index, new_sta);
            count_selected += 1;
          }
        }
        this->showCounts(count_selected, count_unselected);
      }
    }
    // select ssids
    else if (ss_sw != -1) {
      // Get list of indices
      LinkedList<String> ss_index = this->parseCommand(cmd_args.get(ss_sw + 1), ",");

      // Select ALL SSIDs
      if (cmd_args.get(ss_sw + 1) == "all") {
        for (int i = 0; i < ssids->size(); i++) {
          if (ssids->get(i).selected) {
            ssid new_ssid = ssids->get(i);
            new_ssid.selected = false;
            ssids->set(i, new_ssid);
            count_unselected += 1;
          }
          else {
            ssid new_ssid = ssids->get(i);
            new_ssid.selected = true;
            ssids->set(i, new_ssid);
            count_selected += 1;
          }
        }
      }
      else {
      // Mark SSIDs as selected
        for (int i = 0; i < ss_index.size(); i++) {
          int index = ss_index.get(i).toInt();
          if (!this->inRange(ssids->size(), index)) {
            host_connection::console().print(F("Index not in range: "));
            host_connection::console().println(index);
            continue;
          }
          if (ssids->get(index).selected) {
            ssid new_ssid = ssids->get(index);
            new_ssid.selected = false;
            ssids->set(index, new_ssid);
            count_unselected += 1;
          }
          else {
            ssid new_ssid = ssids->get(index);
            new_ssid.selected = true;
            ssids->set(index, new_ssid);
            count_selected += 1;
          }
        }
      }
      this->showCounts(count_selected, count_unselected);
    }
  }
  else if (cmd_args.get(0) == SAVE_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int st_sw = this->argSearch(&cmd_args, "-s");

    if (ap_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.saveAPsMenu);
      #endif
      wifi_scan_obj.RunSaveAPList(true);
    }
    else if (st_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.saveSSIDsMenu);
      #endif
      wifi_scan_obj.RunSaveSSIDList(true);
    }
  }
  else if (cmd_args.get(0) == LOAD_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int st_sw = this->argSearch(&cmd_args, "-s");

    if (ap_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.loadAPsMenu);
      #endif
      wifi_scan_obj.RunLoadAPList();
    }
    else if (st_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.loadSSIDsMenu);
      #endif
      wifi_scan_obj.RunLoadSSIDList();
    }
  }

  // Add AP or station manually
  else if (cmd_args.get(0) == ADD_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int sta_sw = this->argSearch(&cmd_args, "-c");
    int bssid_sw = this->argSearch(&cmd_args, "-b");

    if (ap_sw != -1) {
      // add -a -b <mac> [-ch <channel>] [-e <ssid>]
      if (bssid_sw == -1 || !this->checkValueExists(&cmd_args, bssid_sw)) {
        host_connection::console().println(F("BSSID required: add -a -b <mac>"));
        return;
      }

      String mac_str = cmd_args.get(bssid_sw + 1);
      uint8_t mac[6];
      if (sscanf(mac_str.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
             &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        host_connection::console().println(F("Format must be XX:XX:XX:XX:XX:XX"));
        return;
      }

      // Duplicate check
      for (int i = 0; i < access_points->size(); i++) {
        AccessPoint access_point = access_points->get(i);
        bool match = true;
        for (int x = 0; x < 6; x++) {
          if (mac[x] != access_point.bssid[x]) {
            match = false;
            break;
          }
        }
        if (match) {
          host_connection::console().print(F("AP already exists at ["));
          host_connection::console().print(i);
          host_connection::console().print(F("]: "));
          host_connection::console().println(access_point.essid);
          return;
        }
      }

      int ch_sw = this->argSearch(&cmd_args, "-ch");
      uint8_t channel = 1;
      if (ch_sw != -1 && this->checkValueExists(&cmd_args, ch_sw))
        channel = cmd_args.get(ch_sw + 1).toInt();

      int essid_sw = this->argSearch(&cmd_args, "-e");
      String essid = mac_str;
      if (essid_sw != -1 && this->checkValueExists(&cmd_args, essid_sw))
        essid = cmd_args.get(essid_sw + 1);

      if (!marauder::RuntimeMemoryGuard::instance().allow(
              sizeof(AccessPoint) + sizeof(LinkedList<uint16_t>) + 128)) {
        host_connection::console().println(F("Memory pressure: AP not added"));
        return;
      }
      AccessPoint ap;
      ap.essid = essid;
      ap.channel = channel;
      memcpy(ap.bssid, mac, 6);
      ap.selected = true;
      ap.stations = new LinkedList<uint16_t>();
      ap.beacon[0] = 0;
      ap.beacon[1] = 0;
      ap.rssi = -127;
      ap.packets = 0;
      ap.sec = 0;
      ap.wps = false;
      ap.man = "";
      ap.has_msg_1 = false;
      ap.has_msg_2 = false;
      ap.has_msg_3 = false;
      ap.has_msg_4 = false;

      if (!marauder::memoryGuardedAdd(access_points, ap)) {
        delete ap.stations;
        host_connection::console().println(F("Memory pressure: AP not added"));
        return;
      }

      host_connection::console().print(F("Added AP ["));
      host_connection::console().print(access_points->size() - 1);
      host_connection::console().print(F("][CH:"));
      host_connection::console().print(channel);
      host_connection::console().print(F("] "));
      host_connection::console().print(essid);
      host_connection::console().println(F(" (selected)"));
    }
    else if (sta_sw != -1) {
      // add -c -b <mac> -ap <ap_index>
      if (bssid_sw == -1 || !this->checkValueExists(&cmd_args, bssid_sw)) {
        host_connection::console().println(F("MAC required: add -c -b <mac> -ap <index>"));
        return;
      }

      int ap_idx_sw = this->argSearch(&cmd_args, "-ap");
      if (ap_idx_sw == -1 || !this->checkValueExists(&cmd_args, ap_idx_sw)) {
        host_connection::console().println(F("AP index required: add -c -b <mac> -ap <index>"));
        return;
      }

      int ap_index = cmd_args.get(ap_idx_sw + 1).toInt();
      if (!this->inRange(access_points->size(), ap_index)) {
        host_connection::console().print(F("AP index not in range: "));
        host_connection::console().println(ap_index);
        return;
      }

      String mac_str = cmd_args.get(bssid_sw + 1);
      uint8_t mac[6];
      if (sscanf(mac_str.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
             &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        host_connection::console().println(F("Invalid MAC address format: use XX:XX:XX:XX:XX:XX"));
        return;
      }

      // Duplicate check
      for (int i = 0; i < stations->size(); i++) {
        bool match = true;
        for (int x = 0; x < 6; x++) {
          if (mac[x] != stations->get(i).mac[x]) {
            match = false;
            break;
          }
        }
        if (match) {
          host_connection::console().print(F("Station already exists at ["));
          host_connection::console().print(i);
          host_connection::console().println(F("]"));
          return;
        }
      }

      Station sta;
      memcpy(sta.mac, mac, 6);
      sta.selected = true;
      sta.packets = 0;
      sta.ap = ap_index;

      if (!marauder::memoryGuardedAdd(stations, sta)) {
        host_connection::console().println(F("Memory pressure: station not added"));
        return;
      }

      // Link station to AP
      AccessPoint ap = access_points->get(ap_index);
      if (!marauder::memoryGuardedAdd(ap.stations, stations->size() - 1)) {
        stations->pop();
        host_connection::console().println(F("Memory pressure: station not added"));
        return;
      }
      access_points->set(ap_index, ap);

      host_connection::console().print(F("Added station ["));
      host_connection::console().print(stations->size() - 1);
      host_connection::console().print(F("] -> AP ["));
      host_connection::console().print(ap_index);
      host_connection::console().print(F("] "));
      host_connection::console().print(ap.essid);
      host_connection::console().println(F(" (selected)"));
    }
    else {
      host_connection::console().println(F("Usage: add -a -b <mac> or add -c -b <mac> -ap <index>"));
    }
  }

  // SSID stuff
  else if (cmd_args.get(0) == SSID_CMD) {
    int add_sw = this->argSearch(&cmd_args, "-a");
    int gen_sw = this->argSearch(&cmd_args, "-g");
    int spc_sw = this->argSearch(&cmd_args, "-n");
    int rem_sw = this->argSearch(&cmd_args, "-r");

    // Add ssid
    if (add_sw != -1) {
      // Generate random
      if (gen_sw != -1) {
        int gen_count = cmd_args.get(gen_sw + 1).toInt();
        wifi_scan_obj.generateSSIDs(gen_count);
      }
      // Add specific
      else if (spc_sw != -1) {
        String essid = cmd_args.get(spc_sw + 1);
        wifi_scan_obj.addSSID(essid);
      }
      else {
        host_connection::console().println(F("You did not specify how to add SSIDs"));
      }
    }
    // Remove SSID
    else if (rem_sw != -1) {
      int index = cmd_args.get(rem_sw + 1).toInt();
      if (!this->inRange(ssids->size(), index)) {
        host_connection::console().print(F("Index not in range: "));
        host_connection::console().println(index);
        return;
      }
      ssids->remove(index);
      host_connection::console().println("Removed SSID ["+String(index)+"]");
    }
    else {
      host_connection::console().println(F("You did not specify whether to add or remove SSIDs"));
      return;
    }
  }
}
