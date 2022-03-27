/*
 * device.c: SAT>IP plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */
#include <string>
#include <vdr/menu.h> // cRecordControl

#include "config.h"
#include "discover.h"
#include "log.h"
#include "param.h"
#include "device.h"

static cSatipDevice * SatipDevicesS[SATIP_MAX_DEVICES] = { NULL };

cMutex cSatipDevice::SetChannelMtx = cMutex();

cSatipDevice::cSatipDevice(unsigned int indexP)
: deviceIndex(indexP),
  bytesDelivered(0),
  isOpenDvrM(false),
  checkTsBufferM(false),
  channelM(),
  createdM(0),
  tunedM()
{
  unsigned int bufsize = (unsigned int)SATIP_BUFFER_SIZE;
  bufsize -= (bufsize % TS_SIZE);
  info("Creating device CardIndex=%d DeviceNumber=%d [device %u]", CardIndex(), DeviceNumber(), deviceIndex);
  tsBufferM = new cRingBufferLinear(bufsize + 1, TS_SIZE, false,
                                   *cString::sprintf("SATIP#%d TS", deviceIndex));
  if (tsBufferM) {
     tsBufferM->SetTimeouts(10, 10);
     tsBufferM->SetIoThrottle();
     tuner = new cSatipTuner(*this, tsBufferM->Free());
     }
  // Start section handler
  pSectionFilterHandlerM = new cSatipSectionFilterHandler(deviceIndex, bufsize + 1);
  StartSectionHandler();
}

cSatipDevice::~cSatipDevice()
{
  dbg_funcname("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  // Release immediately any pending conditional wait
  tunedM.Broadcast();
  // Stop section handler
  StopSectionHandler();
  DELETE_POINTER(pSectionFilterHandlerM);
  DELETE_POINTER(tuner);
  DELETE_POINTER(tsBufferM);
}

bool cSatipDevice::Initialize(unsigned int deviceCountP)
{
  dbg_funcname("%s (%u)", __PRETTY_FUNCTION__, deviceCountP);
  if (deviceCountP > SATIP_MAX_DEVICES)
     deviceCountP = SATIP_MAX_DEVICES;
  for (unsigned int i = 0; i < deviceCountP; ++i)
      SatipDevicesS[i] = new cSatipDevice(i);
  for (unsigned int i = deviceCountP; i < SATIP_MAX_DEVICES; ++i)
      SatipDevicesS[i] = NULL;
  return true;
}

void cSatipDevice::Shutdown(void)
{
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  for (int i = 0; i < SATIP_MAX_DEVICES; ++i) {
      if (SatipDevicesS[i])
         SatipDevicesS[i]->CloseDvr();
      }
}

unsigned int cSatipDevice::Count(void)
{
  unsigned int count = 0;
  dbg_funcname("%s", __PRETTY_FUNCTION__);
  for (unsigned int i = 0; i < SATIP_MAX_DEVICES; ++i) {
      if (SatipDevicesS[i] != NULL)
         count++;
      }
  return count;
}

cSatipDevice *cSatipDevice::GetSatipDevice(int cardIndexP)
{
  dbg_funcname_ext("%s (%d)", __PRETTY_FUNCTION__, cardIndexP);
  for (unsigned int i = 0; i < SATIP_MAX_DEVICES; ++i) {
      if (SatipDevicesS[i] && (SatipDevicesS[i]->CardIndex() == cardIndexP)) {
         dbg_funcname_ext("%s (%d): Found!", __PRETTY_FUNCTION__, cardIndexP);
         return SatipDevicesS[i];
         }
      }
  return NULL;
}

cString cSatipDevice::GetSatipStatus(void)
{
  cString info = "";
  for (int i = 0; i < cDevice::NumDevices(); i++) {
      const cDevice *device = cDevice::GetDevice(i);
      if (device && strstr(device->DeviceType(), "SAT>IP")) {
         int timers = 0;
         bool live = (device == cDevice::ActualDevice());
         bool lock = device->HasLock();
         const cChannel *channel = device->GetCurrentlyTunedTransponder();
         LOCK_TIMERS_READ;
         for (const cTimer *timer = Timers->First(); timer; timer = Timers->Next(timer)) {
             if (timer->Recording()) {
                cRecordControl *control = cRecordControls::GetRecordControl(timer);
                if (control && control->Device() == device)
                   timers++;
                }
            }
         info = cString::sprintf("%sDevice: %s\n", *info, *device->DeviceName());
         if (lock)
            info = cString::sprintf("%sCardIndex: %d  HasLock: yes  Strength: %d  Quality: %d%s\n", *info, device->CardIndex(), device->SignalStrength(), device->SignalQuality(), live ? "  Live: yes" : "");
         else
            info = cString::sprintf("%sCardIndex: %d  HasLock: no\n", *info, device->CardIndex());
         if (channel) {
            if (channel->Number() > 0 && device->Receiving())
               info = cString::sprintf("%sTransponder: %d  Channel: %s\n", *info, channel->Transponder(), channel->Name());
            else
               info = cString::sprintf("%sTransponder: %d\n", *info, channel->Transponder());
            }
         if (timers)
            info = cString::sprintf("%sRecording: %d timer%s\n", *info, timers, (timers > 1) ? "s" : "");
         info = cString::sprintf("%s\n", *info);
         }
      }
  return isempty(*info) ? cString(tr("SAT>IP information not available!")) : info;
}

cString cSatipDevice::GetGeneralInformation(void)
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  LOCK_CHANNELS_READ;
  return cString::sprintf("SAT>IP device: %d\nCardIndex: %d\nStream: %s\nSignal: %s\nStream bitrate: %s\n%sChannel: %s\n",
                          deviceIndex, CardIndex(),
                          tuner ? *tuner->GetInformation() : "",
                          tuner ? *tuner->GetSignalStatus() : "",
                          tuner ? *tuner->GetTunerStatistic() : "",
                          *GetBufferStatistic(),
                          *Channels->GetByNumber(cDevice::CurrentChannel())->ToText());
}

