/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2004 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"

/*
	MiNT audio driver
	using XBIOS functions (Falcon)

	Patrice Mandin, Didier M�quignon
*/

#include <unistd.h>
#include <support.h>

/* Mint includes */
#include <mint/osbind.h>
#include <mint/falcon.h>
#include <mint/cookie.h>

#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "../SDL_sysaudio.h"

#include "../../video/ataricommon/SDL_atarimxalloc_c.h"

#include "SDL_mintaudio.h"
#include "SDL_mintaudio_dma8.h"

/*--- Defines ---*/

#define MINT_AUDIO_DRIVER_NAME "mint_xbios"

/* Debug print info */
#define DEBUG_NAME "audio:xbios: "
#if 0
#define DEBUG_PRINT(what) \
	{ \
		printf what; \
	}
#else
#define DEBUG_PRINT(what)
#endif

static unsigned long cookie_snd = 0;

static void
MINTXBIOS_LockDevice(_THIS)
{
    /* Stop replay */
    Buffoper(0);
}

static void
MINTXBIOS_UnlockDevice(_THIS)
{
    /* Restart replay */
    Buffoper(SB_PLA_ENA | SB_PLA_RPT);
}

static void
MINTXBIOS_CloseDevice(_THIS)
{
    if (this->hidden != NULL) {
        /* Stop replay */
        SDL_MintAudio_WaitThread();
        Buffoper(0);

        if (!SDL_MintAudio_mint_present) {
            /* Uninstall interrupt */
            Jdisint(MFP_DMASOUND);
        }

        /* Wait if currently playing sound */
        while (SDL_MintAudio_mutex != 0) {}

        /* Clear buffers */
        if (SDL_MintAudio_audiobuf[0]) {
            Mfree(SDL_MintAudio_audiobuf[0]);
            SDL_MintAudio_audiobuf[0] = SDL_MintAudio_audiobuf[1] = NULL;
        }

        /* Unlock sound system */
        Unlocksnd();

        SDL_free(this->hidden);
        this->hidden = NULL;
    }
}

/* Falcon XBIOS implementation of Devconnect() is buggy with external clock */
static void
Devconnect2(int src, int dst, int sclk, int pre)
{
    static const unsigned short MASK1[3] = { 0, 0x6000, 0 };
    static const unsigned short MASK2[4] = { 0xFFF0, 0xFF8F, 0xF0FF, 0x0FFF };
    static const unsigned short INDEX1[4] = { 1, 3, 5, 7 };
    static const unsigned short INDEX2[4] = { 0, 2, 4, 6 };
    unsigned short sync_div, dev_ctrl, dest_ctrl;
    void *oldstack;

    if (dst == 0) {
        return;
    }

    oldstack = (void *) Super(0);

    dev_ctrl = DMAAUDIO_IO.dev_ctrl;
    dest_ctrl = DMAAUDIO_IO.dest_ctrl;
    dev_ctrl &= MASK2[src];

    if (src == ADC) {
        dev_ctrl |= MASK1[sclk];
    } else {
        dev_ctrl |= (INDEX1[sclk] << (src << 4));
    }

    if (dst & DMAREC) {
        dest_ctrl &= 0xFFF0;
        dest_ctrl |= INDEX1[src];
    }

    if (dst & DSPRECV) {
        dest_ctrl &= 0xFF8F;
        dest_ctrl |= (INDEX1[src] << 4);
    }

    if (dst & EXTOUT) {
        dest_ctrl &= 0xF0FF;
        dest_ctrl |= (INDEX1[src] << 8);
    }

    if (dst & DAC) {
        dev_ctrl &= 0x0FFF;
        dev_ctrl |= MASK1[sclk];
        dest_ctrl &= 0x0FFF;
        dest_ctrl |= (INDEX2[src] << 12);
    }

    sync_div = DMAAUDIO_IO.sync_div;
    if (sclk == CLKEXT) {
        pre <<= 8;
        sync_div &= 0xF0FF;
    } else {
        sync_div &= 0xFFF0;
    }
    sync_div |= pre;

    DMAAUDIO_IO.dev_ctrl = dev_ctrl;
    DMAAUDIO_IO.dest_ctrl = dest_ctrl;
    DMAAUDIO_IO.sync_div = sync_div;

    Super(oldstack);
}

