/* Copyright (C) 2002-2003 Jean-Marc Valin 
   File: speexdec.c

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:
   
   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   
   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
   
   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.
   
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#if !defined WIN32 && !defined _WIN32
#include <unistd.h>
#include <getopt.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "speex.h"
#include "ogg/ogg.h"

#if defined WIN32 || defined _WIN32
#include <windows.h>
#include "getopt_win.h"
#include "wave_out.h"
/* We need the following two to set stdout to binary */
#include <io.h>
#include <fcntl.h>
#endif
#include <math.h>

#ifdef HAVE_SYS_SOUNDCARD_H
#include <sys/soundcard.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#endif

#include <string.h>
#include "wav_io.h"
#include "speex_header.h"
#include "speex_stereo.h"
#include "speex_callbacks.h"
#include "misc.h"

#define MAX_FRAME_SIZE 2000

#define readint(buf, base) (((buf[base+3]<<24)&0xff000000)| \
                           ((buf[base+2]<<16)&0xff0000)| \
                           ((buf[base+1]<<8)&0xff00)| \
  	           	    (buf[base]&0xff))

static void print_comments(char *comments, int length)
{
   char *c=comments;
   int len, i, nb_fields;
   char *end;
   
   if (length<8)
   {
      fprintf (stderr, "Invalid/corrupted comments\n");
      return;
   }
   end = c+length;
   len=readint(c, 0);
   c+=4;
   if (c+len>end)
   {
      fprintf (stderr, "Invalid/corrupted comments\n");
      return;
   }
   fwrite(c, 1, len, stderr);
   c+=len;
   fprintf (stderr, "\n");
   if (c+4>end)
   {
      fprintf (stderr, "Invalid/corrupted comments\n");
      return;
   }
   nb_fields=readint(c, 0);
   c+=4;
   for (i=0;i<nb_fields;i++)
   {
      if (c+4>end)
      {
         fprintf (stderr, "Invalid/corrupted comments\n");
         return;
      }
      len=readint(c, 0);
      c+=4;
      if (c+len>end)
      {
         fprintf (stderr, "Invalid/corrupted comments\n");
         return;
      }
      fwrite(c, 1, len, stderr);
      c+=len;
      fprintf (stderr, "\n");
   }
}

FILE *out_file_open(char *outFile, int rate, int *channels)
{
   FILE *fout=NULL;
   /*Open output file*/
   if (strlen(outFile)==0)
   {
#if defined HAVE_SYS_SOUNDCARD_H
      int audio_fd, format, stereo;
      audio_fd=open("/dev/dsp", O_WRONLY);
      if (audio_fd<0)
      {
         perror("Cannot open /dev/dsp");
         exit(1);         
      }

      format=AFMT_S16_NE;
      if (ioctl(audio_fd, SNDCTL_DSP_SETFMT, &format)==-1)
      {
         perror("SNDCTL_DSP_SETFMT");
         close(audio_fd);
         exit(1);
      }

      stereo=0;
      if (*channels==2)
         stereo=1;
      if (ioctl(audio_fd, SNDCTL_DSP_STEREO, &stereo)==-1)
      {
         perror("SNDCTL_DSP_STEREO");
         close(audio_fd);
         exit(1);
      }
      if (stereo!=0)
      {
         if (*channels==1)
            fprintf (stderr, "Cannot set mono mode, will decode in stereo\n");
         *channels=2;
      }

      if (ioctl(audio_fd, SNDCTL_DSP_SPEED, &rate)==-1)
      {
         perror("SNDCTL_DSP_SPEED");
         close(audio_fd);
         exit(1);
      }
      fout = fdopen(audio_fd, "w");
#elif defined WIN32 || defined _WIN32
      {
         unsigned int speex_channels = *channels;
         if (Set_WIN_Params (INVALID_FILEDESC, rate, SAMPLE_SIZE, speex_channels))
         {
            fprintf (stderr, "Can't access %s\n", "WAVE OUT");
            exit(1);
         }
      }
#else
      fprintf (stderr, "No soundcard support\n");
      exit(1);
#endif
   } else {
      if (strcmp(outFile,"-")==0)
      {
#if defined WIN32 || defined _WIN32
         _setmode(_fileno(stdout), _O_BINARY);
#endif
         fout=stdout;
      }
      else 
      {
#if defined WIN32 || defined _WIN32
         fout = fopen(outFile, "wb");
#else
         fout = fopen(outFile, "w");
#endif
         if (!fout)
         {
            perror(outFile);
            exit(1);
         }
         if (strcmp(outFile+strlen(outFile)-4,".wav")==0 || strcmp(outFile+strlen(outFile)-4,".WAV")==0)
            write_wav_header(fout, rate, *channels, 0, 0);
      }
   }
   return fout;
}

