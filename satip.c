/*
 * satip.c: A plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include "satip.h"
#include <ctype.h>
#include <getopt.h>
#include "common.h"
#include "config.h"
#include "device.h"
#include "discover.h"
#include "log.h"
#include "poller.h"
#include "setup.h"

#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM < 0x072400
#warning "CURL version >= 7.36.0 is recommended"
#endif

#if defined(APIVERSNUM) && APIVERSNUM < 20400
#error "VDR-2.4.0 API version or greater is required!"
#endif

#ifndef GITVERSION
#define GITVERSION ""
#endif

       const char VERSION[]     = "2.4.1" GITVERSION;
static const char DESCRIPTION[] = trNOOP("SAT>IP Devices");


/*******************************************************************************
 * class cPluginSatip
 ******************************************************************************/
cPluginSatip::cPluginSatip(void) : deviceCountM(2), serversM(NULL)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  // Initialize any member variables here.
  // DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
  // VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
}

cPluginSatip::~cPluginSatip()
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  // Clean up after yourself!
}


const char *cPluginSatip::Version(void)
{
  return VERSION;
}

const char *cPluginSatip::Description(void)
{
  return tr(DESCRIPTION);
}

const char *cPluginSatip::MainMenuEntry(void)
{
  return NULL;
}

const char *cPluginSatip::CommandLineHelp(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  // Return a string that describes all known command line options.
  return "  -d <num>, --devices=<number>  set number of devices to be created\n"
         "  -t <mode>, --trace=<mode>     set the debug mode\n"
         "  -s <ipaddr>|<model>|<desc>, --server=[<srcaddress>@]<ipaddress>[:<port>]|<model>[:<filter>]|<description>[:<quirk>];...\n"
         "                                define hard-coded SAT>IP server(s)\n\n"
         "                                srcaddress (Optional)  Source address can be used to define used\n"
         "                                                       networking interface on a host, e.g. 127.0.0.1.\n"
         "                                ipaddress              IP address of SAT>IP server, e.g. 127.0.0.1.\n"
         "                                port (Optional)        IP port number of SAT>IP server, e.g 443.\n"
         "                                model                  Model defines DVB modulation system (DVBS2,\n"
         "                                                       DVBT2, DVBT, DVBC) and number of available\n"
         "                                                       frontends separated by a hyphen, e.g. DVBT2-4.\n"
         "                                filter (Optional)      Filter can be used to limit satellite frontends\n"
         "                                                       to certain satellite position, e.g. S19.2E.\n"
         "                                description            Friendly name of SAT>IP server. This is used\n"
         "                                                       for autodetection of quirks.\n"
         "                                quirk (Optional)       Quirks are non-standard compliant features and\n"
         "                                                       bug fixes of SAT>IP server  defined by a\n"
         "                                                       hexadecimal number. Multiple quirks can be\n"
         "                                                       defined by combining values by addition:\n\n"
         "                                                       0x01: Fix session id bug\n"
         "                                                       0x02: Fix play parameter (addpids/delpids) bug\n"
         "                                                       0x04: Fix frontend locking bug\n"
         "                                                       0x08: Support for RTP over TCP\n"
         "                                                       0x10: Support the X_PMT protocol extension\n"
         "                                                       0x20: Support the CI TNR protocol extension\n"
         "                                                       0x40: Fix auto-detection of pilot tones bug\n"
         "                                                       0x80: Fix re-tuning bug by teardowning a session\n"
         "  -D, --detach                  set the detached mode on\n"
         "  -S, --single                  set the single model server mode on\n"
         "  -n, --noquirks                disable autodetection of the server quirks\n"
         "  -p, --portrange=<start>-<end> set a range of ports used for the RT[C]P server\n"
         "                                a minimum of 2 ports per device is required.\n"
         "  -r, --rcvbuf                  override the size of the RTP receive buffer in bytes\n";
}