cString cSatipDevice::GetPidsInformation(void)
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  return GetPidStatistic();
}

cString cSatipDevice::GetFiltersInformation(void)
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  return cString::sprintf("Active section filters:\n%s", pSectionFilterHandlerM ? *pSectionFilterHandlerM->GetInformation() : "");
}

cString cSatipDevice::GetInformation(unsigned int pageP)
{
  // generate information string
  cString s;
  switch (pageP) {
    case SATIP_DEVICE_INFO_GENERAL:
         s = GetGeneralInformation();
         break;
    case SATIP_DEVICE_INFO_PIDS:
         s = GetPidsInformation();
         break;
    case SATIP_DEVICE_INFO_FILTERS:
         s = GetFiltersInformation();
         break;
    case SATIP_DEVICE_INFO_PROTOCOL:
         s = tuner ? *tuner->GetInformation() : "";
         break;
    case SATIP_DEVICE_INFO_BITRATE:
         s = tuner ? *tuner->GetTunerStatistic() : "";
         break;
    default:
         s = cString::sprintf("%s%s%s",
                              *GetGeneralInformation(),
                              *GetPidsInformation(),
                              *GetFiltersInformation());
         break;
    }
  return s;
}

bool cSatipDevice::Ready(void)
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  return ((cSatipDiscover::GetInstance()->GetServerCount() > 0) || (createdM.Elapsed() > eReadyTimeoutMs));
}

cString cSatipDevice::DeviceType(void) const
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  return "SAT>IP";
}

cString cSatipDevice::DeviceName(void) const
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  std::string sys;
  const char* s = "ACST";
  for(int i=0; s[i]; i++)
     if (ProvidesSource(s[i] << 24))
        sys += s[i];

  return cString::sprintf("%s %d (%s)", *DeviceType(), deviceIndex, sys.c_str());
}

bool cSatipDevice::AvoidRecording(void) const
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  return SatipConfig.IsOperatingModeLow();
}

bool cSatipDevice::SignalStats(int &Valid, double *Strength, double *Cnr, double *BerPre, double *BerPost, double *Per, int *Status) const
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  Valid = DTV_STAT_VALID_NONE;
  if (Strength && tuner) {
     *Strength =  tuner->SignalStrengthDBm();
     if (*Strength < -18.0) /* valid: -71.458 .. -18.541, invalid: 0.0 */
        Valid |= DTV_STAT_VALID_STRENGTH;
     }
  if (Status) {
     *Status = HasLock() ? (DTV_STAT_HAS_SIGNAL | DTV_STAT_HAS_CARRIER | DTV_STAT_HAS_VITERBI | DTV_STAT_HAS_SYNC | DTV_STAT_HAS_LOCK) : DTV_STAT_HAS_NONE;
     Valid |= DTV_STAT_VALID_STATUS;
     }
  return Valid != DTV_STAT_VALID_NONE;
}