void usage()
{
   printf ("Usage: speexdec [options] input_file.spx [output_file]\n");
   printf ("\n");
   printf ("Decodes a Speex file and produce a WAV file or raw file\n");
   printf ("\n");
   printf ("input_file can be:\n");
   printf ("  filename.spx         regular Speex file\n");
   printf ("  -                    stdin\n");
   printf ("\n");  
   printf ("output_file can be:\n");
   printf ("  filename.wav         Wav file\n");
   printf ("  filename.*           Raw PCM file (any extension other that .wav)\n");
   printf ("  -                    stdout\n");
   printf ("  (nothing)            Will be played to soundcard\n");
   printf ("\n");  
   printf ("Options:\n");
   printf (" --enh                 Enable perceptual enhancement (default)\n");
   printf (" --no-enh              Disable perceptual enhancement\n");
   printf (" --force-nb            Force decoding in narrowband\n");
   printf (" --force-wb            Force decoding in wideband\n");
   printf (" --force-uwb           Force decoding in ultra-wideband\n");
   printf (" --mono                Force decoding in mono\n");
   printf (" --stereo              Force decoding in stereo\n");
   printf (" --rate n              Force decoding at sampling rate n Hz\n");
   printf (" --packet-loss n       Simulate n %% random packet loss\n");
   printf (" -V                    Verbose mode (show bit-rate)\n"); 
   printf (" -h, --help            This help\n");
   printf (" -v, --version         Version information\n");
   printf (" --pf                  Deprecated, use --enh instead\n");
   printf (" --no-pf               Deprecated, use --no-enh instead\n");
   printf ("\n");
   printf ("More information is available from the Speex site: http://www.speex.org\n");
   printf ("\n");
   printf ("Please report bugs to the mailing list `speex-dev@xiph.org'.\n");
}

void version()
{
   printf ("speexdec (Speex decoder) version " VERSION " (compiled " __DATE__ ")\n");
   printf ("Copyright (C) 2002-2003 Jean-Marc Valin\n");
}

void version_short()
{
   printf ("speexdec version " VERSION "\n");
   printf ("Copyright (C) 2002-2003 Jean-Marc Valin\n");
}