bool cPluginSatip::ProcessArgs(int argc, char *argv[])
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  // Implement command line argument processing here if applicable.
  static const struct option long_options[] = {
    { "devices",  required_argument, NULL, 'd' },
    { "trace",    required_argument, NULL, 't' },
    { "server",   required_argument, NULL, 's' },
    { "portrange",required_argument, NULL, 'p' },
    { "rcvbuf",   required_argument, NULL, 'r' },
    { "detach",   no_argument,       NULL, 'D' },
    { "single",   no_argument,       NULL, 'S' },
    { "noquirks", no_argument,       NULL, 'n' },
    { NULL,       no_argument,       NULL,  0  }
    };

  cString server;
  cString portrange;
  int c;
  while ((c = getopt_long(argc, argv, "d:t:s:p:r:DSn", long_options, NULL)) != -1) {
    switch (c) {
      case 'd':
           deviceCountM = strtol(optarg, NULL, 0);
           break;
      case 't':
           SatipConfig.SetDebugMode(strtol(optarg, NULL, 0));
           break;
      case 's':
           server = optarg;
           break;
      case 'D':
           SatipConfig.SetDetachedMode(true);
           break;
      case 'S':
           SatipConfig.SetUseSingleModelServers(true);
           break;
      case 'n':
           SatipConfig.SetDisableServerQuirks(true);
           break;
      case 'p':
           portrange = optarg;
           break;
      case 'r':
           SatipConfig.SetRtpRcvBufSize(strtol(optarg, NULL, 0));
           break;
      default:
           return false;
      }
    }
  if (!isempty(*portrange))
     ParsePortRange(portrange);
  // this must be done after all parameters are parsed
  if (!isempty(*server))
     ParseServer(*server);
  return true;
}

bool cPluginSatip::Initialize(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  // Initialize any background activities the plugin shall perform.
  if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
     error("Unable to initialize CURL");
  cSatipPoller::GetInstance()->Initialize();
  cSatipDiscover::GetInstance()->Initialize(serversM);
  return cSatipDevice::Initialize(deviceCountM);
}

bool cPluginSatip::Start(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  // Start any background activities the plugin shall perform.
  curl_version_info_data *data = curl_version_info(CURLVERSION_NOW);
  cString info = cString::sprintf("Using CURL %s", data->version);
  for (int i = 0; data->protocols[i]; ++i) {
      // Supported protocols: HTTP(S), RTSP, FILE
      if (startswith(data->protocols[i], "rtsp"))
         info = cString::sprintf("%s %s", *info, data->protocols[i]);
      }
  dbg_rtsp("%s", *info);
  return true;
}

void cPluginSatip::Stop(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  // Stop any background activities the plugin is performing.
  cSatipDevice::Shutdown();
  cSatipDiscover::GetInstance()->Destroy();
  cSatipPoller::GetInstance()->Destroy();
  curl_global_cleanup();
}

void cPluginSatip::Housekeeping(void)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  // Perform any cleanup or other regular tasks.
}

void cPluginSatip::MainThreadHook(void)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  // Perform actions in the context of the main program thread.
  // WARNING: Use with great care - see PLUGINS.html!
}

cString cPluginSatip::Active(void)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  // Return a message string if shutdown should be postponed
  return NULL;
}

time_t cPluginSatip::WakeupTime(void)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  // Return custom wakeup time for shutdown script
  return 0;
}

cOsdObject *cPluginSatip::MainMenuAction(void)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  // Perform the action when selected from the main VDR menu.
  return NULL;
}

cMenuSetupPage *cPluginSatip::SetupMenu(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  // Return a setup menu in case the plugin supports one.
  return new cSatipPluginSetup();
}