int cSatipDevice::SignalStrength(void) const
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  return (tuner ? tuner->SignalStrength() : -1);
}

int cSatipDevice::SignalQuality(void) const
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  return (tuner ? tuner->SignalQuality() : -1);
}

bool cSatipDevice::ProvidesSource(int sourceP) const
{
  cSource *s = Sources.Get(sourceP);
  dbg_chan_switch("%s (%c) desc='%s' [device %u]", __PRETTY_FUNCTION__, cSource::ToChar(sourceP), s ? s->Description() : "", deviceIndex);
  if (SatipConfig.GetDetachedMode())
     return false;
  // source descriptions starting with '0' are disabled
  if (s && s->Description() && (*(s->Description()) == '0'))
     return false;
  if (!SatipConfig.IsOperatingModeOff() && !!cSatipDiscover::GetInstance()->GetServer(sourceP)) {
     int numDisabledSourcesM = SatipConfig.GetDisabledSourcesCount();
     for (int i = 0; i < numDisabledSourcesM; ++i) {
         if (sourceP == SatipConfig.GetDisabledSources(i))
            return false;
         }
     return true;
     }
  return false;
}

bool cSatipDevice::ProvidesTransponder(const cChannel *channelP) const
{
  dbg_chan_switch("%s (%d) transponder=%d source=%c [device %u]", __PRETTY_FUNCTION__, channelP ? channelP->Number() : -1, channelP ? channelP->Transponder() : -1, channelP ? cSource::ToChar(channelP->Source()) : '?', deviceIndex);
  if (!ProvidesSource(channelP->Source()))
     return false;
  return DeviceHooksProvidesTransponder(channelP);
}

bool cSatipDevice::ProvidesChannel(const cChannel *channelP, int priorityP, bool *needsDetachReceiversP) const
{
  bool result = false;
  bool hasPriority = (priorityP == IDLEPRIORITY) || (priorityP > this->Priority());
  bool needsDetachReceivers = false;

  dbg_chan_switch("%s (%d, %d, %d) [device %u]", __PRETTY_FUNCTION__, channelP ? channelP->Number() : -1, priorityP, !!needsDetachReceiversP, deviceIndex);

  if (channelP && ProvidesTransponder(channelP)) {
     result = hasPriority;
     if (priorityP > IDLEPRIORITY) {
        if (Receiving()) {
           if (IsTunedToTransponder(channelP)) {
              if (channelP->Vpid() && !HasPid(channelP->Vpid()) || channelP->Apid(0) && !HasPid(channelP->Apid(0)) || channelP->Dpid(0) && !HasPid(channelP->Dpid(0))) {
                 if (CamSlot() && channelP->Ca() >= CA_ENCRYPTED_MIN) {
                    if (CamSlot()->CanDecrypt(channelP))
                       result = true;
                    else
                       needsDetachReceivers = true;
                    }
                 else
                    result = true;
                 }
              else
                 result = !!SatipConfig.GetFrontendReuse();
              }
           else
              needsDetachReceivers = true;
           }
        }
     }
  if (needsDetachReceiversP)
     *needsDetachReceiversP = needsDetachReceivers;
  return result;
}

bool cSatipDevice::ProvidesEIT(void) const
{
#if defined(APIVERSNUM) && APIVERSNUM < 20403
  return (SatipConfig.GetEITScan());
#else
  return (SatipConfig.GetEITScan()) && DeviceHooksProvidesEIT();
#endif
}

int cSatipDevice::NumProvidedSystems(void) const
{
  int count = cSatipDiscover::GetInstance()->NumProvidedSystems();
  // Tweak the count according to operation mode
  if (SatipConfig.IsOperatingModeLow())
     count = 15;
  else if (SatipConfig.IsOperatingModeHigh())
     count = 1;
  // Clamp the count between 1 and 15
  if (count > 15)
     count = 15;
  else if (count < 1)
     count = 1;
  return count;
}

const cChannel *cSatipDevice::GetCurrentlyTunedTransponder(void) const
{
  return &channelM;
}

