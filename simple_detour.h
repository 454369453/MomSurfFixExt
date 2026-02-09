#ifndef _SIMPLE_DETOUR_H_
#define _SIMPLE_DETOUR_H_

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

class CSimpleDetour
{
public:
    CSimpleDetour(void *pTarget, void *pHook);
    ~CSimpleDetour();

    bool Enable();
    void Disable();
    void *GetTrampoline() const { return m_pTrampoline; }

private:
    void *m_pTarget;
    void *m_pHook;
    void *m_pTrampoline;
    uint8_t m_OriginalBytes[5];
    bool m_bEnabled;
    bool m_bCreated;
};

#endif