void cPluginSatip::ParseServer(const char *paramP)
{
  dbg_funcname("%s (%s)", __PRETTY_FUNCTION__, paramP);
  int n = 0;
  char *s, *p = strdup(paramP);
  char *r = strtok_r(p, ";", &s);
  while (r) {
        r = skipspace(r);
        dbg_parsing("%s server[%d]=%s", __PRETTY_FUNCTION__, n, r);
        cString sourceAddr, serverAddr, serverModel, serverFilters, serverDescription;
        int serverQuirk = cSatipServer::eSatipQuirkNone;
        int serverPort = SATIP_DEFAULT_RTSP_PORT;
        int n2 = 0;
        char *s2, *p2 = r;
        char *r2 = strtok_r(p2, "|", &s2);
        while (r2) {
              dbg_parsing("%s param[%d]=%s", __PRETTY_FUNCTION__, n2, r2);
              switch (n2++) {
                     case 0:
                          {
                          char *r3 = strchr(r2, '@');
                          if (r3) {
                             *r3 = 0;
                             sourceAddr = r2;
                             r2 = r3 + 1;
                             }
                          serverAddr = r2;
                          r3 = strchr(r2, ':');
                          if (r3) {
                             serverPort = strtol(r3 + 1, NULL, 0);
                             serverAddr = serverAddr.Truncate(r3 - r2);
                             }
                          }
                          break;
                     case 1:
                          {
                          serverModel = r2;
                          char *r3 = strchr(r2, ':');
                          if (r3) {
                             serverFilters = r3 + 1;
                             serverModel = serverModel.Truncate(r3 - r2);
                             }
                          }
                          break;
                     case 2:
                          {
                          serverDescription = r2;
                          char *r3 = strchr(r2, ':');
                          if (r3) {
                             serverQuirk = strtol(r3 + 1, NULL, 0);
                             serverDescription = serverDescription.Truncate(r3 - r2);
                             }
                          }
                          break;
                     default:
                          break;
                     }
              r2 = strtok_r(NULL, "|", &s2);
              }
        if (*serverAddr && *serverModel && *serverDescription) {
           dbg_funcname("%s srcaddr=%s ipaddr=%s port=%d model=%s (%s) desc=%s (%d)", __PRETTY_FUNCTION__, *sourceAddr, *serverAddr, serverPort, *serverModel, *serverFilters, *serverDescription, serverQuirk);
           if (!serversM)
              serversM = new cSatipDiscoverServers();
           serversM->Add(new cSatipDiscoverServer(*sourceAddr, *serverAddr, serverPort, *serverModel, *serverFilters, *serverDescription, serverQuirk));
           }
        ++n;
        r = strtok_r(NULL, ";", &s);
        }
  FREE_POINTER(p);
}

void cPluginSatip::ParsePortRange(const char *paramP)
{
  char *s, *p = skipspace(paramP);
  char *r = strtok_r(p, "-", &s);
  unsigned int rangeStart = 0;
  unsigned int rangeStop = 0;
  if (r) {
     rangeStart = strtol(r, NULL, 0);
     r = strtok_r(NULL, "-", &s);
     }
  if (r)
     rangeStop = strtol(r, NULL, 0);
  else {
     error("Port range argument not valid '%s'", paramP);
     rangeStart = 0;
     rangeStop = 0;
     }
  if (rangeStart % 2) {
     error("The given range start port must be even!");
     rangeStart = 0;
     rangeStop = 0;
     }
  else if (rangeStop - rangeStart + 1 < deviceCountM * 2) {
     error("The given port range is to small: %d < %d!", rangeStop - rangeStart + 1, deviceCountM * 2);
     rangeStart = 0;
     rangeStop = 0;
     }
  SatipConfig.SetPortRangeStart(rangeStart);
  SatipConfig.SetPortRangeStop(rangeStop);
}

int cPluginSatip::ParseCicams(const char *valueP, int *cicamsP)
{
  dbg_funcname("%s (%s,)", __PRETTY_FUNCTION__, valueP);
  int n = 0;
  char *s, *p = strdup(valueP);
  char *r = strtok_r(p, " ", &s);
  while (r) {
        r = skipspace(r);
        dbg_parsing("%s cicams[%d]=%s", __PRETTY_FUNCTION__, n, r);
        if (n < MAX_CICAM_COUNT) {
           cicamsP[n++] = atoi(r);
           }
        r = strtok_r(NULL, " ", &s);
        }
  FREE_POINTER(p);
  return n;
}

int cPluginSatip::ParseSources(const char *valueP, int *sourcesP)
{
  dbg_funcname("%s (%s,)", __PRETTY_FUNCTION__, valueP);
  int n = 0;
  char *s, *p = strdup(valueP);
  char *r = strtok_r(p, " ", &s);
  while (r) {
        r = skipspace(r);
        dbg_parsing("%s sources[%d]=%s", __PRETTY_FUNCTION__, n, r);
        if (n < MAX_DISABLED_SOURCES_COUNT) {
           sourcesP[n++] = cSource::FromString(r);
           }
        r = strtok_r(NULL, " ", &s);
        }
  FREE_POINTER(p);
  return n;
}