static void *process_header(ogg_packet *op, int enh_enabled, int *frame_size, int *rate, int *nframes, int forceMode, int *channels, SpeexStereoState *stereo, int *extra_headers)
{
   void *st;
   SpeexMode *mode;
   SpeexHeader *header;
   int modeID;
   SpeexCallback callback;
      
   header = speex_packet_to_header((char*)op->packet, op->bytes);
   if (!header)
   {
      fprintf (stderr, "Cannot read header\n");
      return NULL;
   }
   if (header->mode >= SPEEX_NB_MODES)
   {
      fprintf (stderr, "Mode number %d does not (yet/any longer) exist in this version\n", 
               header->mode);
      return NULL;
   }
      
   modeID = header->mode;
   if (forceMode!=-1)
      modeID = forceMode;
   mode = speex_mode_list[modeID];
   
   if (header->speex_version_id > 1)
   {
      fprintf (stderr, "This file was encoded with Speex bit-stream version %d, which I don't know how to decode\n", header->speex_version_id);
      return NULL;
   }

   if (mode->bitstream_version < header->mode_bitstream_version)
   {
      fprintf (stderr, "The file was encoded with a newer version of Speex. You need to upgrade in order to play it.\n");
      return NULL;
   }
   if (mode->bitstream_version > header->mode_bitstream_version) 
   {
      fprintf (stderr, "The file was encoded with an older version of Speex. You would need to downgrade the version in order to play it.\n");
      return NULL;
   }
   
   st = speex_decoder_init(mode);
   if (!st)
   {
      fprintf (stderr, "Decoder initialization failed.\n");
      return NULL;
   }
   speex_decoder_ctl(st, SPEEX_SET_ENH, &enh_enabled);
   speex_decoder_ctl(st, SPEEX_GET_FRAME_SIZE, frame_size);

   if (!(*channels==1))
   {
      callback.callback_id = SPEEX_INBAND_STEREO;
      callback.func = speex_std_stereo_request_handler;
      callback.data = stereo;
      speex_decoder_ctl(st, SPEEX_SET_HANDLER, &callback);
   }
   if (!*rate)
      *rate = header->rate;
   /* Adjust rate if --force-* options are used */
   if (forceMode!=-1)
   {
      if (header->mode < forceMode)
         *rate <<= (forceMode - header->mode);
      if (header->mode > forceMode)
         *rate >>= (header->mode - forceMode);
   }

   speex_decoder_ctl(st, SPEEX_SET_SAMPLING_RATE, rate);

   *nframes = header->frames_per_packet;

   if (*channels==-1)
      *channels = header->nb_channels;
   
   fprintf (stderr, "Decoding %d Hz audio using %s mode", 
            *rate, mode->modeName);

   if (*channels==1)
      fprintf (stderr, " (mono");
   else
      fprintf (stderr, " (stereo");
      
   if (header->vbr)
      fprintf (stderr, ", VBR)\n");
   else
      fprintf(stderr, ")\n");
   /*fprintf (stderr, "Decoding %d Hz audio at %d bps using %s mode\n", 
    *rate, mode->bitrate, mode->modeName);*/

   *extra_headers = header->extra_headers;

   free(header);
   return st;
}

