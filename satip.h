/*******************************************************************************
 * satip.h: A plugin for the Video Disk Recorder
 * See the README file for copyright information and how to reach the author.
 ******************************************************************************/
#pragma once
#include <vdr/plugin.h>

/*******************************************************************************
 * forward decls.
 ******************************************************************************/
class cSatipDiscoverServers;



/*******************************************************************************
 * class cPluginSatip
 ******************************************************************************/
class cPluginSatip : public cPlugin {
private:
  unsigned int deviceCountM;
  cSatipDiscoverServers *serversM;
  void ParseServer(const char *paramP);
  void ParsePortRange(const char *paramP);
  int ParseCicams(const char *valueP, int *cicamsP);
  int ParseSources(const char *valueP, int *sourcesP);
  int ParseFilters(const char *valueP, int *filtersP);
public:
  cPluginSatip(void);
  virtual ~cPluginSatip();
  virtual const char *Version(void);
  virtual const char *Description(void);
  virtual const char *CommandLineHelp(void);
  virtual bool ProcessArgs(int argc, char *argv[]);
  virtual bool Initialize(void);
  virtual bool Start(void);
  virtual void Stop(void);
  virtual void Housekeeping(void);
  virtual void MainThreadHook(void);
  virtual cString Active(void);
  virtual time_t WakeupTime(void);
  virtual const char *MainMenuEntry(void);
  virtual cOsdObject *MainMenuAction(void);
  virtual cMenuSetupPage *SetupMenu(void);
  virtual bool SetupParse(const char *Name, const char *Value);
  virtual bool Service(const char *Id, void *Data = NULL);
  virtual const char **SVDRPHelpPages(void);
  virtual cString SVDRPCommand(const char *Command, const char *Option, int &ReplyCode);
};