int cPluginSatip::ParseFilters(const char *valueP, int *filtersP)
{
  dbg_funcname("%s (%s,)", __PRETTY_FUNCTION__, valueP);
  char buffer[256];
  int n = 0;
  while (valueP && *valueP && (n < SECTION_FILTER_TABLE_SIZE)) {
    strn0cpy(buffer, valueP, sizeof(buffer));
    int i = atoi(buffer);
    dbg_parsing("%s filters[%d]=%d", __PRETTY_FUNCTION__, n, i);
    if (i >= 0)
       filtersP[n++] = i;
    if ((valueP = strchr(valueP, ' ')) != NULL)
       valueP++;
    }
  return n;
}

bool cPluginSatip::SetupParse(const char *nameP, const char *valueP)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  // Parse your own setup parameters and store their values.
  if (!strcasecmp(nameP, "OperatingMode"))
     SatipConfig.SetOperatingMode(atoi(valueP));
  else if (!strcasecmp(nameP, "EnableCIExtension"))
     SatipConfig.SetCIExtension(atoi(valueP));
  else if (!strcasecmp(nameP, "EnableFrontendReuse"))
     SatipConfig.SetFrontendReuse(atoi(valueP));
  else if (!strcasecmp(nameP, "CICAM")) {
     int Cicams[MAX_CICAM_COUNT];
     for (unsigned int i = 0; i < ELEMENTS(Cicams); ++i)
         Cicams[i] = 0;
     unsigned int CicamsCount = ParseCicams(valueP, Cicams);
     for (unsigned int i = 0; i < CicamsCount; ++i)
         SatipConfig.SetCICAM(i, Cicams[i]);
     }
  else if (!strcasecmp(nameP, "EnableEITScan"))
     SatipConfig.SetEITScan(atoi(valueP));
  else if (!strcasecmp(nameP, "DisabledSources")) {
     int DisabledSources[MAX_DISABLED_SOURCES_COUNT];
     for (unsigned int i = 0; i < ELEMENTS(DisabledSources); ++i)
         DisabledSources[i] = cSource::stNone;
     unsigned int DisabledSourcesCount = ParseSources(valueP, DisabledSources);
     for (unsigned int i = 0; i < DisabledSourcesCount; ++i)
         SatipConfig.SetDisabledSources(i, DisabledSources[i]);
     }
  else if (!strcasecmp(nameP, "DisabledFilters")) {
     int DisabledFilters[SECTION_FILTER_TABLE_SIZE];
     for (unsigned int i = 0; i < ELEMENTS(DisabledFilters); ++i)
         DisabledFilters[i] = -1;
     unsigned int DisabledFiltersCount = ParseFilters(valueP, DisabledFilters);
     for (unsigned int i = 0; i < DisabledFiltersCount; ++i)
         SatipConfig.SetDisabledFilters(i, DisabledFilters[i]);
     }
  else if (!strcasecmp(nameP, "TransportMode"))
     SatipConfig.SetTransportMode(atoi(valueP));
  else
     return false;
  return true;
}

bool cPluginSatip::Service(const char *idP, void *dataP)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  return false;
}

const char **cPluginSatip::SVDRPHelpPages(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  static const char *HelpPages[] = {
    "INFO [ <page> ] [ <card index> ]\n"
    "    Prints SAT>IP device information and statistics.\n"
    "    The output can be narrowed using optional \"page\""
    "    option: 1=general 2=pids 3=section filters.\n",
    "MODE\n"
    "    Toggles between bit or byte information mode.\n",
    "LIST\n"
    "    Lists active SAT>IP servers.\n",
    "SCAN\n"
    "    Scans active SAT>IP servers.\n",
    "STAT\n"
    "    Lists status information of SAT>IP devices.\n",
    "CONT\n"
    "    Shows SAT>IP device count.\n",
    "OPER [ off | low | normal | high ]\n"
    "    Gets and(or sets operating mode of SAT>IP devices.\n",
    "ATTA\n"
    "    Attach active SAT>IP servers.\n",
    "DETA\n"
    "    Detachs active SAT>IP servers.\n",
    "TRAC [ <mode> ]\n"
    "    Gets and/or sets used debug mode.\n",
    NULL
    };
  return HelpPages;
}