bool cSatipDevice::IsTunedToTransponder(const cChannel *channelP) const
{
  if (tuner && !tuner->IsTuned())
     return false;
  if ((channelM.Source() != channelP->Source()) || (channelM.Transponder() != channelP->Transponder()))
     return false;
  return (strcmp(channelM.Parameters(), channelP->Parameters()) == 0);
}

bool cSatipDevice::MaySwitchTransponder(const cChannel *channelP) const
{
  return cDevice::MaySwitchTransponder(channelP);
}

bool cSatipDevice::SetChannelDevice(const cChannel* channel, bool liveView)
{
  cMutexLock MutexLock(&SetChannelMtx);  // Global lock to prevent any simultaneous zapping
  dbg_chan_switch("%s (%d, %d) [device %u]",
      __PRETTY_FUNCTION__, channel ? channel->Number() : -1, liveView, deviceIndex);

  if (tuner == nullptr) {
     dbg_chan_switch("%s [device %u] -> false (no tuner)", deviceIndex);
     return false;
     }

  if (channel) {
     cDvbTransponderParameters dtp(channel->Parameters());
     std::string params = GetTransponderUrlParameters(channel);
     if (params.empty()) {
        error("Unrecognized channel parameters: %s [device %u]", channel->Parameters(), deviceIndex);
        return false;
        }

     cSatipServer *server = cSatipDiscover::GetInstance()->AssignServer(deviceIndex, channel->Source(), channel->Transponder(), dtp.System());
     if (!server) {
        dbg_chan_switch("%s No suitable server found [device %u]", __PRETTY_FUNCTION__, deviceIndex);
        return false;
        }
     if (tuner->SetSource(server, channel->Transponder(), params.c_str(), deviceIndex)) {
        channelM = *channel;
        // Wait for actual channel tuning to prevent simultaneous frontend allocation failures
        tunedM.TimedWait(SetChannelMtx, eTuningTimeoutMs);
        return true;
        }
     }
  else {
     tuner->SetSource(NULL, 0, NULL, deviceIndex);
     }
  return true;
}

