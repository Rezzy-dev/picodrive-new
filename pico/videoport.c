/*
 * PicoDrive
 * (c) Copyright Dave, 2004
 * (C) notaz, 2006-2009
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include "pico_int.h"
#define NEED_DMA_SOURCE
#include "memory.h"

int line_base_cycles;
extern const unsigned char  hcounts_32[];
extern const unsigned char  hcounts_40[];

#ifndef UTYPES_DEFINED
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
#define UTYPES_DEFINED
#endif

int (*PicoDmaHook)(unsigned int source, int len, unsigned short **base, unsigned int *mask) = NULL;

static __inline void AutoIncrement(void)
{
  Pico.video.addr=(unsigned short)(Pico.video.addr+Pico.video.reg[0xf]);
}

static NOINLINE void VideoWrite128(u32 a, u16 d)
{
  // nasty
  a = ((a & 2) >> 1) | ((a & 0x400) >> 9) | (a & 0x3FC) | ((a & 0x1F800) >> 1);
  ((u8 *)Pico.vram)[a] = d;
}

static void VideoWrite(u16 d)
{
  unsigned int a=Pico.video.addr;

  switch (Pico.video.type)
  {
    case 1: if(a&1) d=(u16)((d<<8)|(d>>8)); // If address is odd, bytes are swapped (which game needs this?)
            Pico.vram [(a>>1)&0x7fff]=d;
            if (a - ((unsigned)(Pico.video.reg[5]&0x7f) << 9) < 0x400)
              Pico.est.rendstatus |= PDRAW_DIRTY_SPRITES;
            break;
    case 3: Pico.m.dirtyPal = 1;
            Pico.cram [(a>>1)&0x003f]=d; break; // wraps (Desert Strike)
    case 5: Pico.vsram[(a>>1)&0x003f]=d; break;
    case 0x81:
      a |= Pico.video.addr_u << 16;
      VideoWrite128(a, d);
      break;
    //default:elprintf(EL_ANOMALY, "VDP write %04x with bad type %i", d, Pico.video.type); break;
  }

  AutoIncrement();
}

static unsigned int VideoRead(void)
{
  unsigned int a=0,d=0;

  a=Pico.video.addr; a>>=1;

  switch (Pico.video.type)
  {
    case 0: d=Pico.vram [a&0x7fff]; break;
    case 8: d=Pico.cram [a&0x003f]; break;
    case 4: d=Pico.vsram[a&0x003f]; break;
    default:elprintf(EL_ANOMALY, "VDP read with bad type %i", Pico.video.type); break;
  }

  AutoIncrement();
  return d;
}

static int GetDmaLength(void)
{
  struct PicoVideo *pvid=&Pico.video;
  int len=0;
  // 16-bit words to transfer:
  len =pvid->reg[0x13];
  len|=pvid->reg[0x14]<<8;
  len = ((len - 1) & 0xffff) + 1;
  return len;
}

static void DmaSlow(int len, unsigned int source)
{
  u32 inc = Pico.video.reg[0xf];
  u32 a = Pico.video.addr;
  u16 *r, *base = NULL;
  u32 mask = 0x1ffff;

  elprintf(EL_VDPDMA, "DmaSlow[%i] %06x->%04x len %i inc=%i blank %i [%u] @ %06x",
    Pico.video.type, source, a, len, inc, (Pico.video.status&8)||!(Pico.video.reg[1]&0x40),
    SekCyclesDone(), SekPc);

  Pico.m.dma_xfers += len;
  if (Pico.m.dma_xfers < len) // lame 16bit var
    Pico.m.dma_xfers = ~0;
  SekCyclesBurnRun(CheckDMA());

  if ((source & 0xe00000) == 0xe00000) { // Ram
    base = (u16 *)Pico.ram;
    mask = 0xffff;
  }
  else if (PicoAHW & PAHW_MCD)
  {
    u8 r3 = Pico_mcd->s68k_regs[3];
    elprintf(EL_VDPDMA, "DmaSlow CD, r3=%02x", r3);
    if (source < 0x20000) { // Bios area
      base = (u16 *)Pico_mcd->bios;
    } else if ((source & 0xfc0000) == 0x200000) { // Word Ram
      if (!(r3 & 4)) { // 2M mode
        base = (u16 *)(Pico_mcd->word_ram2M + (source & 0x20000));
      } else {
        if (source < 0x220000) { // 1M mode
          int bank = r3 & 1;
          base = (u16 *)(Pico_mcd->word_ram1M[bank]);
        } else {
          DmaSlowCell(source - 2, a, len, inc);
          return;
        }
      }
      source -= 2;
    } else if ((source & 0xfe0000) == 0x020000) { // Prg Ram
      base = (u16 *)Pico_mcd->prg_ram_b[r3 >> 6];
      source -= 2; // XXX: test
    }
  }
  else
  {
    // if we have DmaHook, let it handle ROM because of possible DMA delay
    u32 source2;
    if (PicoDmaHook && (source2 = PicoDmaHook(source, len, &base, &mask)))
      source = source2;
    else // Rom
      base = m68k_dma_source(source);
  }
  if (!base) {
    elprintf(EL_VDPDMA|EL_ANOMALY, "DmaSlow[%i] %06x->%04x: invalid src", Pico.video.type, source, a);
    return;
  }

  // operate in words
  source >>= 1;
  mask >>= 1;

  switch (Pico.video.type)
  {
    case 1: // vram
      r = Pico.vram;
      if (inc == 2 && !(a & 1) && a + len * 2 < 0x10000
          && !(((source + len - 1) ^ source) & ~mask))
      {
        // most used DMA mode
        memcpy((char *)r + a, base + (source & mask), len * 2);
        a += len * 2;
      }
      else
      {
        for(; len; len--)
        {
          u16 d = base[source++ & mask];
          if(a & 1) d=(d<<8)|(d>>8);
          r[a >> 1] = d;
          // AutoIncrement
          a = (u16)(a + inc);
        }
      }
      Pico.est.rendstatus |= PDRAW_DIRTY_SPRITES;
      break;

    case 3: // cram
      Pico.m.dirtyPal = 1;
      r = Pico.cram;
      for (; len; len--)
      {
        r[(a / 2) & 0x3f] = base[source++ & mask];
        // AutoIncrement
        a += inc;
      }
      break;

    case 5: // vsram
      r = Pico.vsram;
      for (; len; len--)
      {
        r[(a / 2) & 0x3f] = base[source++ & mask];
        // AutoIncrement
        a += inc;
      }
      break;

    case 0x81: // vram 128k
      a |= Pico.video.addr_u << 16;
      for(; len; len--)
      {
        VideoWrite128(a, base[source++ & mask]);
        // AutoIncrement
        a = (a + inc) & 0x1ffff;
      }
      Pico.video.addr_u = a >> 16;
      break;

    default:
      if (Pico.video.type != 0 || (EL_LOGMASK & EL_VDPDMA))
        elprintf(EL_VDPDMA|EL_ANOMALY, "DMA with bad type %i", Pico.video.type);
      break;
  }
  // remember addr
  Pico.video.addr=(u16)a;
}

static void DmaCopy(int len)
{
  u16 a=Pico.video.addr;
  unsigned char *vr = (unsigned char *) Pico.vram;
  unsigned char inc=Pico.video.reg[0xf];
  int source;
  elprintf(EL_VDPDMA, "DmaCopy len %i [%i]", len, SekCyclesDone());

  Pico.m.dma_xfers += len;
  if (Pico.m.dma_xfers < len)
    Pico.m.dma_xfers = ~0;
  Pico.video.status |= 2; // dma busy

  source =Pico.video.reg[0x15];
  source|=Pico.video.reg[0x16]<<8;

  for (; len; len--)
  {
    vr[a] = vr[source++ & 0xffff];
    // AutoIncrement
    a=(u16)(a+inc);
  }
  // remember addr
  Pico.video.addr=a;
  Pico.est.rendstatus |= PDRAW_DIRTY_SPRITES;
}

static NOINLINE void DmaFill(int data)
{
  unsigned short a=Pico.video.addr;
  unsigned char *vr=(unsigned char *) Pico.vram;
  unsigned char high = (unsigned char) (data >> 8);
  unsigned char inc=Pico.video.reg[0xf];
  int source;
  int len, l;

  len = GetDmaLength();
  elprintf(EL_VDPDMA, "DmaFill len %i inc %i [%i]", len, inc, SekCyclesDone());

  Pico.m.dma_xfers += len;
  if (Pico.m.dma_xfers < len) // lame 16bit var
    Pico.m.dma_xfers = ~0;
  Pico.video.status |= 2; // dma busy

  switch (Pico.video.type)
  {
    case 1: // vram
      for (l = len; l; l--) {
        // Write upper byte to adjacent address
        // (here we are byteswapped, so address is already 'adjacent')
        vr[a] = high;

        // Increment address register
        a = (u16)(a + inc);
      }
      break;
    case 3:   // cram
    case 5: { // vsram
      // TODO: needs fifo; anyone using these?
      static int once;
      if (!once++)
        elprintf(EL_STATUS|EL_ANOMALY|EL_VDPDMA, "TODO: cram/vsram fill");
    }
    default:
      a += len * inc;
      break;
  }

  // remember addr
  Pico.video.addr = a;
  // register update
  Pico.video.reg[0x13] = Pico.video.reg[0x14] = 0;
  source  = Pico.video.reg[0x15];
  source |= Pico.video.reg[0x16] << 8;
  source += len;
  Pico.video.reg[0x15] = source;
  Pico.video.reg[0x16] = source >> 8;

  Pico.est.rendstatus |= PDRAW_DIRTY_SPRITES;
}

static NOINLINE void CommandDma(void)
{
  struct PicoVideo *pvid=&Pico.video;
  u32 len, method;
  u32 source;

  if ((pvid->reg[1]&0x10)==0) return; // DMA not enabled

  len = GetDmaLength();
  source =Pico.video.reg[0x15];
  source|=Pico.video.reg[0x16] << 8;
  source|=Pico.video.reg[0x17] << 16;

  method=pvid->reg[0x17]>>6;
  if (method < 2)
    DmaSlow(len, source << 1); // 68000 to VDP
  else if (method == 3)
    DmaCopy(len); // VRAM Copy
  else
    return;

  source += len;
  Pico.video.reg[0x13] = Pico.video.reg[0x14] = 0;
  Pico.video.reg[0x15] = source;
  Pico.video.reg[0x16] = source >> 8;
}

static NOINLINE void CommandChange(void)
{
  struct PicoVideo *pvid = &Pico.video;
  unsigned int cmd, addr;

  cmd = pvid->command;

  // Get type of transfer 0xc0000030 (v/c/vsram read/write)
  pvid->type = (u8)(((cmd >> 2) & 0xc) | (cmd >> 30));
  if (pvid->type == 1) // vram
    pvid->type |= pvid->reg[1] & 0x80; // 128k

  // Get address 0x3fff0003
  addr  = (cmd >> 16) & 0x3fff;
  addr |= (cmd << 14) & 0xc000;
  pvid->addr = (u16)addr;
  pvid->addr_u = (u8)((cmd >> 2) & 1);
}

static void DrawSync(int blank_on)
{
  if (Pico.m.scanline < 224 && !(PicoOpt & POPT_ALT_RENDERER) &&
      !PicoSkipFrame && Pico.est.DrawScanline <= Pico.m.scanline) {
    //elprintf(EL_ANOMALY, "sync");
    PicoDrawSync(Pico.m.scanline, blank_on);
  }
}

PICO_INTERNAL_ASM void PicoVideoWrite(unsigned int a,unsigned short d)
{
  struct PicoVideo *pvid=&Pico.video;

  //if (Pico.m.scanline < 224)
  //  elprintf(EL_STATUS, "PicoVideoWrite [%06x] %04x", a, d);
  a&=0x1c;

  if (a==0x00) // Data port 0 or 2
  {
    // try avoiding the sync..
    if (Pico.m.scanline < 224 && (pvid->reg[1]&0x40) &&
        !(!pvid->pending &&
          ((pvid->command & 0xc00000f0) == 0x40000010 && Pico.vsram[pvid->addr>>1] == d))
       )
      DrawSync(0);

    if (pvid->pending) {
      CommandChange();
      pvid->pending=0;
    }

    // preliminary FIFO emulation for Chaos Engine, The (E)
    if (!(pvid->status&8) && (pvid->reg[1]&0x40) && !(PicoOpt&POPT_DIS_VDP_FIFO)) // active display?
    {
      pvid->status&=~0x200; // FIFO no longer empty
      pvid->lwrite_cnt++;
      if (pvid->lwrite_cnt >= 4) pvid->status|=0x100; // FIFO full
      if (pvid->lwrite_cnt >  4) {
        SekCyclesBurnRun(32); // penalty // 488/12-8
      }
      elprintf(EL_ASVDP, "VDP data write: %04x [%06x] {%i} #%i @ %06x", d, Pico.video.addr,
               Pico.video.type, pvid->lwrite_cnt, SekPc);
    }
    VideoWrite(d);

    if ((pvid->command&0x80) && (pvid->reg[1]&0x10) && (pvid->reg[0x17]>>6)==2)
      DmaFill(d);

    return;
  }

  if (a==0x04) // Control (command) port 4 or 6
  {
    if (pvid->pending)
    {
      // Low word of command:
      pvid->command &= 0xffff0000;
      pvid->command |= d;
      pvid->pending = 0;
      CommandChange();
      // Check for dma:
      if (d & 0x80) {
        DrawSync(0);
        CommandDma();
      }
    }
    else
    {
      if ((d&0xc000)==0x8000)
      {
        // Register write:
        int num=(d>>8)&0x1f;
        int dold=pvid->reg[num];
        int blank_on = 0;
        pvid->type=0; // register writes clear command (else no Sega logo in Golden Axe II)
        if (num > 0x0a && !(pvid->reg[1]&4)) {
          elprintf(EL_ANOMALY, "%02x written to reg %02x in SMS mode @ %06x", d, num, SekPc);
          return;
        }

        if (num == 1 && !(d&0x40) && SekCyclesDone() - line_base_cycles <= 488-390)
          blank_on = 1;
        DrawSync(blank_on);
        pvid->reg[num]=(unsigned char)d;
        switch (num)
        {
          case 0x00:
            elprintf(EL_INTSW, "hint_onoff: %i->%i [%i] pend=%i @ %06x", (dold&0x10)>>4,
                    (d&0x10)>>4, SekCyclesDone(), (pvid->pending_ints&0x10)>>4, SekPc);
            goto update_irq;
          case 0x01:
            elprintf(EL_INTSW, "vint_onoff: %i->%i [%i] pend=%i @ %06x", (dold&0x20)>>5,
                    (d&0x20)>>5, SekCyclesDone(), (pvid->pending_ints&0x20)>>5, SekPc);
            goto update_irq;
          case 0x05:
            //elprintf(EL_STATUS, "spritep moved to %04x", (unsigned)(Pico.video.reg[5]&0x7f) << 9);
            if (d^dold) Pico.est.rendstatus |= PDRAW_SPRITES_MOVED;
            break;
          case 0x0c:
            // renderers should update their palettes if sh/hi mode is changed
            if ((d^dold)&8) Pico.m.dirtyPal = 2;
            break;
        }
        return;

update_irq:
#ifndef EMU_CORE_DEBUG
        // update IRQ level
        if (!SekShouldInterrupt()) // hack
        {
          int lines, pints, irq=0;
          lines = (pvid->reg[1] & 0x20) | (pvid->reg[0] & 0x10);
          pints = (pvid->pending_ints&lines);
               if (pints & 0x20) irq = 6;
          else if (pints & 0x10) irq = 4;
          SekInterrupt(irq); // update line

          if (irq) SekEndRun(24); // make it delayed
        }
#endif
      }
      else
      {
        // High word of command:
        pvid->command&=0x0000ffff;
        pvid->command|=d<<16;
        pvid->pending=1;
      }
    }
  }
}

PICO_INTERNAL_ASM unsigned int PicoVideoRead(unsigned int a)
{
  a&=0x1c;

  if (a==0x04) // control port
  {
    struct PicoVideo *pv=&Pico.video;
    unsigned int d;
    d=pv->status;
    //if (PicoOpt&POPT_ALT_RENDERER) d|=0x0020; // sprite collision (Shadow of the Beast)
    if (SekCyclesDone() - line_base_cycles >= 488-88)
      d|=0x0004; // H-Blank (Sonic3 vs)

    d |= ((pv->reg[1]&0x40)^0x40) >> 3;  // set V-Blank if display is disabled
    d |= (pv->pending_ints&0x20)<<2;     // V-int pending?
    if (d&0x100) pv->status&=~0x100; // FIFO no longer full

    pv->pending = 0; // ctrl port reads clear write-pending flag (Charles MacDonald)

    elprintf(EL_SR, "SR read: %04x @ %06x", d, SekPc);
    return d;
  }

  // H-counter info (based on Generator):
  // frame:
  //                       |       <- hblank? ->      |
  // start    <416>       hint  <36> hdisplay <38>  end // CPU cycles
  // |---------...---------|------------|-------------|
  // 0                   B6 E4                       FF // 40 cells
  // 0                   93 E8                       FF // 32 cells

  // Gens (?)              v-render
  // start  <hblank=84>   hint    hdisplay <404>      |
  // |---------------------|--------------------------|
  // E4  (hc[0x43]==0)    07                         B1 // 40
  // E8  (hc[0x45]==0)    05                         91 // 32

  // check: Sonic 3D Blast bonus, Cannon Fodder, Chase HQ II, 3 Ninjas kick back, Road Rash 3, Skitchin', Wheel of Fortune
  if ((a&0x1c)==0x08)
  {
    unsigned int d;

    d = (SekCyclesDone() - line_base_cycles) & 0x1ff; // FIXME
    if (Pico.video.reg[12]&1)
         d = hcounts_40[d];
    else d = hcounts_32[d];

    elprintf(EL_HVCNT, "hv: %02x %02x (%i) @ %06x", d, Pico.video.v_counter, SekCyclesDone(), SekPc);
    return d | (Pico.video.v_counter << 8);
  }

  if (a==0x00) // data port
  {
    return VideoRead();
  }

  return 0;
}

unsigned int PicoVideoRead8(unsigned int a)
{
  unsigned int d;
  a&=0x1d;

  switch (a)
  {
    case 0: return VideoRead() >> 8;
    case 1: return VideoRead() & 0xff;
    case 4: // control port/status reg
      d = Pico.video.status >> 8;
      if (d&1) Pico.video.status&=~0x100; // FIFO no longer full
      Pico.video.pending = 0;
      elprintf(EL_SR, "SR read (h): %02x @ %06x", d, SekPc);
      return d;
    case 5:
      d = Pico.video.status & 0xff;
      //if (PicoOpt&POPT_ALT_RENDERER) d|=0x0020; // sprite collision (Shadow of the Beast)
      d |= ((Pico.video.reg[1]&0x40)^0x40) >> 3;  // set V-Blank if display is disabled
      d |= (Pico.video.pending_ints&0x20)<<2;     // V-int pending?
      if (SekCyclesDone() - line_base_cycles >= 488-88) d |= 4;    // H-Blank
      Pico.video.pending = 0;
      elprintf(EL_SR, "SR read (l): %02x @ %06x", d, SekPc);
      return d;
    case 8: // hv counter
      elprintf(EL_HVCNT, "vcounter: %02x (%i) @ %06x", Pico.video.v_counter, SekCyclesDone(), SekPc);
      return Pico.video.v_counter;
    case 9:
      d = (SekCyclesDone() - line_base_cycles) & 0x1ff; // FIXME
      if (Pico.video.reg[12]&1)
           d = hcounts_40[d];
      else d = hcounts_32[d];
      elprintf(EL_HVCNT, "hcounter: %02x (%i) @ %06x", d, SekCyclesDone(), SekPc);
      return d;
  }

  return 0;
}

// vim:shiftwidth=2:ts=2:expandtab