cString cPluginSatip::SVDRPCommand(const char *commandP, const char *optionP, int &replyCodeP)
{
  dbg_funcname("%s (%s, %s,)", __PRETTY_FUNCTION__, commandP, optionP);
  if (strcasecmp(commandP, "INFO") == 0) {
     int index = cDevice::ActualDevice()->CardIndex();
     int page = SATIP_DEVICE_INFO_ALL;
     char *opt = strdup(optionP);
     char *num = skipspace(opt);
     char *option = num;
     while (*option && !isspace(*option))
           ++option;
     if (*option) {
        *option = 0;
        option = skipspace(++option);
        if (isnumber(option))
           index = atoi(option);
        }
     if (isnumber(num)) {
        page = atoi(num);
        if ((page < SATIP_DEVICE_INFO_ALL) || (page > SATIP_DEVICE_INFO_FILTERS))
           page = SATIP_DEVICE_INFO_ALL;
        }
     free(opt);
     cSatipDevice *device = cSatipDevice::GetSatipDevice(index);
     if (device) {
        return device->GetInformation(page);
        }
     else {
        replyCodeP = 550; // Requested action not taken
        return cString("SATIP information not available!");
        }
     }
  else if (strcasecmp(commandP, "MODE") == 0) {
     unsigned int mode = !SatipConfig.GetUseBytes();
     SatipConfig.SetUseBytes(mode);
     return cString::sprintf("SATIP information mode: %s\n", mode ? "bytes" : "bits");
     }
  else if (strcasecmp(commandP, "LIST") == 0) {
     cString list = cSatipDiscover::GetInstance()->GetServerList();
     if (!isempty(list)) {
        return list;
        }
     else {
        replyCodeP = 550; // Requested action not taken
        return cString("No SATIP servers detected!");
        }
     }
  else if (strcasecmp(commandP, "SCAN") == 0) {
     cSatipDiscover::GetInstance()->TriggerScan();
     return cString("SATIP server scan requested");
     }
  else if (strcasecmp(commandP, "STAT") == 0) {
     return cSatipDevice::GetSatipStatus();
     }
  else if (strcasecmp(commandP, "CONT") == 0) {
     return cString::sprintf("SATIP device count: %u", cSatipDevice::Count());
     }
  else if (strcasecmp(commandP, "OPER") == 0) {
     cString mode;
     unsigned int oper = SatipConfig.GetOperatingMode();
     if (optionP && *optionP) {
        if (strcasecmp(optionP, "off") == 0)
           oper = cSatipConfig::eOperatingModeOff;
        else if (strcasecmp(optionP, "low") == 0)
           oper = cSatipConfig::eOperatingModeLow;
        else if (strcasecmp(optionP, "normal") == 0)
           oper = cSatipConfig::eOperatingModeNormal;
        else if (strcasecmp(optionP, "high") == 0)
           oper = cSatipConfig::eOperatingModeHigh;
        SatipConfig.SetOperatingMode(oper);
     }
     switch (oper) {
       case cSatipConfig::eOperatingModeOff:
            mode = "off";
            break;
       case cSatipConfig::eOperatingModeLow:
            mode = "low";
            break;
       case cSatipConfig::eOperatingModeNormal:
            mode = "normal";
            break;
       case cSatipConfig::eOperatingModeHigh:
            mode = "high";
            break;
       default:
            mode = "unknown";
            break;
       }
     return cString::sprintf("SATIP operating mode: %s\n", *mode);
     }
  else if (strcasecmp(commandP, "ATTA") == 0) {
     SatipConfig.SetDetachedMode(false);
     info("SATIP servers attached");
     return cString("SATIP servers attached");
     }
  else if (strcasecmp(commandP, "DETA") == 0) {
     SatipConfig.SetDetachedMode(true);
     info("SATIP servers detached");
     return cString("SATIP servers detached");
     }
  else if (strcasecmp(commandP, "TRAC") == 0) {
     if (optionP && *optionP)
        SatipConfig.SetDebugMode(strtol(optionP, NULL, 0));
     return cString::sprintf("SATIP debug mode: 0x%04X\n", SatipConfig.GetDebugMode());
     }

  return NULL;
}

VDRPLUGINCREATOR(cPluginSatip); // Don't touch this!
