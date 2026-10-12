/* NID -> function name and the result with which the export reports a state its caller polls or
 * waits for (no event pending, timed out, busy, would block), not a failure: the call report counts
 * these apart. Sorted by NID in strcmp order, then by result, for a binary search. Kept by hand from
 * the codes the runtime's implementations return. */
{"12wOHk8ywb0","sceKernelPollSema",0x80020010},
{"656LMQSrg6U","sceSystemServiceReceiveEvent",0x80a10004},
{"9lvj5DjHZiA","sceKernelPollEventFlag",0x80020010},
{"BmMjYxmew1w","scePthreadCondTimedwait",0x8002003c},
{"H2a+IN9TP0E","scePthreadSemTrywait",0x80020023},
{"IafI2PxcPnQ","scePthreadMutexTimedlock",0x8002003c},
{"JTvBflhYazQ","sceKernelWaitEventFlag",0x8002003c},
{"WBWzsRifCEA","sem_trywait",35},
{"XD3mDeybCnk","scePthreadRwlockTryrdlock",0x80020010},
{"Zxa0VhQVTsk","sceKernelWaitSema",0x8002003c},
{"adh--6nIqTk","scePthreadRwlockTimedwrlock",0x8002003c},
{"bIHoZCTomsI","scePthreadRwlockTrywrlock",0x80020010},
{"fjN6NQHhK8k","scePthreadSemTimedwait",0x8002003c},
{"fzyMKs9kim0","sceKernelWaitEqueue",0x8002003c},
{"iPtZRWICjrM","scePthreadRwlockTimedrdlock",0x8002003c},
{"upoVrzMHFeE","scePthreadMutexTrylock",0x80020010},
{"w5IHyvahg-o","sem_timedwait",60},
{"yH17Q6NWtVg","sceUserServiceGetEvent",0x80960007},