void cSatipDevice::SetChannelTuned(void)
{
  dbg_chan_switch("%s () [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  // Release immediately any pending conditional wait
  tunedM.Broadcast();
}

bool cSatipDevice::SetPid(cPidHandle *handleP, int typeP, bool onP)
{
  dbg_pids("%s (%d, %d, %d) [device %u]", __PRETTY_FUNCTION__, handleP ? handleP->pid : -1, typeP, onP, deviceIndex);
  if (tuner && handleP && handleP->pid >= 0 && handleP->pid <= 8191) {
     if (onP)
        return tuner->SetPid(handleP->pid, typeP, true);
     else if (!handleP->used && pSectionFilterHandlerM && !pSectionFilterHandlerM->Exists(handleP->pid))
        return tuner->SetPid(handleP->pid, typeP, false);
     }
  return true;
}

int cSatipDevice::OpenFilter(u_short pidP, u_char tidP, u_char maskP)
{
  dbg_pids("%s (%d, %02X, %02X) [device %d]", __PRETTY_FUNCTION__, pidP, tidP, maskP, deviceIndex);
  if (pSectionFilterHandlerM) {
     int handle = pSectionFilterHandlerM->Open(pidP, tidP, maskP);
     if (tuner && (handle >= 0))
        tuner->SetPid(pidP, ptOther, true);
     return handle;
     }
  return -1;
}

void cSatipDevice::CloseFilter(int handleP)
{
  if (pSectionFilterHandlerM) {
     int pid = pSectionFilterHandlerM->GetPid(handleP);
     dbg_pids("%s (%d) [device %u]", __PRETTY_FUNCTION__, pid, deviceIndex);
     if (tuner)
        tuner->SetPid(pid, ptOther, false);
     pSectionFilterHandlerM->Close(handleP);
     }
}

bool cSatipDevice::OpenDvr(void)
{
  dbg_chan_switch("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  bytesDelivered = 0;
  tsBufferM->Clear();
  if (tuner)
     tuner->Open();
  isOpenDvrM = true;
  return true;
}

void cSatipDevice::CloseDvr(void)
{
  dbg_chan_switch("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  if (tuner)
     tuner->Close();
  isOpenDvrM = false;
}

bool cSatipDevice::HasLock(int timeoutMsP) const
{
  dbg_funcname_ext("%s (%d) [device %d]", __PRETTY_FUNCTION__, timeoutMsP, deviceIndex);
  if (timeoutMsP > 0) {
     cTimeMs timer(timeoutMsP);
     while (!timer.TimedOut()) {
           if (tuner && tuner->HasLock())
              return true;
           cCondWait::SleepMs(100);
           }
     }
  return (tuner && tuner->HasLock());
}

bool cSatipDevice::HasInternalCam(void)
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  return SatipConfig.GetCIExtension();
}

void cSatipDevice::WriteData(uchar *bufferP, int lengthP)
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  // Fill up TS buffer
  if (isOpenDvrM && tsBufferM) {
     int len = tsBufferM->Put(bufferP, lengthP);
     if (len != lengthP)
        tsBufferM->ReportOverflow(lengthP - len);
     }
  // Filter the sections
  if (pSectionFilterHandlerM)
     pSectionFilterHandlerM->Write(bufferP, lengthP);
}

int cSatipDevice::GetId(void)
{
  return deviceIndex;
}

int cSatipDevice::GetPmtPid(void)
{
  int pid = channelM.Ca() ? ::GetPmtPid(channelM.Source(), channelM.Transponder(), channelM.Sid()) : 0;
  dbg_ci("%s pmtpid=%d source=%c transponder=%d sid=%d name=%s [device %u]", __PRETTY_FUNCTION__, pid, cSource::ToChar(channelM.Source()), channelM.Transponder(), channelM.Sid(), channelM.Name(), deviceIndex);
  return pid;
}

int cSatipDevice::GetCISlot(void)
{
  int slot = 0;
  int ca = 0;
  for (const int *id = channelM.Caids(); *id; ++id) {
      if (checkCASystem(SatipConfig.GetCICAM(0), *id)) {
         ca = *id;
         slot = 1;
         break;
         }
      else if (checkCASystem(SatipConfig.GetCICAM(1), *id)) {
         ca = *id;
         slot = 2;
         break;
         }
      }
  dbg_ci("%s slot=%d ca=%X name=%s [device %u]", __PRETTY_FUNCTION__, slot, ca, channelM.Name(), deviceIndex);
  return slot;
}

cString cSatipDevice::GetTnrParameterString(void)
{
   if (channelM.Ca())
      return GetTnrUrlParameters(&channelM).c_str();
   return NULL;
}

bool cSatipDevice::IsIdle(void)
{
  return !Receiving();
}

uchar *cSatipDevice::GetData(int *availableP, bool checkTsBuffer)
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  if (isOpenDvrM && tsBufferM) {
     int count = 0;
     if (bytesDelivered) {
        tsBufferM->Del(bytesDelivered);
        bytesDelivered = 0;
        }
     if (checkTsBuffer && tsBufferM->Available() < TS_SIZE)
        return NULL;
     uchar *p = tsBufferM->Get(count);
     if (p && count >= TS_SIZE) {
        if (*p != TS_SYNC_BYTE) {
           for (int i = 1; i < count; i++) {
               if (p[i] == TS_SYNC_BYTE) {
                  count = i;
                  break;
                  }
               }
           tsBufferM->Del(count);
           info("Skipped %d bytes to sync on TS packet", count);
           return NULL;
           }
        bytesDelivered = TS_SIZE;
        if (availableP)
           *availableP = count;
        // Update pid statistics
        AddPidStatistic(ts_pid(p), payload(p));
        return p;
        }
     }
  return NULL;
}

void cSatipDevice::SkipData(int countP)
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  bytesDelivered = countP;
  // Update buffer statistics
  AddBufferStatistic(countP, tsBufferM->Available());
}

bool cSatipDevice::GetTSPacket(uchar *&dataP)
{
  dbg_funcname_ext("%s [device %u]", __PRETTY_FUNCTION__, deviceIndex);
  if (SatipConfig.GetDetachedMode())
     return false;
  if (tsBufferM) {
     if (cCamSlot *cs = CamSlot()) {
        if (cs->WantsTsData()) {
           int available;
           dataP = GetData(&available, checkTsBufferM);
           if (!dataP)
              available = 0;
           dataP = cs->Decrypt(dataP, available);
           SkipData(available);
           checkTsBufferM = dataP != NULL;
           return true;
           }
        }
     dataP = GetData();
     return true;
     }
  dataP = NULL;
  return true;
}