static void
MINTXBIOS_CheckExternalClock(_THIS)
{
#define SIZE_BUF_CLOCK_MEASURE (44100/10)

    unsigned long cookie_snd;
    char *buffer;
    int i, j;

    /* DSP present with its GPIO port ? */
    if (Getcookie(C__SND, &cookie_snd) == C_NOTFOUND) {
        return;
    }
    if ((cookie_snd & SND_DSP) == 0) {
        return;
    }

    buffer = Atari_SysMalloc(SIZE_BUF_CLOCK_MEASURE, MX_STRAM);
    if (buffer == NULL) {
        DEBUG_PRINT((DEBUG_NAME "Not enough memory for the measure\n"));
        return;
    }
    SDL_memset(buffer, 0, SIZE_BUF_CLOCK_MEASURE);

    Buffoper(0);
    Settracks(0, 0);
    Setmontracks(0);
    Setmode(MONO8);
    Jdisint(MFP_TIMERA);

    for (i = 0; i < 2; i++) {
        Gpio(GPIO_SET, 7);      /* DSP port gpio outputs */
        Gpio(GPIO_WRITE, 2 + i);        /* 22.5792/24.576 MHz for 44.1/48KHz */
        Devconnect2(DMAPLAY, DAC, CLKEXT, CLK50K);      /* Matrix and clock source */
        Setbuffer(0, buffer, buffer + SIZE_BUF_CLOCK_MEASURE);  /* Set buffer */
        Xbtimer(XB_TIMERA, 5, 38, SDL_MintAudio_XbiosInterruptMeasureClock);    /* delay mode timer A, prediv /64, 1KHz */
        Jenabint(MFP_TIMERA);
        SDL_MintAudio_clocktics = 0;
        Buffoper(SB_PLA_ENA);
        usleep(110000);

        if ((Buffoper(-1) & 1) == 0) {
            if (SDL_MintAudio_clocktics) {
                unsigned long khz;

                khz =
                    ((SIZE_BUF_CLOCK_MEASURE /
                      SDL_MintAudio_clocktics) + 1) & 0xFFFFFFFE;
                DEBUG_PRINT((DEBUG_NAME "measure %d: freq=%lu KHz\n",
                             i + 1, khz));

                if (khz == 44) {
                    for (j = 1; j < 4; j++) {
                        SDL_MintAudio_AddFrequency(this,
                                                   MASTERCLOCK_44K
                                                   /
                                                   (MASTERPREDIV_FALCON
                                                    * (1 << j)),
                                                   MASTERCLOCK_44K,
                                                   (1 << j) - 1, 2 + i);
                    }
                } else if (khz == 48) {
                    for (j = 1; j < 4; j++) {
                        SDL_MintAudio_AddFrequency(this,
                                                   MASTERCLOCK_48K
                                                   /
                                                   (MASTERPREDIV_FALCON
                                                    * (1 << j)),
                                                   MASTERCLOCK_48K,
                                                   (1 << j) - 1, 2 + i);
                    }
                }
            } else {
                DEBUG_PRINT((DEBUG_NAME "No measure\n"));
            }
        } else {
            DEBUG_PRINT((DEBUG_NAME "No SDMA clock\n"));
        }

        Buffoper(0);            /* stop */
        Jdisint(MFP_TIMERA);    /* Uninstall interrupt */
    }

    Mfree(buffer);
}