int main(int argc, char **argv)
{
   int c;
   int option_index = 0;
   char *inFile, *outFile;
   FILE *fin, *fout=NULL;
   short out[MAX_FRAME_SIZE];
   float output[MAX_FRAME_SIZE];
   int frame_size=0;
   void *st=NULL;
   SpeexBits bits;
   int packet_count=0;
   int stream_init = 0;
   struct option long_options[] =
   {
      {"help", no_argument, NULL, 0},
      {"version", no_argument, NULL, 0},
      {"version-short", no_argument, NULL, 0},
      {"enh", no_argument, NULL, 0},
      {"no-enh", no_argument, NULL, 0},
      {"pf", no_argument, NULL, 0},
      {"no-pf", no_argument, NULL, 0},
      {"force-nb", no_argument, NULL, 0},
      {"force-wb", no_argument, NULL, 0},
      {"force-uwb", no_argument, NULL, 0},
      {"rate", required_argument, NULL, 0},
      {"mono", no_argument, NULL, 0},
      {"stereo", no_argument, NULL, 0},
      {"packet-loss", required_argument, NULL, 0},
      {0, 0, 0, 0}
   };
   ogg_sync_state oy;
   ogg_page       og;
   ogg_packet     op;
   ogg_stream_state os;
   int enh_enabled;
   int nframes=2;
   int print_bitrate=0;
   int close_in=0;
   int eos=0;
   int forceMode=-1;
   int audio_size=0;
   float loss_percent=-1;
   SpeexStereoState stereo = SPEEX_STEREO_STATE_INIT;
   int channels=-1;
   int rate=0;
   int extra_headers;
   int wav_format=0;

   enh_enabled = 1;

   /*Process options*/
   while(1)
   {
      c = getopt_long (argc, argv, "hvV",
                       long_options, &option_index);
      if (c==-1)
         break;
      
      switch(c)
      {
      case 0:
         if (strcmp(long_options[option_index].name,"help")==0)
         {
            usage();
            exit(0);
         } else if (strcmp(long_options[option_index].name,"version")==0)
         {
            version();
            exit(0);
         } else if (strcmp(long_options[option_index].name,"version-short")==0)
         {
            version_short();
            exit(0);
         } else if (strcmp(long_options[option_index].name,"enh")==0)
         {
            enh_enabled=1;
         } else if (strcmp(long_options[option_index].name,"no-enh")==0)
         {
            enh_enabled=0;
         } else if (strcmp(long_options[option_index].name,"pf")==0)
         {
            fprintf (stderr, "--pf is deprecated, use --enh instead\n");
            enh_enabled=1;
         } else if (strcmp(long_options[option_index].name,"no-pf")==0)
         {
            fprintf (stderr, "--no-pf is deprecated, use --no-enh instead\n");
            enh_enabled=0;
         } else if (strcmp(long_options[option_index].name,"force-nb")==0)
         {
            forceMode=0;
         } else if (strcmp(long_options[option_index].name,"force-wb")==0)
         {
            forceMode=1;
         } else if (strcmp(long_options[option_index].name,"force-uwb")==0)
         {
            forceMode=2;
         } else if (strcmp(long_options[option_index].name,"mono")==0)
         {
            channels=1;
         } else if (strcmp(long_options[option_index].name,"stereo")==0)
         {
            channels=2;
         } else if (strcmp(long_options[option_index].name,"rate")==0)
         {
            rate=atoi (optarg);
         } else if (strcmp(long_options[option_index].name,"packet-loss")==0)
         {
            loss_percent = atof(optarg);
         }
         break;
      case 'h':
         usage();
         exit(0);
         break;
      case 'v':
         version();
         exit(0);
         break;
      case 'V':
         print_bitrate=1;
         break;
      case '?':
         usage();
         exit(1);
         break;
      }
   }
   if (argc-optind!=2 && argc-optind!=1)
   {
      usage();
      exit(1);
   }
   inFile=argv[optind];

   if (argc-optind==2)
      outFile=argv[optind+1];
   else
      outFile = "";
   wav_format = strlen(outFile)>=4 && (
                                       strcmp(outFile+strlen(outFile)-4,".wav")==0
                                       || strcmp(inFile+strlen(inFile)-4,".WAV")==0);
   /*Open input file*/
   if (strcmp(inFile, "-")==0)
   {
#if defined WIN32 || defined _WIN32
      _setmode(_fileno(stdin), _O_BINARY);
#endif
      fin=stdin;
   }
   else 
   {
#if defined WIN32 || defined _WIN32
      fin = fopen(inFile, "rb");
#else
      fin = fopen(inFile, "r");
#endif
      if (!fin)
      {
         perror(inFile);
         exit(1);
      }
      close_in=1;
   }


   /*Init Ogg data struct*/
   ogg_sync_init(&oy);
   
   speex_bits_init(&bits);
   /*Main decoding loop*/
   while (1)
   {
      char *data;
      int i, j, nb_read;
      /*Get the ogg buffer for writing*/
      data = ogg_sync_buffer(&oy, 200);
      /*Read bitstream from input file*/
      nb_read = fread(data, sizeof(char), 200, fin);      
      ogg_sync_wrote(&oy, nb_read);

      /*Loop for all complete pages we got (most likely only one)*/
      while (ogg_sync_pageout(&oy, &og)==1)
      {
         if (stream_init == 0) {
            ogg_stream_init(&os, ogg_page_serialno(&og));
            stream_init = 1;
         }
         /*Add page to the bitstream*/
         ogg_stream_pagein(&os, &og);
         /*Extract all available packets*/
         while (!eos && ogg_stream_packetout(&os, &op)==1)
         {
            /*If first packet, process as Speex header*/
            if (packet_count==0)
            {
               st = process_header(&op, enh_enabled, &frame_size, &rate, &nframes, forceMode, &channels, &stereo, &extra_headers);
               if (!nframes)
                  nframes=1;
               if (!st)
                  exit(1);
               fout = out_file_open(outFile, rate, &channels);

            } else if (packet_count==1)
            {
               print_comments((char*)op.packet, op.bytes);
            } else if (packet_count<=1+extra_headers)
            {
               /* Ignore extra headers */
            } else {
               
               int lost=0;
               if (loss_percent>0 && 100*((float)rand())/RAND_MAX<loss_percent)
                  lost=1;

               /*End of stream condition*/
               if (op.e_o_s)
                  eos=1;

               /*Copy Ogg packet to Speex bitstream*/
               speex_bits_read_from(&bits, (char*)op.packet, op.bytes);
               for (j=0;j!=nframes;j++)
               {
                  int ret;
                  /*Decode frame*/
                  if (!lost)
                     ret = speex_decode(st, &bits, output);
                  else
                     ret = speex_decode(st, NULL, output);

                  if (ret==-1)
                     break;
                  if (ret==-2)
                  {
                     fprintf (stderr, "Decoding error: corrupted stream?\n");
                     break;
                  }
                  if (speex_bits_remaining(&bits)<0)
                  {
                     fprintf (stderr, "Decoding overflow: corrupted stream?\n");
                     break;
                  }
                  if (channels==2)
                     speex_decode_stereo(output, frame_size, &stereo);

                  if (print_bitrate) {
                     int tmp;
                     char ch=13;
                     speex_decoder_ctl(st, SPEEX_GET_BITRATE, &tmp);
                     fputc (ch, stderr);
                     fprintf (stderr, "Bitrate is use: %d bps     ", tmp);
                  }
                  /*PCM saturation (just in case)*/
                  for (i=0;i<frame_size*channels;i++)
                  {
                     if (output[i]>32000.0)
                        output[i]=32000.0;
                     else if (output[i]<-32000.0)
                        output[i]=-32000.0;
                  }
                  /*Convert to short and save to output file*/
		  if (strlen(outFile)!=0)
                  {
                     for (i=0;i<frame_size*channels;i++)
                        out[i]=(short)le_short((short)floor(.5+output[i]));
		  } else {
                     for (i=0;i<frame_size*channels;i++)
                        out[i]=(short)floor(.5+output[i]);
		  }
#if defined WIN32 || defined _WIN32
                  if (strlen(outFile)==0)
                      WIN_Play_Samples (out, sizeof(short) * frame_size*channels);
                  else
#endif
                  fwrite(out, sizeof(short), frame_size*channels, fout);
                  
                  audio_size+=sizeof(short)*frame_size*channels;
               }
            }
            packet_count++;
         }
      }
      if (feof(fin))
         break;

   }

   if (wav_format)
   {
      if (fseek(fout,4,SEEK_SET)==0)
      {
         int tmp;
         tmp = le_int(audio_size+36);
         fwrite(&tmp,4,1,fout);
         if (fseek(fout,32,SEEK_CUR)==0)
         {
            tmp = le_int(audio_size);
            fwrite(&tmp,4,1,fout);
         } else
         {
            fprintf (stderr, "First seek worked, second didn't\n");
         }
      } else {
         fprintf (stderr, "Cannot seek on wave file, size will be incorrect\n");
      }
   }

   if (st)
      speex_decoder_destroy(st);
   else 
   {
      fprintf (stderr, "This doesn't look like a Speex file\n");
   }
   speex_bits_destroy(&bits);
   if (stream_init)
      ogg_stream_clear(&os);
   ogg_sync_clear(&oy);

#if defined WIN32 || defined _WIN32
   if (strlen(outFile)==0)
      WIN_Audio_close ();
#endif

   if (close_in)
      fclose(fin);
   if (fout != NULL)
      fclose(fout);   

   return 0;
}
