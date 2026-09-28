/*
 * Linux/sun3 boot program for the Sun-3 PROM (3.60 v3.0.1 and its kin).
 *
 * What the PROM does for `b sd(c,u,p)` (ROM 0x5A00..0x5F0C, 3.60 v3.0.1):
 * it fills the bootparam block (romvec->bootParam: device name, ctlr,
 * unit, partition, file name, argv, and bootDevice = the device's boottab),
 * calls the device's b_probe, then the generic b_boot, which reads
 * d_maxiobytes (8 KiB for sd) from block 1 of the partition to physical
 * 0x4000 through b_strategy and jumps there.  So a boot block is at most
 * 8 KiB, lives in sectors 1..16 (sector 0 is the label), is entered in
 * supervisor mode with the monitor's context-0 maps (low 8 MB identity,
 * ROM and devices at 0x0FE00000+) and the monitor's stack, and gets
 * nothing else: NetBSD's bootxx and SunOS's bootsd both re-open the boot
 * device themselves through bootParam->bootDevice, as we do here.
 *
 * The device interface (SunOS saio.h, transcribed from the ROM's sd
 * driver and the two boot blocks):
 *   b_open(si)   needs si_devdata (d_localbytes of scratch, zeroed by the
 *                driver), si_dmaaddr (d_dmabytes: 8 KiB of data + the
 *                UDC chain table) and, for devices with d_devbytes,
 *                si_devaddr (the registers mapped by the caller); reads
 *                the label and turns si_boff (the partition number) into
 *                si_cyloff (its first sector).  Returns -1 on failure.
 *   b_strategy(si, 1)  reads si_cc bytes (<= d_maxiobytes) of block
 *                si_bn (partition-relative; the driver adds si_cyloff
 *                *into* si_bn) into si_dmaaddr by DMA and copies them to
 *                si_ma; returns the byte count, 0 on error.
 *   b_close(si).
 * The DMA address the driver programs is si_dmaaddr & 0xFFFFFF, which the
 * Sun-3 DVMA hardware translates as VA 0x0F000000 | addr in the current
 * context, so si_dmaaddr must be a VA in the top 16 MB that maps the same
 * memory the CPU sees.  NetBSD's bootxx solves that by aliasing the DVMA
 * segment 0x0FF00000 onto the segment it runs in (segmap copy); we do the
 * same for our own segment at RELOC.
 *
 * Disk format (written by mk-bootdisk.py): Sun label in sector 0, this
 * program in sectors 1..16, a header in sector HDR_BLOCK (17) listing the
 * segments to load (partition-relative sector, byte size, physical
 * destination), the segments from sector 32 up.  See struct bootimg_hdr.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define ROMVEC_VA       0x0FEF0000
#define RELOC           0x780000        /* boot.ld */
#define DVMA_SEG_VA     0x0FF00000      /* the PROM's DVMA segment */
#define DEV_MAP_VA      0x0FF20000      /* where we map device registers */
#define PAGE_SIZE       8192
#define AC_PAGEMAP      0x10000000
#define AC_SEGMAP       0x20000000
#define PTE_VALID       0x80000000
#define PTE_WRITE       0x40000000
#define PTE_SYSTEM      0x20000000
#define PTE_NC          0x10000000
#define PTE_TYPE_SHIFT  26
#define SAIO_F_READ     1

#define HDR_BLOCK       17
#define HDR_MAGIC       "SUN3LNX1"
#define HDR_MAX_SEGS    30

struct bootimg_seg {
    u32 lba;                /* first sector, relative to the partition */
    u32 size;               /* bytes */
    u32 dest;               /* physical (= virtual, identity map) address */
    u32 flags;              /* 0 */
};

struct bootimg_hdr {
    char magic[8];          /* HDR_MAGIC */
    u32 entry;              /* physical address to jump to */
    u32 nseg;
    struct bootimg_seg seg[HDR_MAX_SEGS];
};

/* SunOS 4 sun3 saio.h: Sun's compiler aligned these to 2 bytes */
struct devinfo {
    u32 d_devbytes;
    u32 d_dmabytes;
    u32 d_localbytes;
    u32 d_stdcount;
    u32 *d_stdaddrs;
    u32 d_devtype;          /* PTE type: 0 obmem, 1 obio, 2/3 VME */
    u32 d_maxiobytes;
};

struct saioreq;
struct boottab {
    char b_dev[2];
    int (*b_probe)(struct saioreq *);
    int (*b_boot)(struct saioreq *);
    int (*b_open)(struct saioreq *);
    int (*b_close)(struct saioreq *);
    int (*b_strategy)(struct saioreq *, int rw);
    char *b_desc;
    struct devinfo *b_devinfo;
} __attribute__((packed));