static int
MINTXBIOS_CheckAudio(_THIS)
{
    int i;
    Uint32 extclock;

    DEBUG_PRINT((DEBUG_NAME "asked: %d bits, ",
                 SDL_AUDIO_BITSIZE(this->spec.format)));
    DEBUG_PRINT(("float=%d, ", SDL_AUDIO_ISFLOAT(this->spec.format)));
    DEBUG_PRINT(("signed=%d, ", SDL_AUDIO_ISSIGNED(this->spec.format)));
    DEBUG_PRINT(("big endian=%d, ", SDL_AUDIO_ISBIGENDIAN(this->spec.format)));
    DEBUG_PRINT(("channels=%d, ", this->spec.channels));
    DEBUG_PRINT(("freq=%d\n", this->spec.freq));

    this->spec.format |= SDL_AUDIO_MASK_SIGNED;   /* Audio is always signed */

    /* clamp out int32/float32 */
    if (SDL_AUDIO_BITSIZE(this->spec.format) >= 16) {
        this->spec.format = AUDIO_S16MSB;    /* Audio is always big endian */
        this->spec.channels = 2;     /* 16 bits always stereo */
    } else if (this->spec.channels > 2) {
        this->spec.channels = 2;     /* no more than stereo! */
    }

    MINTAUDIO_freqcount = 0;

    /* Add external clocks if present */
    MINTXBIOS_CheckExternalClock(this);

    /* Standard clocks */
    for (i = 1; i < 12; i++) {
        /* Remove unusable Falcon codec predivisors */
        if ((i == 6) || (i == 8) || (i == 10)) {
            continue;
        }
        SDL_MintAudio_AddFrequency(this,
                                   MASTERCLOCK_FALCON1 /
                                   (MASTERPREDIV_FALCON * (i + 1)),
                                   MASTERCLOCK_FALCON1, i, -1);
    }

#if 1
    for (i = 0; i < MINTAUDIO_freqcount; i++) {
        DEBUG_PRINT((DEBUG_NAME "freq %d: %lu Hz, clock %lu, prediv %d\n",
                     i, MINTAUDIO_frequencies[i].frequency,
                     MINTAUDIO_frequencies[i].masterclock,
                     MINTAUDIO_frequencies[i].predivisor));
    }
#endif

    MINTAUDIO_numfreq = SDL_MintAudio_SearchFrequency(this, this->spec.freq);
    this->spec.freq = MINTAUDIO_frequencies[MINTAUDIO_numfreq].frequency;

    DEBUG_PRINT((DEBUG_NAME "obtained: %d bits, ",
                 SDL_AUDIO_BITSIZE(this->spec.format)));
    DEBUG_PRINT(("float=%d, ", SDL_AUDIO_ISFLOAT(this->spec.format)));
    DEBUG_PRINT(("signed=%d, ", SDL_AUDIO_ISSIGNED(this->spec.format)));
    DEBUG_PRINT(("big endian=%d, ", SDL_AUDIO_ISBIGENDIAN(this->spec.format)));
    DEBUG_PRINT(("channels=%d, ", this->spec.channels));
    DEBUG_PRINT(("freq=%d\n", this->spec.freq));

    return 0;
}

static void
MINTXBIOS_InitAudio(_THIS)
{
    int channels_mode, dmaclock, prediv;
    void *buffer;

    /* Stop currently playing sound */
    SDL_MintAudio_quit_thread = SDL_FALSE;
    SDL_MintAudio_thread_finished = SDL_TRUE;
    SDL_MintAudio_WaitThread();
    Buffoper(0);

    /* Set replay tracks */
    Settracks(0, 0);
    Setmontracks(0);

    /* Select replay format */
    channels_mode = STEREO16;
    switch (SDL_AUDIO_BITSIZE(this->spec.format)) {
    case 8:
        if (this->spec.channels == 2) {
            channels_mode = STEREO8;
        } else {
            channels_mode = MONO8;
        }
        break;
    }
    if (Setmode(channels_mode) < 0) {
        DEBUG_PRINT((DEBUG_NAME "Setmode() failed\n"));
    }

    dmaclock = MINTAUDIO_frequencies[MINTAUDIO_numfreq].masterclock;
    prediv = MINTAUDIO_frequencies[MINTAUDIO_numfreq].predivisor;
    if (MINTAUDIO_frequencies[MINTAUDIO_numfreq].gpio_bits != -1) {
        Gpio(GPIO_SET, 7);      /* DSP port gpio outputs */
        Gpio(GPIO_WRITE, MINTAUDIO_frequencies[MINTAUDIO_numfreq].gpio_bits);
        Devconnect2(DMAPLAY, DAC | EXTOUT, CLKEXT, prediv);
    } else {
        Devconnect2(DMAPLAY, DAC, CLK25M, prediv);
    }

    /* Set buffer */
    buffer = SDL_MintAudio_audiobuf[SDL_MintAudio_numbuf];
    if (Setbuffer(0, buffer, buffer + this->spec.size) < 0) {
        DEBUG_PRINT((DEBUG_NAME "Setbuffer() failed\n"));
    }

    if (SDL_MintAudio_mint_present) {
        SDL_MintAudio_thread_pid = tfork(SDL_MintAudio_Thread, 0);
    } else {
        /* Install interrupt */
        Jdisint(MFP_DMASOUND);
        /*Xbtimer(XB_TIMERA, 8, 1, SDL_MintAudio_XbiosInterrupt); */
        Xbtimer(XB_TIMERA, 8, 1, SDL_MintAudio_Dma8Interrupt);
        Jenabint(MFP_DMASOUND);

        if (Setinterrupt(SI_TIMERA, SI_PLAY) < 0) {
            DEBUG_PRINT((DEBUG_NAME "Setinterrupt() failed\n"));
        }
    }

    /* Go */
    Buffoper(SB_PLA_ENA | SB_PLA_RPT);
    DEBUG_PRINT((DEBUG_NAME "hardware initialized\n"));
}

