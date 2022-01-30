/*
 * discover.c: SAT>IP plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include <string.h>
#ifdef USE_TINYXML
 #include <tinyxml.h>
#else
 #include <pugixml.hpp>
#endif
#include "common.h"
#include "config.h"
#include "log.h"
#include "socket.h"
#include "discover.h"

cSatipDiscover *cSatipDiscover::instanceS = NULL;

cSatipDiscover *cSatipDiscover::GetInstance(void)
{
  if (!instanceS)
     instanceS = new cSatipDiscover();
  return instanceS;
}

bool cSatipDiscover::Initialize(cSatipDiscoverServers *serversP)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  if (instanceS) {
     if (serversP) {
        for (cSatipDiscoverServer *s = serversP->First(); s; s = serversP->Next(s))
            instanceS->AddServer(s->SrcAddress(), s->IpAddress(), s->IpPort(), s->Model(), s->Filters(), s->Description(), s->Quirk());
        }
     else
        instanceS->Activate();
     }
  return true;
}

void cSatipDiscover::Destroy(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  if (instanceS)
     instanceS->Deactivate();
}

size_t cSatipDiscover::HeaderCallback(char *ptrP, size_t sizeP, size_t nmembP, void *dataP)
{
  cSatipDiscover *obj = reinterpret_cast<cSatipDiscover *>(dataP);
  size_t len = sizeP * nmembP;
  dbg_funcname_ext("%s len=%zu", __PRETTY_FUNCTION__, len);

  if (obj && (len > 0))
     obj->headerBufferM.Add(ptrP, len);

  return len;
}

size_t cSatipDiscover::DataCallback(char *ptrP, size_t sizeP, size_t nmembP, void *dataP)
{
  cSatipDiscover *obj = reinterpret_cast<cSatipDiscover *>(dataP);
  size_t len = sizeP * nmembP;
  dbg_funcname_ext("%s len=%zu", __PRETTY_FUNCTION__, len);

  if (obj && (len > 0))
     obj->dataBufferM.Add(ptrP, len);

  return len;
}

int cSatipDiscover::DebugCallback(CURL *handleP, curl_infotype typeP, char *dataP, size_t sizeP, void *userPtrP)
{
  cSatipDiscover *obj = reinterpret_cast<cSatipDiscover *>(userPtrP);

  if (obj) {
     switch (typeP) {
       case CURLINFO_TEXT:
            dbg_curlinfo("%s HTTP INFO %.*s", __PRETTY_FUNCTION__, (int)sizeP, dataP);
            break;
       case CURLINFO_HEADER_IN:
            dbg_curlinfo("%s HTTP HEAD <<< %.*s", __PRETTY_FUNCTION__, (int)sizeP, dataP);
            break;
       case CURLINFO_HEADER_OUT:
            dbg_curlinfo("%s HTTP HEAD >>>\n%.*s", __PRETTY_FUNCTION__, (int)sizeP, dataP);
            break;
       case CURLINFO_DATA_IN:
            dbg_curlinfo("%s HTTP DATA <<< %.*s", __PRETTY_FUNCTION__, (int)sizeP, dataP);
            break;
       case CURLINFO_DATA_OUT:
            dbg_curlinfo("%s HTTP DATA >>>\n%.*s", __PRETTY_FUNCTION__, (int)sizeP, dataP);
            break;
       default:
            break;
       }
     }

  return 0;
}

cSatipDiscover::cSatipDiscover()
: cThread("SATIP discover"),
  mutexM(),
  headerBufferM(),
  dataBufferM(),
  msearchM(*this),
  probeUrlListM(),
  handleM(curl_easy_init()),
  sleepM(),
  probeIntervalM(0),
  serversM()
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
}

cSatipDiscover::~cSatipDiscover()
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  Deactivate();
  cMutexLock MutexLock(&mutexM);
  // Free allocated memory
  if (handleM)
     curl_easy_cleanup(handleM);
  handleM = NULL;
  probeUrlListM.Clear();
}

void cSatipDiscover::Activate(void)
{
  // Start the thread
  Start();
}

void cSatipDiscover::Deactivate(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  sleepM.Signal();
  if (Running())
     Cancel(3);
}

void cSatipDiscover::Action(void)
{
  dbg_funcname("%s Entering", __PRETTY_FUNCTION__);
  probeIntervalM.Set(eProbeIntervalMs);
  msearchM.Probe();
  // Do the thread loop
  while (Running()) {
        cStringList tmp;

        if (probeIntervalM.TimedOut()) {
           probeIntervalM.Set(eProbeIntervalMs);
           msearchM.Probe();
           mutexM.Lock();
           serversM.Cleanup(eCleanupTimeoutMs);
           mutexM.Unlock();
           }
        mutexM.Lock();
        if (probeUrlListM.Size()) {
           for (int i = 0; i < probeUrlListM.Size(); ++i)
               tmp.Insert(strdup(probeUrlListM.At(i)));
           probeUrlListM.Clear();
           }
        mutexM.Unlock();
        if (tmp.Size()) {
           for (int i = 0; i < tmp.Size(); ++i)
               Fetch(tmp.At(i));
           tmp.Clear();
           }
        // to avoid busy loop and reduce cpu load
        sleepM.Wait(eSleepTimeoutMs);
        }
  dbg_funcname("%s Exiting", __PRETTY_FUNCTION__);
}

void cSatipDiscover::Fetch(const char *urlP)
{
  dbg_funcname("%s (%s)", __PRETTY_FUNCTION__, urlP);
  if (handleM && !isempty(urlP)) {
     const char *addr = NULL;
     long rc = 0;
     CURLcode res = CURLE_OK;

     // Verbose output
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_VERBOSE, 1L);
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_DEBUGFUNCTION, cSatipDiscover::DebugCallback);
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_DEBUGDATA, this);

     // Set header and data callbacks
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_HEADERFUNCTION, cSatipDiscover::HeaderCallback);
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_WRITEHEADER, this);
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_WRITEFUNCTION, cSatipDiscover::DataCallback);
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_WRITEDATA, this);

     // No progress meter and no signaling
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_NOPROGRESS, 1L);
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_NOSIGNAL, 1L);

     // Set timeouts
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_TIMEOUT_MS, (long)eConnectTimeoutMs);
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_CONNECTTIMEOUT_MS, (long)eConnectTimeoutMs);

     // Set user-agent
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_USERAGENT, *cString::sprintf("vdr-%s/%s", PLUGIN_NAME_I18N, VERSION));

     // Set URL
     SATIP_CURL_EASY_SETOPT(handleM, CURLOPT_URL, urlP);

     // Fetch the data
     SATIP_CURL_EASY_PERFORM(handleM);
     SATIP_CURL_EASY_GETINFO(handleM, CURLINFO_RESPONSE_CODE, &rc);
     SATIP_CURL_EASY_GETINFO(handleM, CURLINFO_PRIMARY_IP, &addr);
     if (rc == 200) {
        ParseDeviceInfo(addr, ParseRtspPort());
        headerBufferM.Reset();
        dataBufferM.Reset();
        }
     else
        error("Discovery detected invalid status code: %ld", rc);
     }
}

int cSatipDiscover::ParseRtspPort(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  char *s, *p = headerBufferM.Data();
  char *r = strtok_r(p, "\r\n", &s);
  int port = SATIP_DEFAULT_RTSP_PORT;

  while (r) {
        dbg_funcname_ext("%s (%zu): %s", __PRETTY_FUNCTION__, headerBufferM.Size(), r);
        r = skipspace(r);
        if (strstr(r, "X-SATIP-RTSP-Port")) {
           int tmp = -1;
           if (sscanf(r, "X-SATIP-RTSP-Port:%11d", &tmp) == 1) {
              port = tmp;
              break;
              }
           }
        r = strtok_r(NULL, "\r\n", &s);
        }

  return port;
}

void cSatipDiscover::ParseDeviceInfo(const char *addrP, const int portP)
{
  dbg_funcname("%s (%s, %d)", __PRETTY_FUNCTION__, addrP, portP);
  const char *desc = NULL, *model = NULL;
#ifdef USE_TINYXML
  TiXmlDocument doc;
  doc.Parse(dataBufferM.Data());
  TiXmlHandle docHandle(&doc);
  TiXmlElement *descElement = docHandle.FirstChild("root").FirstChild("device").FirstChild("friendlyName").ToElement();
  if (descElement)
     desc = descElement->GetText() ? descElement->GetText() : "MyBrokenHardware";
  TiXmlElement *modelElement = docHandle.FirstChild("root").FirstChild("device").FirstChild("satip:X_SATIPCAP").ToElement();
  if (modelElement)
     model = modelElement->GetText() ? modelElement->GetText() : "DVBS2-1";
#else
  pugi::xml_document doc;
  if (doc.load_buffer(dataBufferM.Data(), dataBufferM.Size())) {
     pugi::xml_node descNode = doc.first_element_by_path("root/device/friendlyName");
     if (descNode)
        desc = descNode.text().as_string("MyBrokenHardware");
     pugi::xml_node modelNode = doc.first_element_by_path("root/device/satip:X_SATIPCAP");
     if (modelNode)
        model = modelNode.text().as_string("DVBS2-1");
     }
#endif
  AddServer(NULL, addrP, portP, model, NULL, desc, cSatipServer::eSatipQuirkNone);
}

void cSatipDiscover::AddServer(const char *srcAddrP, const char *addrP, const int portP, const char *modelP, const char *filtersP, const char *descP, const int quirkP)
{
  dbg_funcname("%s (%s, %s, %d, %s, %s, %s, %d)", __PRETTY_FUNCTION__, srcAddrP, addrP, portP, modelP, filtersP, descP, quirkP);
  cMutexLock MutexLock(&mutexM);
  if (SatipConfig.GetUseSingleModelServers() && modelP && !isempty(modelP)) {
     int n = 0;
     char *s, *p = strdup(modelP);
     char *r = strtok_r(p, ",", &s);
     while (r) {
           r = skipspace(r);
           cString desc = cString::sprintf("%s #%d", !isempty(descP) ? descP : "MyBrokenHardware", n++);
           cSatipServer *tmp = new cSatipServer(srcAddrP, addrP, portP, r, filtersP, desc, quirkP);
           if (!serversM.Update(tmp)) {
              info("Adding server '%s|%s|%s' Bind: %s Filters: %s CI: %s Quirks: %s", tmp->Address(), tmp->Model(), tmp->Description(), !isempty(tmp->SrcAddress()) ? tmp->SrcAddress() : "default", !isempty(tmp->Filters()) ? tmp->Filters() : "none", tmp->HasCI() ? "yes" : "no", tmp->HasQuirk() ? tmp->Quirks() : "none");
              serversM.Add(tmp);
              }
           else
              DELETENULL(tmp);
           r = strtok_r(NULL, ",", &s);
           }
     FREE_POINTER(p);
     }
  else {
     cSatipServer *tmp = new cSatipServer(srcAddrP, addrP, portP, modelP, filtersP, descP, quirkP);
     if (!serversM.Update(tmp)) {
        info("Adding server '%s|%s|%s' Bind: %s Filters: %s CI: %s Quirks: %s", tmp->Address(), tmp->Model(), tmp->Description(), !isempty(tmp->SrcAddress()) ? tmp->SrcAddress() : "default", !isempty(tmp->Filters()) ? tmp->Filters() : "none", tmp->HasCI() ? "yes" : "no", tmp->HasQuirk() ? tmp->Quirks() : "none");
        serversM.Add(tmp);
        }
     else
        DELETENULL(tmp);
     }
}

int cSatipDiscover::GetServerCount(void)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return serversM.Count();
}

cSatipServer *cSatipDiscover::AssignServer(int deviceIdP, int sourceP, int transponderP, int systemP)
{
  dbg_funcname_ext("%s (%d, %d, %d, %d)", __PRETTY_FUNCTION__, deviceIdP, sourceP, transponderP, systemP);
  cMutexLock MutexLock(&mutexM);
  return serversM.Assign(deviceIdP, sourceP, transponderP, systemP);
}

cSatipServer *cSatipDiscover::GetServer(int sourceP)
{
  dbg_funcname_ext("%s (%d)", __PRETTY_FUNCTION__, sourceP);
  cMutexLock MutexLock(&mutexM);
  return serversM.Find(sourceP);
}

cSatipServer *cSatipDiscover::GetServer(cSatipServer *serverP)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return serversM.Find(serverP);
}

cSatipServers *cSatipDiscover::GetServers(void)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return &serversM;
}

cString cSatipDiscover::GetServerString(cSatipServer *serverP)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return serversM.GetString(serverP);
}

cString cSatipDiscover::GetServerList(void)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return serversM.List();
}

void cSatipDiscover::ActivateServer(cSatipServer *serverP, bool onOffP)
{
  dbg_funcname_ext("%s (, %d)", __PRETTY_FUNCTION__, onOffP);
  cMutexLock MutexLock(&mutexM);
  serversM.Activate(serverP, onOffP);
}

void cSatipDiscover::AttachServer(cSatipServer *serverP, int deviceIdP, int transponderP)
{
  dbg_funcname_ext("%s (, %d, %d)", __PRETTY_FUNCTION__, deviceIdP, transponderP);
  cMutexLock MutexLock(&mutexM);
  serversM.Attach(serverP, deviceIdP, transponderP);
}

void cSatipDiscover::DetachServer(cSatipServer *serverP, int deviceIdP, int transponderP)
{
  dbg_funcname_ext("%s (, %d, %d)", __PRETTY_FUNCTION__, deviceIdP, transponderP);
  cMutexLock MutexLock(&mutexM);
  serversM.Detach(serverP, deviceIdP, transponderP);
}

bool cSatipDiscover::IsServerQuirk(cSatipServer *serverP, int quirkP)
{
  dbg_funcname_ext("%s (, %d)", __PRETTY_FUNCTION__, quirkP);
  cMutexLock MutexLock(&mutexM);
  return serversM.IsQuirk(serverP, quirkP);
}

bool cSatipDiscover::HasServerCI(cSatipServer *serverP)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return serversM.HasCI(serverP);
}

cString cSatipDiscover::GetSourceAddress(cSatipServer *serverP)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return serversM.GetSrcAddress(serverP);
}

cString cSatipDiscover::GetServerAddress(cSatipServer *serverP)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return serversM.GetAddress(serverP);
}

int cSatipDiscover::GetServerPort(cSatipServer *serverP)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return serversM.GetPort(serverP);
}

int cSatipDiscover::NumProvidedSystems(void)
{
  dbg_funcname_ext("%s", __PRETTY_FUNCTION__);
  cMutexLock MutexLock(&mutexM);
  return serversM.NumProvidedSystems();
}

void cSatipDiscover::SetUrl(const char *urlP)
{
  dbg_funcname_ext("%s (%s)", __PRETTY_FUNCTION__, urlP);
  mutexM.Lock();
  probeUrlListM.Insert(strdup(urlP));
  mutexM.Unlock();
  sleepM.Signal();
}