struct saioreq {
    char si_flgs;
    char si_pad;
    struct boottab *si_boottab;
    char *si_devdata;
    int si_ctlr;
    int si_unit;
    int si_boff;            /* partition number in, ... */
    int si_cyloff;          /* ... its first sector out (b_open) */
    int si_offset;
    int si_bn;
    char *si_ma;
    int si_cc;
    void *si_sif;
    char *si_devaddr;
    char *si_dmaaddr;
} __attribute__((packed));

struct bootparam {
    char *argPtr[8];
    char strings[100];
    char devName[2];
    int ctlrNum;
    int unitNum;
    int partNum;
    char *fileName;
    struct boottab *bootDevice;
} __attribute__((packed));

/* the Sun-3 PROM vector (NetBSD mon.h / Linux openprom.h), first 0xE0 bytes */
struct romvec {
    char *initSp;
    int (*startMon)(void);
    int *diagberr;
    struct bootparam **bootParam;
    u32 *memorySize;
    int (*getChar)(void);
    int (*putChar)(int);
    int (*mayGet)(void);
    int (*mayPut)(int);
    u8 *echo;
    u8 *inSource;
    u8 *outSink;
    int (*getKey)(void);
    int (*initGetKey)(void);
    u32 *translation;
    u8 *keyBid;
    int *screen_x;
    int *screen_y;
    void *keyBuf;
    char *monId;
    int (*fbWriteChar)(int);
    int *fbAddr;
    char **font;
    int (*fbWriteStr)(char *, int);
    void (*reBoot)(char *);
    u8 *lineBuf;
    u8 **linePtr;
    int *lineSize;
    int (*getLine)(int);
    u8 (*getNextChar)(void);
    u8 (*peekNextChar)(void);
    int *fbThere;
    int (*getNum)(void);
    int (*printf)(const char *, ...);
    void (*printHex)(int);
    u8 *leds;
    void (*setLeds)(int);
    void (*nmiAddr)(void);
    void (*abortEntry)(void);
    int *nmiClock;
    int *fbType;
    u32 romvecVersion;
    void *globRam;
    void *kbdZscc;
    int *keyrInit;
    u8 *keyrTick;
    u32 *memoryAvail;
    long *resetAddr;
    long *resetMap;
    void (*exitToMon)(void);
    u8 *memoryBitmap;
    void (*setcxsegmap)(int, char *, int);
    void (**vector_cmd)(void);
};

#define romvec ((struct romvec *)ROMVEC_VA)

/* --- console through the PROM's non-blocking putchar --- */

static void putc(int c)
{
    if (c == '\n') {
        putc('\r');
    }
    while (romvec->mayPut(c) == -1) {
        /* transmitter busy */
    }
}

static void puts(const char *s)
{
    while (*s) {
        putc(*s++);
    }
}

static void puthex(u32 v)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    puts("0x");
    for (i = 28; i >= 0; i -= 4) {
        putc(digits[(v >> i) & 15]);
    }
}

static void putdec(u32 v)
{
    char buf[12];
    int i = sizeof(buf);

    buf[--i] = 0;
    do {
        u32 q = v / 10;

        buf[--i] = '0' + (v - q * 10);
        v = q;
    } while (v);
    puts(buf + i);
}

/* --- the Sun-3 MMU, through control space --- */

static void set_fc(int fc)
{
    __asm__ volatile("movec %0,%%sfc\n\tmovec %0,%%dfc" : : "d"(fc));
}

static u8 get_segmap(u32 va)
{
    u8 v;

    __asm__ volatile("movesb (%1),%0" : "=d"(v) : "a"(AC_SEGMAP | va));
    return v;
}

static void set_segmap(u32 va, u8 pmeg)
{
    __asm__ volatile("movesb %0,(%1)" : : "d"(pmeg), "a"(AC_SEGMAP | va)
                     : "memory");
}

static void set_pte(u32 va, u32 pte)
{
    __asm__ volatile("movesl %0,(%1)" : : "d"(pte), "a"(AC_PAGEMAP | va)
                     : "memory");
}

/*
 * Map d_devbytes of device space (PTE type d_devtype at physical addr)
 * at DEV_MAP_VA, the way bootsd's dev_mapin does.  Not needed for sd
 * (d_devbytes 0: the ROM's si driver uses the monitor's own mapping).
 */
static char *dev_mapin(u32 type, u32 addr, u32 len)
{
    u32 va = DEV_MAP_VA;
    u32 pfn = addr / PAGE_SIZE;
    u32 end = (addr + len + PAGE_SIZE - 1) / PAGE_SIZE;

    for (; pfn < end; pfn++, va += PAGE_SIZE) {
        set_pte(va, PTE_VALID | PTE_WRITE | PTE_SYSTEM | PTE_NC |
                    (type << PTE_TYPE_SHIFT) | pfn);
    }
    return (char *)(DEV_MAP_VA + (addr & (PAGE_SIZE - 1)));
}