static int
MINTXBIOS_OpenDevice(_THIS, const char *devname, int iscapture)
{
    /* Lock sound system */
    if (Locksnd() != 1) {
        SDL_SetError("MINTXBIOS_OpenAudio: Audio system already in use");
        return 0;
    }

    SDL_MintAudio_device = this;

    /* Check audio capabilities */
    if (MINTXBIOS_CheckAudio(this) == -1) {
        return 0;
    }

    /* Initialize all variables that we clean on shutdown */
    this->hidden = (struct SDL_PrivateAudioData *)
                        SDL_malloc((sizeof *this->hidden));
    if (this->hidden == NULL) {
        SDL_OutOfMemory();
        return 0;
    }
    SDL_memset(this->hidden, 0, (sizeof *this->hidden));

    SDL_CalculateAudioSpec(&this->spec);

    /* Allocate memory for audio buffers in DMA-able RAM */
    DEBUG_PRINT((DEBUG_NAME "buffer size=%d\n", this->spec.size));

    SDL_MintAudio_audiobuf[0] = Atari_SysMalloc(this->spec.size * 2, MX_STRAM);
    if (SDL_MintAudio_audiobuf[0] == NULL) {
        SDL_free(this->hidden);
        this->hidden = NULL;
        SDL_OutOfMemory();
        return 0;
    }
    SDL_MintAudio_audiobuf[1] = SDL_MintAudio_audiobuf[0] + this->spec.size;
    SDL_MintAudio_numbuf = 0;
    SDL_memset(SDL_MintAudio_audiobuf[0],this->spec.silence,this->spec.size*2);
    SDL_MintAudio_audiosize = this->spec.size;
    SDL_MintAudio_mutex = 0;

    DEBUG_PRINT((DEBUG_NAME "buffer 0 at 0x%08x\n",
                 SDL_MintAudio_audiobuf[0]));
    DEBUG_PRINT((DEBUG_NAME "buffer 1 at 0x%08x\n",
                 SDL_MintAudio_audiobuf[1]));

    SDL_MintAudio_CheckFpu();

    /* Setup audio hardware */
    MINTXBIOS_InitAudio(this);

    return 1;  /* good to go. */
}

static int
MINTXBIOS_Init(SDL_AudioDriverImpl *impl)
{
    unsigned long dummy = 0;
    /*SDL_MintAudio_mint_present = (Getcookie(C_MiNT, &dummy) == C_FOUND); */
    SDL_MintAudio_mint_present = SDL_FALSE;

    /* We can't use XBIOS in interrupt with Magic, don't know about thread */
    if (Getcookie(C_MagX, &dummy) == C_FOUND) {
        return (0);
    }

    /* Cookie _SND present ? if not, assume ST machine */
    if (Getcookie(C__SND, &cookie_snd) == C_NOTFOUND) {
        cookie_snd = SND_PSG;
    }

    /* Check if we have 16 bits audio */
    if ((cookie_snd & SND_16BIT) == 0) {
        SDL_SetError(DEBUG_NAME "no 16-bit sound");
        return (0);
    }

    /* Check if audio is lockable */
    if (Locksnd() != 1) {
        SDL_SetError(DEBUG_NAME "audio locked by other application");
        return (0);
    }

    Unlocksnd();

    DEBUG_PRINT((DEBUG_NAME "XBIOS audio available!\n"));

    /* Set the function pointers */
    impl->OpenDevice = MINTXBIOS_OpenDevice;
    impl->CloseDevice = MINTXBIOS_CloseDevice;
    impl->LockAudio = MINTXBIOS_LockAudio;
    impl->UnlockAudio = MINTXBIOS_UnlockAudio;
    impl->OnlyHasDefaultOutputDevice = 1;
    impl->ProvidesOwnCallbackThread = 1;
    impl->SkipMixerLock = 1;

    return 1;
}

AudioBootStrap MINTAUDIO_XBIOS_bootstrap = {
    MINT_AUDIO_DRIVER_NAME, "MiNT XBIOS audio driver", MINTXBIOS_Init, 0
};

/* vi: set ts=4 sw=4 expandtab: */
