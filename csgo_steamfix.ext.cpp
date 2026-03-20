/*
 * csgo_steamfix.ext.cpp
 *
 * SourceMod Extension — патч jump-таблицы engine.so
 *
 * Сборка:
 *   g++ -m32 -shared -fPIC -O2 -std=c++14 \
 *       -I/path/to/sourcemod/public \
 *       -I/path/to/amtl \
 *       -I/path/to/amtl/amtl \
 *       -I/path/to/sourcepawn/include \
 *       -o csgo_steamfix.ext.so csgo_steamfix.ext.cpp -ldl
 *
 * License: MIT
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <sys/mman.h>
#include <link.h>
#include <dlfcn.h>

#include "IExtensionSys.h"

using namespace SourceMod;

/* =========================================================================
 * Патч движка
 * ========================================================================= */
static const uint8_t SIG[]  = {
    0xFF, 0x24, 0x85, 0x00, 0x00, 0x00, 0x00,
    0x8D, 0xB4, 0x26, 0x00, 0x00, 0x00, 0x00,
    0x31, 0xF6
};
static const uint8_t MASK[] = {
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF
};
static constexpr size_t SIG_LEN = sizeof(SIG);

static uint32_t *g_jt4_ptr  = nullptr;
static uint32_t  g_jt4_orig = 0;
static bool      g_patched  = false;

static bool make_writable(void *addr, size_t len)
{
    uintptr_t page = reinterpret_cast<uintptr_t>(addr);
    page &= ~static_cast<uintptr_t>(getpagesize() - 1);
    return mprotect(reinterpret_cast<void*>(page),
                    len + (reinterpret_cast<uintptr_t>(addr) - page),
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

static const uint8_t *find_sig(const uint8_t *base, size_t size)
{
    if (size < SIG_LEN) return nullptr;
    const uint8_t *end = base + size - SIG_LEN;
    for (const uint8_t *p = base; p <= end; ++p) {
        size_t i = 0;
        for (; i < SIG_LEN; ++i)
            if ((p[i] & MASK[i]) != (SIG[i] & MASK[i])) break;
            if (i == SIG_LEN) return p;
    }
    return nullptr;
}

struct EngineInfo { const uint8_t *base = nullptr; size_t size = 0; };

static int phdr_cb(struct dl_phdr_info *info, size_t, void *data)
{
    if (!info->dlpi_name || !strstr(info->dlpi_name, "engine.so"))
        return 0;
    auto *ei = static_cast<EngineInfo*>(data);
    ei->base = reinterpret_cast<const uint8_t*>(
        static_cast<uintptr_t>(info->dlpi_addr));
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            uintptr_t end = info->dlpi_phdr[i].p_vaddr
            + info->dlpi_phdr[i].p_memsz;
            if (end > ei->size) ei->size = end;
        }
        return 1;
}

/* Попробовать найти движок через dlopen если phdr не нашёл */
static void try_find_via_dlopen(EngineInfo *ei)
{
    void *h = dlopen("engine.so", RTLD_NOLOAD | RTLD_NOW);
    if (!h) return;

    /* Получить любой символ чтобы найти базу */
    struct link_map *lm = nullptr;
    if (dlinfo(h, RTLD_DI_LINKMAP, &lm) == 0 && lm) {
        ei->base = reinterpret_cast<const uint8_t*>(
            static_cast<uintptr_t>(lm->l_addr));
        /* Размер поставим большой — просканируем 64MB */
        ei->size = 0x4000000;
    }
    dlclose(h);
}

static bool do_patch(char *err, size_t errlen)
{
    if (g_patched) return true;

    EngineInfo ei;
    dl_iterate_phdr(phdr_cb, &ei);

    if (!ei.base || !ei.size) {
        printf("[steamfix] phdr не нашёл engine.so, пробуем dlopen...\n");
        try_find_via_dlopen(&ei);
    }

    if (!ei.base || !ei.size) {
        snprintf(err, errlen, "[steamfix] engine.so не найден");
        return false;
    }

    printf("[steamfix] engine base=0x%08X size=0x%zX\n",
           (uint32_t)(uintptr_t)ei.base, ei.size);

    const uint8_t *hit = find_sig(ei.base, ei.size);
    if (!hit) {
        snprintf(err, errlen, "[steamfix] сигнатура не найдена");
        return false;
    }

    printf("[steamfix] sig hit rva=0x%X\n", (uint32_t)(hit - ei.base));

    uint32_t jt_addr;
    memcpy(&jt_addr, hit + 3, sizeof(jt_addr));
    uint32_t *jt = reinterpret_cast<uint32_t*>((uintptr_t)jt_addr);

    printf("[steamfix] jt=0x%08X  jt[0]=0x%08X  jt[4]=0x%08X\n",
           jt_addr, jt[0], jt[4]);

    if (jt[0] == jt[4]) {
        printf("[steamfix] уже пропатчено\n");
        g_patched = true;
        return true;
    }

    if (!make_writable(&jt[4], sizeof(uint32_t))) {
        snprintf(err, errlen, "[steamfix] mprotect не удался");
        return false;
    }

    g_jt4_ptr  = &jt[4];
    g_jt4_orig = jt[4];

    printf("[steamfix] jt[4]: 0x%08X -> 0x%08X\n", jt[4], jt[0]);
    jt[4] = jt[0];
    g_patched = true;
    printf("[steamfix] engine patched!\n");
    return true;
}

static void do_unpatch()
{
    if (!g_patched || !g_jt4_ptr) return;
    *g_jt4_ptr = g_jt4_orig;
    g_patched  = false;
    g_jt4_ptr  = nullptr;
    printf("[steamfix] engine restored\n");
}

/* =========================================================================
 * Extension class
 * ========================================================================= */
class CSGOSteamFix : public IExtensionInterface
{
public:
    bool OnExtensionLoad(IExtension *me, IShareSys *sys,
                         char *error, size_t maxlength, bool late) override
                         {
                             printf("[steamfix] loading..\n");

                             /* Если late load — патчим сразу, движок уже загружен */
                             if (late) {
                                 return do_patch(error, maxlength);
                             }
                             return true;
                         }

                         void OnExtensionUnload() override { do_unpatch(); }

                         /* Вызывается после того как все модули загружены — движок точно есть */
                         void OnExtensionsAllLoaded() override
                         {
                             if (!g_patched) {
                                 char err[256] = {};
                                 if (!do_patch(err, sizeof(err))) {
                                     printf("[steamfix] ERROR: %s\n", err);
                                 }
                             }
                         }

                         void OnExtensionPauseChange(bool) override  { }
                         bool QueryRunning(char*, size_t) override   { return true; }
                         bool IsMetamodExtension() override          { return false; }
                         void OnDependenciesDropped() override       { }

                         const char *GetExtensionName()        override { return "CS:GO Steam Fix"; }
                         const char *GetExtensionURL()         override { return "https://github.com/eonexdev/csgo-sv-fix-engine"; }
                         const char *GetExtensionTag()         override { return "steamfix"; }
                         const char *GetExtensionAuthor()      override { return "opensource"; }
                         const char *GetExtensionVerString()   override { return "1.0.0"; }
                         const char *GetExtensionDescription() override { return "Allow archived CS:GO clients to join"; }
                         const char *GetExtensionDateString()  override { return __DATE__; }
};

static CSGOSteamFix g_Extension;

extern "C" __attribute__((visibility("default")))
IExtensionInterface *GetSMExtAPI()
{
    return &g_Extension;
}
