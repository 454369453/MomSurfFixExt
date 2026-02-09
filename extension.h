#ifndef _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_

#include "smsdk_ext.h"
// 【修改】直接引用，因为我们把 public/CDetour 加入了 include path
#include <idetour.h> 

class MomSurfFixExt : public SDKExtension
{
public:
    virtual bool SDK_OnLoad(char *error, size_t maxlength, bool late);
    virtual void SDK_OnUnload();
    virtual void SDK_OnAllLoaded();
    virtual bool QueryRunning(char *error, size_t maxlength);
};

extern MomSurfFixExt g_MomSurfFixExt;

#endif