/* --- the boot device --- */

static struct saioreq si;
static char devdata[256];
static u8 dmabuf[0x2020 + 16] __attribute__((aligned(16)));
static struct bootimg_hdr hdr __attribute__((aligned(4)));

static int dev_open(struct bootparam *bp)
{
    struct boottab *bt = bp->bootDevice;
    struct devinfo *di = bt->b_devinfo;

    if (di->d_localbytes > sizeof(devdata) ||
        di->d_dmabytes > sizeof(dmabuf) || di->d_maxiobytes < 512) {
        puts("boot: device needs more than we carry\n");
        return -1;
    }
    si.si_flgs = 0;
    si.si_boottab = bt;
    si.si_ctlr = bp->ctlrNum;
    si.si_unit = bp->unitNum;
    si.si_boff = bp->partNum;
    si.si_devdata = devdata;
    si.si_devaddr = 0;
    if (di->d_devbytes) {
        u32 ctlr = bp->ctlrNum < (int)di->d_stdcount ? bp->ctlrNum : 0;

        si.si_devaddr = dev_mapin(di->d_devtype, di->d_stdaddrs[ctlr],
                                  di->d_devbytes);
    }
    /* DVMA: alias the PROM's DVMA segment onto ours (NetBSD bootxx) */
    set_segmap(DVMA_SEG_VA, get_segmap(RELOC));
    si.si_dmaaddr = (char *)(DVMA_SEG_VA + ((u32)dmabuf - RELOC));

    return bt->b_open(&si) == -1 ? -1 : 0;
}

/* whole sectors: the driver rounds the CDB up and reports that count */
static int dev_read(u32 blk, void *buf, u32 len)
{
    len = (len + 511) & ~511u;
    si.si_bn = blk;
    si.si_ma = buf;
    si.si_cc = len;
    si.si_offset = 0;
    return si.si_boottab->b_strategy(&si, SAIO_F_READ) == (int)len ? 0 : -1;
}

static int load_seg(const struct bootimg_seg *s, u32 maxio)
{
    u32 done = 0, dots = 0;

    puts("  ");
    puthex(s->dest);
    puts(" <- sector ");
    putdec(s->lba);
    puts(", ");
    putdec(s->size);
    puts(" bytes ");
    while (done < s->size) {
        u32 n = s->size - done;

        if (n > maxio) {
            n = maxio;
        }
        if (dev_read(s->lba + done / 512, (void *)(s->dest + done), n)) {
            puts("\nboot: read error at sector ");
            putdec(s->lba + done / 512);
            putc('\n');
            return -1;
        }
        done += n;
        if (done / 0x40000 != dots) {
            dots = done / 0x40000;
            putc('.');
        }
    }
    putc('\n');
    return 0;
}

void boot_main(void)
{
    struct bootparam *bp = *romvec->bootParam;
    struct devinfo *di;
    u8 saved_dvma;
    u32 i;

    set_fc(3);
    puts("\nLinux/sun3 boot: ");
    putc(bp->devName[0]);
    putc(bp->devName[1]);
    putc('(');
    putdec(bp->ctlrNum);
    putc(',');
    putdec(bp->unitNum);
    putc(',');
    putdec(bp->partNum);
    puts("), memory ");
    putdec(*romvec->memorySize / (1024 * 1024));
    puts(" MB\n");

    saved_dvma = get_segmap(DVMA_SEG_VA);
    if (dev_open(bp)) {
        puts("boot: open failed\n");
        return;
    }
    di = bp->bootDevice->b_devinfo;

    if (dev_read(HDR_BLOCK, &hdr, 512)) {
        puts("boot: cannot read the image header\n");
        return;
    }
    for (i = 0; i < 8; i++) {
        if (hdr.magic[i] != HDR_MAGIC[i]) {
            puts("boot: no Linux image on this partition\n");
            return;
        }
    }
    if (hdr.nseg > HDR_MAX_SEGS) {
        puts("boot: bad header\n");
        return;
    }
    for (i = 0; i < hdr.nseg; i++) {
        const struct bootimg_seg *s = &hdr.seg[i];

        if (s->dest + s->size + 511 > RELOC || s->dest + s->size < s->dest) {
            puts("boot: segment reaches the boot program at 0x780000\n");
            return;
        }
        if (load_seg(s, di->d_maxiobytes)) {
            return;
        }
    }
    si.si_boottab->b_close(&si);
    set_segmap(DVMA_SEG_VA, saved_dvma);

    puts("Linux/sun3 boot: entry ");
    puthex(hdr.entry);
    putc('\n');
    __asm__ volatile("jmp (%0)" : : "a"(hdr.entry));
    __builtin_unreachable();
}
