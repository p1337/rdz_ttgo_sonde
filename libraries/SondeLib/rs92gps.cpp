/*  SPDX-License-Identifier:        GPL-3.0
 *  based on https://github.com/rs1729/RS/blob/master/rs92/rs92gps.c
 * 
 *  radiosonde RS92
 *
 *
 *  broadcast ephemeris:
 *  http://cddis.gsfc.nasa.gov/Data_and_Derived_Products/GNSS/broadcast_ephemeris_data.html
 *  ftp://cddis.gsfc.nasa.gov/gnss/data/daily/YYYY/DDD/YYn/brdcDDD0.YYn.Z (updated)
 *  ftp://cddis.gsfc.nasa.gov/gnss/data/daily/YYYY/brdc/brdcDDD0.YYn.Z (final)
 *
 *  SEM almanac:
 *  https://celestrak.com/GPS/almanac/SEM/
 *
 *  GPS calendar:
 *  http://adn.agi.com/GNSSWeb/
 *
 *  GPS-Hoehe ueber Ellipsoid, Geoid-Hoehe:
 *  http://geographiclib.sourceforge.net/cgi-bin/GeoidEval
 */

/*
    gcc rs92gps.c -lm -o rs92gps
    (includes nav_gps_vel.c)

    examples:

    sox -t oss /dev/dsp -t wav - lowpass 2600 2>/dev/null | ./rs92gps -e brdc3050.15n

    ./rs92gps -r 2015_11_01.wav > raw1.out
    sox -t oss /dev/dsp -t wav - lowpass 2600 2>/dev/null | stdbuf -oL ./rs92gps -r > raw2.out
    ./rs92gps --dop 5 -gg -e brdc3050.15n --rawin1 raw.out

    sox -t oss /dev/dsp -t wav - lowpass 2600 2>/dev/null | tee audio.wav | ./rs92gps -e brdc3050.15n
    ./rs92gps -g1 -e brdc3050.15n 2015_11_01-14.wav | tee out1.txt
    ./rs92gps -g2 -e brdc3050.15n 2015_11_01-14.wav | tee out2.txt
    sox 2015_11_01.wav -t wav - lowpass 2600 2>/dev/null | ./rs92gps -gg -e brdc3050.15n | tee out3.txt

    sox -t oss /dev/dsp -t wav - lowpass 2600 2>/dev/null | stdbuf -oL ./rs92gps -e brdc3050.15n > out1.txt
    sox -t oss /dev/dsp -t wav - lowpass 2600 2>/dev/null | stdbuf -oL ./rs92gps -e brdc3050.15n | tee out2.txt

*/

#if 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#endif

#include <SPIFFS.h>
#include "nav_gps_vel.h"
#include "rs92gps.h"
#include "Sonde.h"



gpx_t gpx;

int option_verbose = 0,  // ausfuehrliche Anzeige
    option_raw = 1,      // rohe Frames
    option_inv = 0,      // invertiert Signal
    option_res = 0,      // genauere Bitmessung
    option_crc = 0,      // check CRC
    option_avg = 0,      // moving average
    option_b = 0,
    fileloaded = 0,
    option_vergps = 0,
    option_iter = 0,
    option_vel = 0,      // velocity
    option_aux = 0,      // Aux/Ozon
    option_der = 0,      // linErr
    rawin = 0;
double dop_limit = 9.9;
double d_err = 10000;

int rollover = 0,
    err_gps = 0;

int almanac = 0,
    ephem = 0;

int exSat = -1;

#if 0
/* --- RS92-SGP: 8N1 manchester --- */
#define BITS (2*(1+8+1))  // 20
#define HEADOFS 40 //  HEADOFS+HEADLEN = 120  (bis 0x10)
#define HEADLEN 80 // (HEADOFS+HEADLEN) mod BITS = 0
/*
#define HEADOFS 0  // HEADOFS muss 0 wegen Wiederholung
#define HEADLEN 60 // HEADLEN < 100, (HEADOFS+HEADLEN) mod BITS = 0
*/
#define FRAMESTART ((HEADOFS+HEADLEN)/BITS)

/*               2A                  10*/
char header[] = "10100110011001101001"
                "10100110011001101001"
                "10100110011001101001"
                "10100110011001101001"
                "1010011001100110100110101010100110101001";
char buf[HEADLEN+1] = "x";
#endif

int bufpos = -1;

#define FRAME_LEN 240
uint8_t frame[FRAME_LEN] = { 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x10};
/* --- RS92-SGP ------------------- */


char buffer_rawin[3*FRAME_LEN+8]; //## rawin1: buffer_rawin[2*FRAME_LEN+4];
int frameofs = 0;

#define MASK_LEN 64
uint8_t mask[MASK_LEN] = { 0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98,
                         0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
                         0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1,
                         0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
                         0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C,
                         0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
                         0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23,
                         0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1};
/* LFSR: ab i=8 (mod 64):
 * m[16+i] = m[i] ^ m[i+2] ^ m[i+4] ^ m[i+6]
 * ________________3205590EF944C6262160C2EA795D6DA15469470CDCE85CF1
 * F776827F0799A22C937C3063F5102E61D0BCB4B606AAF423786E3BAEBF7B4CC196833E51B1490898
 */

/* ------------------------------------------------------------------------------------ */

#define BAUD_RATE 4800

int sample_rate = 0, bits_sample = 0, channels = 0;
float samples_per_bit = 0;

int findstr(char *buff, char *str, int pos) {
    int i;
    for (i = 0; i < 4; i++) {
        if (buff[(pos+i)%4] != str[i]) break;
    }
    return i;
}

#if 0
int read_wav_header(FILE *fp) {
    char txt[4+1] = "\0\0\0\0";
    unsigned char dat[4];
    int byte, p=0;

    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "RIFF", 4)) return -1;
    if (fread(txt, 1, 4, fp) < 4) return -1;
    // pos_WAVE = 8L
    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "WAVE", 4)) return -1;
    // pos_fmt = 12L
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "fmt ", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;

    if (fread(dat, 1, 2, fp) < 2) return -1;
    channels = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    memcpy(&sample_rate, dat, 4); //sample_rate = dat[0]|(dat[1]<<8)|(dat[2]<<16)|(dat[3]<<24);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;
    //byte = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 2, fp) < 2) return -1;
    bits_sample = dat[0] + (dat[1] << 8);

    // pos_dat = 36L + info
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "data", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;


    Serial.printf("sample_rate: %d\n", sample_rate);
    Serial.printf("bits       : %d\n", bits_sample);
    Serial.printf("channels   : %d\n", channels);

    if ((bits_sample != 8) && (bits_sample != 16)) return -1;

    samples_per_bit = sample_rate/(float)BAUD_RATE;

    Serial.printf("samples/bit: %.2f\n", samples_per_bit);

    return 0;
}
#endif

#if 0
#define EOF_INT  0x1000000

#define LEN_movAvg 3
int movAvg[LEN_movAvg];
unsigned long sample_count = 0;
double bitgrenze = 0;

int read_signed_sample(FILE *fp) {  // int = i32_t
    int byte, i, sample, s=0;       // EOF -> 0x1000000

    for (i = 0; i < channels; i++) {
                           // i = 0: links bzw. mono
        byte = fgetc(fp);
        if (byte == EOF) return EOF_INT;
        if (i == 0) sample = byte;

        if (bits_sample == 16) {
            byte = fgetc(fp);
            if (byte == EOF) return EOF_INT;
            if (i == 0) sample +=  byte << 8;
        }

    }

    if (bits_sample ==  8)  s = sample-128;   // 8bit: 00..FF, centerpoint 0x80=128
    if (bits_sample == 16)  s = (short)sample;

    if (option_avg) {
        movAvg[sample_count % LEN_movAvg] = s;
        s = 0;
        for (i = 0; i < LEN_movAvg; i++) s += movAvg[i];
        s = (s+0.5) / LEN_movAvg;
    }

    sample_count++;

    return s;
}

int par=1, par_alt=1;

int read_bits_fsk(FILE *fp, int *bit, int *len) {
    static int sample;
    int n, y0;
    float l, x1;
    static float x0;

    n = 0;
    do{
        y0 = sample;
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        par_alt = par;
        par =  (sample >= 0) ? 1 : -1;  // 8bit: 0..127,128..255 (-128..-1,0..127)
        n++;
    } while (par*par_alt > 0);

    if (!option_res) l = (float)n / samples_per_bit;
    else {                                 // genauere Bitlaengen-Messung
        x1 = sample/(float)(sample-y0);    // hilft bei niedriger sample rate
        l = (n+x0-x1) / samples_per_bit;   // meist mehr frames (nicht immer)
        x0 = x1;
    }

    *len = (int)(l+0.5);

    if (!option_inv) *bit = (1+par_alt)/2;  // oben 1, unten -1
    else             *bit = (1-par_alt)/2;  // sdr#<rev1381?, invers: unten 1, oben -1

    /* Y-offset ? */

    return 0;
}

int bitstart = 0;
int read_rawbit(FILE *fp, int *bit) {
    int sample;
    int n, sum;

    sum = 0;
    n = 0;

    if (bitstart) {
        n = 1;    // d.h. bitgrenze = sample_count-1 (?)
        bitgrenze = sample_count-1;
        bitstart = 0;
    }
    bitgrenze += samples_per_bit;

    do {
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        //par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        sum += sample;
        n++;
    } while (sample_count < bitgrenze);  // n < samples_per_bit

    if (sum >= 0) *bit = 1;
    else          *bit = 0;

    if (option_inv) *bit ^= 1;

    return 0;
}

/* ------------------------------------------------------------------------------------ */

// manchester1 1->10,0->01: 1.bit
// manchester2 0->10,1->01: 2.bit
// RS92-SGP: 8N1 manchester2
char manch(char *mbits) {
   if      ((mbits[0] == 1) && (mbits[1] == 0)) return 0;
   else if ((mbits[0] == 0) && (mbits[1] == 1)) return 1;
   else return -1;
}
int bits2byte(char bits[]) {
    int i, byteval=0, d=1;
    int bit8[8];

    if (manch(bits+0) != 0) return 0x100;
    for (i = 0; i < 8; i++) {
        bit8[i] = manch(bits+2*(i+1));
    }
    if (manch(bits+(2*(8+1))) != 1) return 0x100;

    for (i = 0; i < 8; i++) {     // little endian
        if      (bit8[i] == 1)  byteval += d;
        else if (bit8[i] == 0)  byteval += 0;
        else return 0x100;
        d <<= 1;
    }
    return byteval;
}


void inc_bufpos() {
  bufpos = (bufpos+1) % HEADLEN;
}

int compare() {
    int i=0, j = bufpos;

    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != header[HEADOFS+HEADLEN-1-i]) break;
        j--;
        i++;
    }
    return i;
}
#endif

/*
uint8_t xorbyte(int pos) {
    return  xframe[pos] ^ mask[pos % MASK_LEN];
}
*/
uint8_t framebyte(int pos) {
    return  frame[pos];
}


/* ------------------------------------------------------------------------------------ */

#define GPS_WEEK1024  1
#define WEEKSEC       604800

/*
 * Convert GPS Week and Seconds to Modified Julian Day.
 * - Adapted from sci.astro FAQ.
 * - Ignores UTC leap seconds.
 */

void Gps2Date(long GpsWeek, long GpsSeconds, int *Year, int *Month, int *Day) {

    long GpsDays, Mjd;
    long J, C, Y, M;

    GpsDays = GpsWeek * 7 + (GpsSeconds / 86400);
    Mjd = 44244 + GpsDays;

    J = Mjd + 2468570;
    C = 4 * J / 146097;
    J = J - (146097 * C + 3) / 4;
    Y = 4000 * (J + 1) / 1461001;
    J = J - 1461 * Y / 4 + 31;
    M = 80 * J / 2447;
    *Day = J - 2447 * M / 80;
    J = M / 11;
    *Month = M + 2 - (12 * J);
    *Year = 100 * (C - 49) + Y + J;
}

/* ------------------------------------------------------------------------------------ */

#define pos_FrameNb   0x08  // 2 byte
#define pos_SondeID   0x0C  // 8 byte  // oder: 0x0A, 10 byte?
#define pos_CalData   0x17  // 1 byte, counter 0x00..0x1f
#define pos_Calfreq   0x1A  // 2 byte, calfr 0x00

#define posGPS_TOW    0x48  // 4 byte
#define posGPS_PRN    0x4E  // 12*5 bit in 8 byte
#define posGPS_STATUS 0x56  // 12 byte
#define posGPS_DATA   0x62  // 12*8 byte

#define pos_PTU       0x2C  // 24 byte
#define pos_AuxData   0xC8  // 8 byte


#define BLOCK_CFG 0x6510  // frame[pos_FrameNb-2], frame[pos_FrameNb-1]
#define BLOCK_PTU 0x690C
#define BLOCK_GPS 0x673D  // frame[posGPS_TOW-2], frame[posGPS_TOW-1]
#define BLOCK_AUX 0x6805

#define LEN_CFG (2*(BLOCK_CFG & 0xFF))
#define LEN_GPS (2*(BLOCK_GPS & 0xFF))
#define LEN_PTU (2*(BLOCK_PTU & 0xFF))


int crc16(int start, int len) {
    int crc16poly = 0x1021;
    int rem = 0xFFFF, i, j;
    int byte;

    if (start+len >= FRAME_LEN) return -1;

    for (i = 0; i < len; i++) {
        byte = framebyte(start+i);
        rem = rem ^ (byte << 8);
        for (j = 0; j < 8; j++) {
            if (rem & 0x8000) {
                rem = (rem << 1) ^ crc16poly;
            }
            else {
                rem = (rem << 1);
            }
            rem &= 0xFFFF;
        }
    }
    return rem;
}

int get_FrameNb() {
    int i;
    unsigned byte;
    uint8_t frnr_bytes[2];
    int frnr;

    for (i = 0; i < 2; i++) {
        byte = framebyte(pos_FrameNb + i);
        frnr_bytes[i] = byte;
    }

    frnr = frnr_bytes[0] + (frnr_bytes[1] << 8);
    gpx.frnr = frnr;

    return 0;
}

int get_SondeID() {
    int i, ret=0;
    unsigned byte;
    uint8_t sondeid_bytes[10];
    int crc_frame, crc;

    // BLOCK_CFG == frame[pos_FrameNb-2 .. pos_FrameNb-1] ?
    crc_frame = framebyte(pos_FrameNb+LEN_CFG) | (framebyte(pos_FrameNb+LEN_CFG+1) << 8);
    crc = crc16(pos_FrameNb, LEN_CFG);
/*
    if (option_crc) {
      //fprintf(stdout, " (%04X:%02X%02X) ", BLOCK_CFG, frame[pos_FrameNb-2], frame[pos_FrameNb-1]);
      fprintf(stdout, " [%04X:%04X] ", crc_frame, crc);
    }
*/
    ret = 0;
    if ( 0  &&  option_crc  &&  crc != crc_frame) {
        ret = -2;  // erst wichtig, wenn Cal/Cfg-Data
    }

    for (i = 0; i < 8; i++) {
        byte = framebyte(pos_SondeID + i);
        if ((byte < 0x20) || (byte > 0x7E)) return -1;
        sondeid_bytes[i] = byte;
    }

    for (i = 0; i < 8; i++) {
        gpx.id[i] = sondeid_bytes[i];
    }
    gpx.id[8] = '\0';

    return ret;
}

char weekday[7][3] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

int get_GPStime() {
    int i, ret=0;
    unsigned byte;
    uint8_t gpstime_bytes[4];
    int gpstime = 0, // 32bit
        day;
    int ms;
    int crc_frame, crc;

    // BLOCK_GPS == frame[posGPS_TOW-2 .. posGPS_TOW-1] ?
    crc_frame = framebyte(posGPS_TOW+LEN_GPS) | (framebyte(posGPS_TOW+LEN_GPS+1) << 8);
    crc = crc16(posGPS_TOW, LEN_GPS);
/*
    if (option_crc) {
      //fprintf(stdout, " (%04X:%02X%02X) ", BLOCK_GPS, frame[posGPS_TOW-2], frame[posGPS_TOW-1]);
      fprintf(stdout, " [%04X:%04X] ", crc_frame, crc);
    }
*/
    ret = 0;
    if (option_crc  &&  crc != crc_frame) {
        ret = -2;
    }

    for (i = 0; i < 4; i++) {
        byte = framebyte(posGPS_TOW + i);
        gpstime_bytes[i] = byte;
    }

    memcpy(&gpstime, gpstime_bytes, 4);
    ms = gpstime % 1000;
    gpstime /= 1000;

    gpx.gpssec = gpstime;
    Serial.printf("GPS time is %04x (%d)\n", gpstime, gpstime);

    day = (gpstime / (24 * 3600)) % 7;        // besser CRC-check, da auch
    //if ((day < 0) || (day > 6)) return -1;  // gpssec=604800,604801 beobachtet

    gpstime %= (24*3600);

    gpx.wday = day;
    gpx.std = gpstime / 3600;
    gpx.min = (gpstime % 3600) / 60;
    gpx.sek = gpstime % 60 + ms/1000.0;

    return ret;
}

int get_Aux() {
    int i;
    unsigned short byte;

    for (i = 0; i < 4; i++) {
        byte = framebyte(pos_AuxData+2*i)+(framebyte(pos_AuxData+2*i+1)<<8);
        gpx.aux[i] = byte;
    }

    return 0;
}

int get_Cal() {
    int i;
    unsigned byte;
    uint8_t calfr = 0;
    //uint8_t burst = 0;
    int freq = 0;
    uint8_t freq_bytes[2];

    byte = framebyte(pos_CalData);
    calfr = byte;

    if (option_verbose == 4) {
        fprintf(stdout, "\n");
        fprintf(stdout, "[%5d] ", gpx.frnr);
        fprintf(stdout, "  0x%02x:", calfr);
    }
    for (i = 0; i < 16; i++) {
        byte = framebyte(pos_CalData+1+i);
        if (option_verbose == 4) {
            fprintf(stdout, " %02x", byte);
        }
    }
    if (option_aux) {
        get_Aux();
        if (option_verbose == 4) {
            fprintf(stdout, "  #  ");
            for (i = 0; i < 8; i++) {
                byte = framebyte(pos_AuxData+i);
                fprintf(stdout, "%02x ", byte);
            }
        }
        else {
            if (gpx.aux[0] != 0 || gpx.aux[1] != 0 || gpx.aux[2] != 0 || gpx.aux[3] != 0) {
                fprintf(stdout, " # %04x %04x %04x %04x", gpx.aux[0], gpx.aux[1], gpx.aux[2], gpx.aux[3]);
            }
        }
    }

    if (calfr == 0x00) {
        for (i = 0; i < 2; i++) {
            byte = framebyte(pos_Calfreq + i);
            freq_bytes[i] = byte;
        }
        byte = freq_bytes[0] + (freq_bytes[1] << 8);
        //fprintf(stdout, ":%04x ", byte);
        freq = 400000 + 10*byte; // kHz;
        gpx.freq = freq;
        fprintf(stdout, " : fq %d kHz", freq);
    }

    return 0;
}


/* ---------------------------------------------------------------------------------------------------- */



//we only use ephs  EPHEM_t alm[33];
//EPHEM_t eph[33][24];
EPHEM_t *ephs = NULL;

SAT_t sat[33],
      sat1s[33];


uint8_t prn_le[12*5+4];
/* le - little endian */
int prnbits_le(uint16_t byte16, uint8_t bits[64], int block) {
    int i; /* letztes bit Ueberlauf, wenn 3. PRN = 32 */
    for (i = 0; i < 15; i++) {
        bits[15*block+i] = byte16 & 1;
        byte16 >>= 1;
    }
    bits[60+block] = byte16 & 1;
    return byte16 & 1;
}
uint8_t prns[12], // PRNs in data
      sat_status[12];
int prn32toggle = 0x1, ind_prn32, prn32next;
void prn12(uint8_t *prn_le, uint8_t prns[12]) {
    int i, j, d;
    for (i = 0; i < 12; i++) {
        prns[i] = 0;
        d = 1;
        for (j = 0; j < 5; j++) {
          if (prn_le[5*i+j]) prns[i] += d;
          d <<= 1;
        }
    }
    ind_prn32 = 32;
    for (i = 0; i < 12; i++) {
        // PRN-32 overflow
        if ( (prns[i] == 0) && (sat_status[i] & 0x0F) ) {  // 5 bit: 0..31
            if (  ((i % 3 == 2) && (prn_le[60+i/3] & 1))       // Spalte 2
               || ((i % 3 != 2) && (prn_le[5*(i+1)] & 1)) ) {  // Spalte 0,1
                prns[i] = 32; ind_prn32 = i;
            }
        }
        else if ((sat_status[i] & 0x0F) == 0) {  // erste beiden bits: 0x03 ?
            prns[i] = 0;
        }
    }

    prn32next = 0;
    if (ind_prn32 < 12) {
        // PRN-32 overflow
        if (ind_prn32 % 3 != 2) { // -> ind_prn32<11                            // vorausgesetzt im Block folgt auf PRN-32
            if ((sat_status[ind_prn32+1] & 0x0F)  &&  prns[ind_prn32+1] > 1) {  // entweder PRN-1 oder PRN-gerade
                                               // &&  prns[ind_prn32+1] != 3 ?
                for (j = 0; j < ind_prn32; j++) {
                    if (prns[j] == (prns[ind_prn32+1]^prn32toggle)  &&  (sat_status[j] & 0x0F)) break;
                }
                if (j < ind_prn32) { prn32toggle ^= 0x1; }
                else {
                    for (j = ind_prn32+2; j < 12; j++) {
                        if (prns[j] == (prns[ind_prn32+1]^prn32toggle)  &&  (sat_status[j] & 0x0F)) break;
                    }
                    if (j < 12) { prn32toggle ^= 0x1; }
                }
                prns[ind_prn32+1] ^= prn32toggle;
                /*
                  // nochmal testen
                  for (j = 0; j < ind_prn32; j++) { if (prns[j] == prns[ind_prn32+1]) break; }
                  if (j < ind_prn32) prns[ind_prn32+1] = 0;
                  else {
                      for (j = ind_prn32+2; j < 12; j++) { if (prns[j] == prns[ind_prn32+1]) break; }
                      if (j < 12) prns[ind_prn32+1] = 0;
                  }
                  if (prns[ind_prn32+1] == 0) { prn32toggle ^= 0x1; }
                */
            }
            prn32next = prns[ind_prn32+1];  // ->  ind_prn32<11  &&  ind_prn32 % 3 != 2
        }
    }
}


int calc_satpos_alm(EPHEM_t alm[], double t, SAT_t *satp) {
    return -1;
#if 0
    double X, Y, Z, vX, vY, vZ;
    int j;
    int week;
    double cl_corr, cl_drift;

    for (j = 1; j < 33; j++) {
        if (alm[j].prn > 0) {  // prn==j

            // Woche hat 604800 sec
            if      (t-alm[j].toa >  WEEKSEC/2) rollover = +1;
            else if (t-alm[j].toa < -WEEKSEC/2) rollover = -1;
            else rollover = 0;
            week = alm[j].week - rollover;
            /*if (j == 1)*/ gpx.week = week + GPS_WEEK1024*1024;

            if (option_vel >= 2) {
                GPS_SatellitePositionVelocity_Ephem(
                    week, t, alm[j],
                    &cl_corr, &cl_drift, &X, &Y, &Z, &vX, &vY, &vZ
                );
                satp[alm[j].prn].clock_drift = cl_drift;
                satp[alm[j].prn].vX = vX;
                satp[alm[j].prn].vY = vY;
                satp[alm[j].prn].vZ = vZ;
            }
            else {
                GPS_SatellitePosition_Ephem(
                    week, t, alm[j],
                    &cl_corr, &X, &Y, &Z
                );
            }

            satp[alm[j].prn].X = X;
            satp[alm[j].prn].Y = Y;
            satp[alm[j].prn].Z = Z;
            satp[alm[j].prn].clock_corr = cl_corr;

        }
    }

    return 0;
#endif
}

int calc_satpos_rnx(EPHEM_t eph[][24], double t, SAT_t *satp) {
    double X, Y, Z, vX, vY, vZ;
    int j, i, ti;
    int week = 0;
    double cl_corr, cl_drift;
    double tdiff, td;

    for (j = 1; j < 33; j++) {

        // Woche hat 604800 sec
        tdiff = WEEKSEC;
        ti = 0;
        for (i = 0; i < 24; i++) {
            if (eph[j][i].prn > 0) {
                if      (t-eph[j][i].toe >  WEEKSEC/2) rollover = +1;
                else if (t-eph[j][i].toe < -WEEKSEC/2) rollover = -1;
                else rollover = 0;
                td = t-eph[j][i].toe - rollover*WEEKSEC;
                if (td < 0) td *= -1;

                if ( td < tdiff ) {
                    tdiff = td;
                    ti = i;
                    week = eph[j][ti].week - rollover;
                    gpx.week = eph[j][ti].gpsweek - rollover;
                }
            }
        }

        if (option_vel >= 2) {
            GPS_SatellitePositionVelocity_Ephem(
                week, t, eph[j][ti],
                &cl_corr, &cl_drift, &X, &Y, &Z, &vX, &vY, &vZ
            );
            satp[eph[j][ti].prn].clock_drift = cl_drift;
            satp[eph[j][ti].prn].vX = vX;
            satp[eph[j][ti].prn].vY = vY;
            satp[eph[j][ti].prn].vZ = vZ;
        }
        else {
            GPS_SatellitePosition_Ephem(
                week, t, eph[j][ti],
                &cl_corr, &X, &Y, &Z
            );
        }

        satp[eph[j][ti].prn].X = X;
        satp[eph[j][ti].prn].Y = Y;
        satp[eph[j][ti].prn].Z = Z;
        satp[eph[j][ti].prn].clock_corr = cl_corr;

    }

    return 0;
}

int calc_satpos_rnx2(EPHEM_t *eph, double t, SAT_t *satp) {
    double X, Y, Z, vX, vY, vZ;
    int j;
    int week = 0;
    double cl_corr, cl_drift;
    double tdiff, td;
    int count, count0, satfound;

    for (j = 1; j < 33; j++) {

        count = count0 = 1;
        satfound = 0;

        // Woche hat 604800 sec
        tdiff = WEEKSEC;
        while (eph[count].prn > 0) {
            if (eph[count].prn == j) {

                satfound += 1;

                if      (t - eph[count].toe >  WEEKSEC/2) rollover = +1;
                else if (t - eph[count].toe < -WEEKSEC/2) rollover = -1;
                else rollover = 0;
                td = fabs( t - eph[count].toe - rollover*WEEKSEC);

                if ( td < tdiff ) {
                    tdiff = td;
                    week = eph[count].week - rollover;
                    gpx.week = eph[count].gpsweek - rollover;
                    count0 = count;
                }
            }
            count += 1;
        }

        if ( satfound )
        {
            if (option_vel >= 2) {
                GPS_SatellitePositionVelocity_Ephem(
                    week, t, eph[count0],
                    &cl_corr, &cl_drift, &X, &Y, &Z, &vX, &vY, &vZ
                );
                satp[j].clock_drift = cl_drift;
                satp[j].vX = vX;
                satp[j].vY = vY;
                satp[j].vZ = vZ;
            }
            else {
                GPS_SatellitePosition_Ephem(
                    week, t, eph[count0],
                    &cl_corr, &X, &Y, &Z
                );
            }

            satp[j].X = X;
            satp[j].Y = Y;
            satp[j].Z = Z;
            satp[j].clock_corr = cl_corr;
            satp[j].ephtime = eph[count0].toe;
        }

    }

    return 0;
}


typedef struct {
    uint32_t tow;
    uint8_t status;
    int chips;
    int deltachips;
} RANGE_t;
RANGE_t range[33];

int prn[12];  // valide PRN 0,..,k-1


// pseudo.range = -df*pseudo.chips
//           df = lightspeed/(chips/sec)/2^10
const double df = 299792.458/1023.0/1024.0; //0.286183844 // c=299792458m/s, 1023000chips/s
//           dl = L1/(chips/sec)/4
const double dl = 1575.42/1.023/4.0; //385.0 // GPS L1 1575.42MHz=154*10.23MHz, dl=154*10/4

double pr_ofs;
double GPSsatAlt = 20200e3;

int get_pseudorange() {
    uint32_t gpstime;
    uint8_t gpstime_bytes[4];
    uint8_t pseudobytes[4];
    uint32_t chipbytes, deltabytes;
    int i, j, k;
    uint8_t bytes[4];
    uint16_t byte16;
    double  pr0, prj;

    // GPS-TOW in ms
    for (i = 0; i < 4; i++) {
        gpstime_bytes[i] = framebyte(posGPS_TOW + i);
    }
    memcpy(&gpstime, gpstime_bytes, 4);

    // Sat Status
    Serial.print("Sat status: ");
    for (i = 0; i < 12; i++) {
        sat_status[i] = framebyte(posGPS_STATUS + i);
	Serial.printf("%d:%d ", i, sat_status[i]);
    }
    Serial.print("\n");

    // PRN-Nummern
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 2; j++) {
            bytes[j] = frame[posGPS_PRN+2*i+j];
        }
        memcpy(&byte16, bytes, 2);
        prnbits_le(byte16, prn_le, i);
    }
    prn12(prn_le, prns);


    // GPS Sat Pos (& Vel)
    //if (almanac) calc_satpos_alm(  alm, gpstime/1000.0, sat);
    if (ephem)   calc_satpos_rnx2(ephs, gpstime/1000.0, sat);

    // GPS Sat Pos t -= 1s
    if (option_vel == 1) {
        //if (almanac) calc_satpos_alm(  alm, gpstime/1000.0-1, sat1s);
        if (ephem)   calc_satpos_rnx2(ephs, gpstime/1000.0-1, sat1s);
    }

    k = 0;
    for (j = 0; j < 12; j++) {

        // Pseudorange/chips
        for (i = 0; i < 4; i++) {
            pseudobytes[i] = frame[posGPS_DATA+8*j+i];
        }
        memcpy(&chipbytes, pseudobytes, 4);
	//Serial.printf("Chipbytes(%d): %04x\n",j, chipbytes);

        // delta_pseudochips / 385
        for (i = 0; i < 3; i++) {
            pseudobytes[i] = frame[posGPS_DATA+8*j+4+i];
        }
        deltabytes = 0; // bzw. pseudobytes[3]=0 (24 bit);  deltabytes & (0xFF<<24) als
        memcpy(&deltabytes, pseudobytes, 3); // gemeinsamer offset relevant in --vel1 !

        //if ( (prns[j] == 0) && (sat_status[j] & 0x0F) )  prns[j] = 32;
        range[prns[j]].tow = gpstime;
        range[prns[j]].status = sat_status[j];

        if ( chipbytes == 0x7FFFFFFF  ||  chipbytes == 0x55555555 ) {
             range[prns[j]].chips = 0;
             continue;
        }
        if (option_vergps != 8) {
        if ( chipbytes >  0x10000000  &&  chipbytes <  0xF0000000 ) {
             range[prns[j]].chips = 0;
             continue;
        }}

        range[prns[j]].chips = chipbytes;
        range[prns[j]].deltachips = deltabytes;

/*
        if (range[prns[j]].deltachips == 0x555555) {
            range[prns[j]].deltachips = 0;
            continue;
        }
*/
	Serial.printf("j=%d: prns=%d status=%d  dist=%f\n ", j, prns[j], sat_status[j], dist(sat[prns[j]].X, sat[prns[j]].Y, sat[prns[j]].Z, 0, 0, 0));
	//int o=prns[j];
	//Serial.printf("x=%f y=%f z=%f\n", sat[o].X, sat[o].Y, sat[o].Z);
        if (  (prns[j] > 0)  &&  ((sat_status[j] & 0x0F) == 0xF)
           && (dist(sat[prns[j]].X, sat[prns[j]].Y, sat[prns[j]].Z, 0, 0, 0) > 6700000) )
        {
            for (i = 0; i < k; i++) { if (prn[i] == prns[j]) break; }
            if (i == k  &&  prns[j] != exSat) {
                //if ( range[prns[j]].status & 0xF0 )  // Signalstaerke > 0 ?
                {
                    prn[k] = prns[j];
                    k++;
                }
            }
        }

    }


    for (j = 0; j < 12; j++) {    // 0x013FB0A4
        sat[prns[j]].pseudorange = /*0x01400000*/ - range[prns[j]].chips * df;
        sat1s[prns[j]].pseudorange = -(range[prns[j]].chips - range[prns[j]].deltachips/dl)*df;
                                   //+ sat[prns[j]].clock_corr - sat1s[prns[j]].clock_corr
        sat[prns[j]].pseudorate = - range[prns[j]].deltachips * df / dl;

        sat[prns[j]].prn = prns[j];
        sat1s[prns[j]].prn = prns[j];
    }


    pr0 = (double)0x01400000;
    for (j = 0; j < k; j++) {
        prj = sat[prn[j]].pseudorange + sat[prn[j]].clock_corr;
        if (prj < pr0) pr0 = prj;
    }
    for (j = 0; j < k; j++) sat[prn[j]].PR = sat[prn[j]].pseudorange + sat[prn[j]].clock_corr - pr0 + GPSsatAlt;
    // es kann PRNs geben, die zeitweise stark abweichende PR liefern;
    // eventuell Standardabweichung ermitteln und fehlerhafte Sats weglassen
    for (j = 0; j < k; j++) {                      //  sat/sat1s...             PR-check
        sat1s[prn[j]].PR = sat1s[prn[j]].pseudorange + sat[prn[j]].clock_corr - pr0 + GPSsatAlt;
    }
    pr_ofs = pr0;

    return k;
}

int get_GPSvel(double lat, double lon, double vel_ecef[3],
               double *vH, double *vD, double *vU) {
    // ECEF-Velocities
    // ECEF-Vel -> NorthEastUp
    double phi = lat*M_PI/180.0;
    double lam = lon*M_PI/180.0;
    double vN = -vel_ecef[0]*sin(phi)*cos(lam) - vel_ecef[1]*sin(phi)*sin(lam) + vel_ecef[2]*cos(phi);
    double vE = -vel_ecef[0]*sin(lam) + vel_ecef[1]*cos(lam);
    *vU =  vel_ecef[0]*cos(phi)*cos(lam) + vel_ecef[1]*cos(phi)*sin(lam) + vel_ecef[2]*sin(phi);
    // NEU -> HorDirVer
    *vH = sqrt(vN*vN+vE*vE);
    *vD = atan2(vE, vN) * 180 / M_PI;
    if (*vD < 0) *vD += 360;

    return 0;
}

double DOP[4];

int naiv_2Dfix(int N, SAT_t sats[], double alt) {
// simple 2D fix: 3 Sats & Alt above ellipsoid
//
// - fuer 3 unbekannte lat, lon, t braucht man mind. 3 Satelliten
// - fuer Iteration braucht man jedoch einen Startwert
// - es gibt direkte Methoden
// - hier werden die vorhandenen Funktionen benutzt:
//   - der 4. Satellit im Erdmittelpunkt
//   - seine pseudorange(+clock) wird grob geschaetzt
//     (pseudochips liefern erst Rueckschluesse, wenn man Position kennt)
//   - dann approximieren, bis Hoehe stimmt
// - bei 3-4 Satelliten ist die DOP-Konstellation oft schlecht
// - moeglicherweise ist in einigen Faellen die 2. Loesung besser

    double radius = 6371e3 + alt; // wird dann approximiert
    double lat2d, lon2d, alt2d,
           pos_ecef[3], rx_cl_bias,
           dpos_ecef[3];
    //double d;
    double rofs = 200000.0, rdiff = 0.0;
    int k, k_limit;
    double gdop = -1;


    sats[3].X = sats[3].Y = sats[3].Z = 0;

    k = 0;
    k_limit = 100;

    if (N >= 3) {

            do
            {
                // PR = pseudorange + clock_corr - pr_ofs + GPSsatAlt
                sats[3].X = sats[3].Y = sats[3].Z = 0;
                sats[3].PR = radius - rofs;
                sats[3].pseudorange = sats[3].PR + pr_ofs - GPSsatAlt;


                NAV_bancroft1(4, sats, pos_ecef, &rx_cl_bias);
                //NAV_bancroft3(4, sats, pos_ecef1, &rx_cl_bias1, pos_ecef2, &rx_cl_bias2);

                ecef2elli(pos_ecef[0], pos_ecef[1], pos_ecef[2], &lat2d, &lon2d, &alt2d);
                rdiff = alt-alt2d;

                rofs -= rdiff*1.2;
                k += 1;

            } while (k < k_limit  &&  fabs(rdiff) > 1.0);

            NAV_LinP(4, sats, pos_ecef, rx_cl_bias, dpos_ecef, &rx_cl_bias);
            //  for (j=0;j<3;j++) pos_ecef[j] += dpos_ecef[j];
            //  NAV_LinP(4, sats, pos_ecef, rx_cl_bias, dpos_ecef, &rx_cl_bias);
            //  d = dist(0, 0, 0, dpos_ecef[0], dpos_ecef[1], dpos_ecef[2]);

    }

    if (calc_DOPn(4, sats, pos_ecef, DOP) == 0) {
        gdop = sqrt(DOP[0]+DOP[1]+DOP[2]+DOP[3]);
    }
    //if (gdop > 2*dop_limit) gdop = -1;
    //if (gdop < 0) gdop = -1;

    gpx.lat = lat2d;
    gpx.lon = lon2d;
    gpx.alt = alt2d;
    gpx.dop = gdop;

    if ( fabs(alt2d-alt) > 1000.0 ) return -1;
    if ( k == k_limit ) return 0;
    return 1;
}


int get_GPSkoord(int N) {
    double lat, lon, alt, rx_cl_bias;
    double vH, vD, vU;
    double lat1s, lon1s, alt1s,
           lat0 , lon0 , alt0 , pos0_ecef[3];
    double pos_ecef[3], pos1s_ecef[3], dpos_ecef[3],
           vel_ecef[3], dvel_ecef[3];
    double gdop, gdop0 = 1000.0;
    //double hdop, vdop, pdop;
    int i0, i1, i2, i3, j, k, n;
    int nav_ret = 0;
    int num = 0;
    SAT_t Sat_A[4];
    SAT_t Sat_B[12]; // N <= 12
    SAT_t Sat_B1s[12];
    SAT_t Sat_C[12]; // 11
    double diter = 0;
    int exN = -1;

    if (option_vergps == 8) {
        fprintf(stdout, "  sats: ");
        for (j = 0; j < N; j++) fprintf(stdout, "%02d ", prn[j]);
        fprintf(stdout, "\n");
    }

    gpx.lat = gpx.lon = gpx.alt = 0;

    if (option_vergps != 2) {
    for (i0=0;i0<N;i0++) { for (i1=i0+1;i1<N;i1++) { for (i2=i1+1;i2<N;i2++) { for (i3=i2+1;i3<N;i3++) {

        Sat_A[0] = sat[prn[i0]];
        Sat_A[1] = sat[prn[i1]];
        Sat_A[2] = sat[prn[i2]];
        Sat_A[3] = sat[prn[i3]];
        nav_ret = NAV_ClosedFormSolution_FromPseudorange( Sat_A, &lat, &lon, &alt, &rx_cl_bias, pos_ecef );

        if (nav_ret == 0) {
            num += 1;
            if (calc_DOPn(4, Sat_A, pos_ecef, DOP) == 0) {
                gdop = sqrt(DOP[0]+DOP[1]+DOP[2]+DOP[3]);
                //fprintf(stdout, " DOP : %.1f ", gdop);

                NAV_LinP(4, Sat_A, pos_ecef, rx_cl_bias, dpos_ecef, &rx_cl_bias);
                diter = dist(0, 0, 0, dpos_ecef[0], dpos_ecef[1], dpos_ecef[2]);
                for (j = 0; j < 3; j++) pos_ecef[j] += dpos_ecef[j];
                ecef2elli(pos_ecef[0], pos_ecef[1], pos_ecef[2], &lat, &lon, &alt);
                if ( option_vel == 4 ) {
                    vel_ecef[0] = vel_ecef[1] = vel_ecef[2] = 0;
                    NAV_LinV(4, Sat_A, pos_ecef, vel_ecef, 0.0, dvel_ecef, &rx_cl_bias);
                    for (j=0; j<3; j++) vel_ecef[j] += dvel_ecef[j];
                    NAV_LinV(4, Sat_A, pos_ecef, vel_ecef, rx_cl_bias, dvel_ecef, &rx_cl_bias);
                    for (j=0; j<3; j++) vel_ecef[j] += dvel_ecef[j];
                    get_GPSvel(lat, lon, vel_ecef, &vH, &vD, &vU);
                }
                if (option_vergps == 8) {
                    // gdop = sqrt(DOP[0]+DOP[1]+DOP[2]+DOP[3]); // s.o.
                    //hdop = sqrt(DOP[0]+DOP[1]);
                    //vdop = sqrt(DOP[2]);
                    //pdop = sqrt(DOP[0]+DOP[1]+DOP[2]);
                    if (gdop < dop_limit) {
                        fprintf(stdout, "       ");
                        fprintf(stdout, "lat: %.5f , lon: %.5f , alt: %.1f ", lat, lon, alt);
                        fprintf(stdout, " (d:%.1f)", diter);
                        if ( option_vel == 4 ) {
                            fprintf(stdout, "  vH: %4.1f  D: %5.1f°  vV: %3.1f ", vH, vD, vU);
                        }
                        fprintf(stdout, "  sats: ");
                        fprintf(stdout, "%02d %02d %02d %02d  ", prn[i0], prn[i1], prn[i2], prn[i3]);
                        fprintf(stdout, " GDOP : %.1f  ", gdop);
                        //fprintf(stdout, " HDOP: %.1f  VDOP: %.1f ", hdop, vdop);
                        //fprintf(stdout, " PDOP: %.1f  ", pdop);
                        fprintf(stdout, "\n");
                    }
                }
            }
            else gdop = -1;

            if (gdop > 0 && gdop < gdop0) {  // wenn fehlerhafter Sat, diter wohl besserer Indikator
                gpx.lat = lat;
                gpx.lon = lon;
                gpx.alt = alt;
                gpx.dop = gdop;
                gpx.diter = diter;
                gpx.sats[0] = prn[i0]; gpx.sats[1] = prn[i1]; gpx.sats[2] = prn[i2]; gpx.sats[3] = prn[i3];
                gdop0 = gdop;

                if (option_vel == 4) {
                    gpx.vH = vH;
                    gpx.vD = vD;
                    gpx.vU = vU;
                }
            }
        }

    }}}}
    }

    if (option_vergps == 8  ||  option_vergps == 2) {

        for (j = 0; j < N; j++) Sat_B[j] = sat[prn[j]];
        for (j = 0; j < N; j++) Sat_B1s[j] = sat1s[prn[j]];

        NAV_bancroft1(N, Sat_B, pos_ecef, &rx_cl_bias);
        ecef2elli(pos_ecef[0], pos_ecef[1], pos_ecef[2], &lat, &lon, &alt);
        gdop = -1;
        if (calc_DOPn(N, Sat_B, pos_ecef, DOP) == 0) {
            gdop = sqrt(DOP[0]+DOP[1]+DOP[2]+DOP[3]);
        }

        NAV_LinP(N, Sat_B, pos_ecef, rx_cl_bias, dpos_ecef, &rx_cl_bias);
        if (option_iter) {
            for (j = 0; j < 3; j++) pos_ecef[j] += dpos_ecef[j];
            ecef2elli(pos_ecef[0], pos_ecef[1], pos_ecef[2], &lat, &lon, &alt);
        }
        gpx.diter = dist(0, 0, 0, dpos_ecef[0], dpos_ecef[1], dpos_ecef[2]);

        // Sat mit schlechten Daten suchen
        if (gpx.diter > d_err) {
            if (N > 5) {  // N > 4 kann auch funktionieren
                for (n = 0; n < N; n++) {
                    k = 0;
                    for (j = 0; j < N; j++) {
                        if (j != n) {
                            Sat_C[k] = Sat_B[j];
                            k++;
                        }
                    }
                    for (j = 0; j < 3; j++) pos0_ecef[j] = 0;
                    NAV_bancroft1(N-1, Sat_C, pos0_ecef, &rx_cl_bias);
                    NAV_LinP(N-1, Sat_C, pos0_ecef, rx_cl_bias, dpos_ecef, &rx_cl_bias);
                    diter = dist(0, 0, 0, dpos_ecef[0], dpos_ecef[1], dpos_ecef[2]);
                    ecef2elli(pos0_ecef[0], pos0_ecef[1], pos0_ecef[2], &lat0, &lon0, &alt0);
                    if (diter < gpx.diter) {
                        gpx.diter = diter;
                        for (j = 0; j < 3; j++) pos_ecef[j] = pos0_ecef[j];
                        lat = lat0;
                        lon = lon0;
                        alt = alt0;
                        exN = n;
                    }
                }
                if (exN >= 0) {
                    if (prn[exN] == prn32next) prn32toggle ^= 0x1;
                    for (k = exN; k < N-1; k++) {
                        Sat_B[k] = Sat_B[k+1];
                        prn[k] = prn[k+1];
                        if (option_vel == 1) {
                            Sat_B1s[k] = Sat_B1s[k+1];
                        }
                    }
                    N = N-1;
                    if (calc_DOPn(N, Sat_B, pos_ecef, DOP) == 0) {
                        gdop = sqrt(DOP[0]+DOP[1]+DOP[2]+DOP[3]);
                    }
                }
            }
/*
            if (exN < 0  &&  prn32next > 0) {
                //prn32next used in pre-fix? prn32toggle ^= 0x1;
            }
*/
        }

        if (option_vel == 1) {
            NAV_bancroft1(N, Sat_B1s, pos1s_ecef, &rx_cl_bias);
            if (option_iter) {
                NAV_LinP(N, Sat_B1s, pos1s_ecef, rx_cl_bias, dpos_ecef, &rx_cl_bias);
                for (j = 0; j < 3; j++) pos1s_ecef[j] += dpos_ecef[j];
            }
            for (j = 0; j < 3; j++) vel_ecef[j] = pos_ecef[j] - pos1s_ecef[j];
            get_GPSvel(lat, lon, vel_ecef, &vH, &vD, &vU);
            ecef2elli(pos1s_ecef[0], pos1s_ecef[1], pos1s_ecef[2], &lat1s, &lon1s, &alt1s);
            if (option_vergps == 8) {
                fprintf(stdout, "\ndeltachips1s lat: %.6f , lon: %.6f , alt: %.2f ", lat1s, lon1s, alt1s);
                fprintf(stdout, " vH: %4.1f  D: %5.1f°  vV: %3.1f ", vH, vD, vU);
                fprintf(stdout, "\n");
            }
        }
        if (option_vel >= 2) {
              //fprintf(stdout, "\nP(%.1f,%.1f,%.1f) \n", pos_ecef[0], pos_ecef[1], pos_ecef[2]);
            vel_ecef[0] = vel_ecef[1] = vel_ecef[2] = 0;
            NAV_LinV(N, Sat_B, pos_ecef, vel_ecef, 0.0, dvel_ecef, &rx_cl_bias);
            for (j=0; j<3; j++) vel_ecef[j] += dvel_ecef[j];
              //fprintf(stdout, " V(%.1f,%.1f,%.1f) ", vel_ecef[0], vel_ecef[1], vel_ecef[2]);
              //fprintf(stdout, " rx_vel_bias: %.1f \n", rx_cl_bias);
            /* 2. Iteration:
                NAV_LinV(N, Sat_B, pos_ecef, vel_ecef, rx_cl_bias, dvel_ecef, &rx_cl_bias);
                for (j=0; j<3; j++) vel_ecef[j] += dvel_ecef[j];
                //fprintf(stdout, " V(%.1f,%.1f,%.1f) ", vel_ecef[0], vel_ecef[1], vel_ecef[2]);
                //fprintf(stdout, " rx_vel_bias: %.1f \n", rx_cl_bias);
            */
            get_GPSvel(lat, lon, vel_ecef, &vH, &vD, &vU);
        }

        if (option_vergps == 8) {
            fprintf(stdout, "bancroft[%2d] lat: %.6f , lon: %.6f , alt: %.2f ", N, lat, lon, alt);
            fprintf(stdout, " (d:%.1f)", gpx.diter);
            if (option_vel) {
                fprintf(stdout, "  vH: %4.1f  D: %5.1f°  vV: %3.1f ", vH, vD, vU);
            }
            fprintf(stdout, "  DOP[");
            for (j = 0; j < N; j++) {
                fprintf(stdout, "%d", prn[j]);
                if (j < N-1) fprintf(stdout, ","); else fprintf(stdout, "] %.1f ", gdop);
            }
            fprintf(stdout, "\n");
        }

        if (option_vergps == 2) {
            gpx.lat = lat;
            gpx.lon = lon;
            gpx.alt = alt;
            gpx.dop = gdop;
            num = N;

            if (option_vel) {
                gpx.vH = vH;
                gpx.vD = vD;
                gpx.vU = vU;
            }
        }

    }

    return num;
}


/* ------------------------------------------------------------------------------------ */


int print_position() {  // GPS-Hoehe ueber Ellipsoid
    int j, k = 0, n = 0;
    int err1, err2, fix2d = 0;

    err1 = 0;
    if (!option_verbose) err1 = err_gps;
    err1 |= get_FrameNb();
    err1 |= get_SondeID();

    err2  = err1 | err_gps;
  //err2 |= get_GPSweek();
    err2 |= get_GPStime();

#if 0
    Serial.printf("ephem=%d\n",ephem);
    Serial.printf("print_position: ephs is %p\n", ephs);
#endif

    if (!err2 && (almanac || ephem)) {
        k = get_pseudorange();
	gpx.k = k;
	Serial.printf("k=%d\n", k);
        if (k >= 4) {
            n = get_GPSkoord(k);
        }
	if (k == 3) {
	    SAT_t Sat_A[4];
	    for (j = 0; j < 3; j++) { Sat_A[j] = sat[prn[j]]; }
	    fix2d = naiv_2Dfix( 3, Sat_A, sonde.config.rs92.alt2d);
	}
    }

    if (!err1) {
        fprintf(stdout, "[%5d] ", gpx.frnr);
        fprintf(stdout, "(%s) ", gpx.id);
    }

    if (!err2) {
        if (option_verbose) {
            Gps2Date(gpx.week, gpx.gpssec, &gpx.jahr, &gpx.monat, &gpx.tag);
            //fprintf(stdout, "(W %d) ", gpx.week);
            fprintf(stdout, "(%04d-%02d-%02d) ", gpx.jahr, gpx.monat, gpx.tag);
        }
        fprintf(stdout, "%s ", weekday[gpx.wday]);  // %04.1f: wenn sek >= 59.950, wird auf 60.0 gerundet
        fprintf(stdout, "%02d:%02d:%06.3f", gpx.std, gpx.min, gpx.sek);

	if (k == 3  &&  fix2d > 0  &&  gpx.dop > 0  &&  gpx.dop < 2*dop_limit) {
             fprintf(stdout, "  2D  lat: %.5f  lon: %.5f  alt: %.1f ", gpx.lat, gpx.lon, gpx.alt);
             fprintf(stdout, " DOP[%02d,%02d,%02d,0] %.1f ", prn[0], prn[1], prn[2], gpx.dop);
        }
        else if (n > 0) {
            fprintf(stdout, " ");

            if (almanac) fprintf(stdout, " lat: %.4f  lon: %.4f  alt: %.1f ", gpx.lat, gpx.lon, gpx.alt);
            else         fprintf(stdout, " lat: %.5f  lon: %.5f  alt: %.1f ", gpx.lat, gpx.lon, gpx.alt);

            if (option_verbose  &&  option_vergps != 8) {
                fprintf(stdout, " (d:%.1f)", gpx.diter);
            }
            if (option_vel  /*&&  option_vergps >= 2*/) {
                fprintf(stdout,"  vH: %4.1f  D: %5.1f°  vV: %3.1f ", gpx.vH, gpx.vD, gpx.vU);
            }
            if (option_verbose) {
                if (option_vergps != 2) {
                    fprintf(stdout, " DOP[%02d,%02d,%02d,%02d] %.1f",
                                   gpx.sats[0], gpx.sats[1], gpx.sats[2], gpx.sats[3], gpx.dop);
                }
                else {  // wenn option_vergps=2, dann n=N=k(-1)
                    fprintf(stdout, " DOP[");
                    for (j = 0; j < n; j++) {
                        fprintf(stdout, "%d", prn[j]);
                        if (j < n-1) fprintf(stdout, ","); else fprintf(stdout, "] %.1f ", gpx.dop);
                    }
                }
            }
        }

        get_Cal();

        if (option_vergps == 8 /*||  option_vergps == 2*/)
        {
            fprintf(stdout, "\n");
            for (j = 0; j < 60; j++) { fprintf(stdout, "%d", prn_le[j]); if (j % 5 == 4) fprintf(stdout, " "); }
            fprintf(stdout, ": ");
            for (j = 0; j < 12; j++) fprintf(stdout, "%2d ", prns[j]);
            fprintf(stdout, "\n");
            fprintf(stdout, "                                                                  status: ");
            for (j = 0; j < 12; j++) fprintf(stdout, "%02X ", sat_status[j]); //range[prns[j]].status
            fprintf(stdout, "\n");
        }

    }

    if (!err1) {
        fprintf(stdout, "\n");
        //if (option_vergps == 8) fprintf(stdout, "\n");
    }

    return err2;
}

void print_frame(uint8_t *data, int len) {
    int i;
    uint8_t byte;

    for (i = 0; i<len; i++) {
	frame[i] = data[i];
    }
    for (i = len; i < FRAME_LEN; i++) {
        frame[i] = 0;
    }

    if (option_raw) {
        for (i = 0; i < len; i++) {
            //byte = frame[i];
            byte = framebyte(i);
            fprintf(stdout, "%02x", byte);
        }
        fprintf(stdout, "\n");
    }
    //Serial.printf("print_frame: ephs is %p\n", ephs);
    print_position();
}

void get_eph(const char *file) {
        ephs = read_RNXpephs(file);
        if (ephs) {
            ephem = 1;
            almanac = 0;
        }
	Serial.printf("reading RNX done, result is %d, ephs=%p\n", ephem, ephs);
        if (!option_der) d_err = 1000;
}



#if 0
int main(int argc, char *argv[]) {

    FILE *fp, *fp_alm = NULL, *fp_eph = NULL;
    char *fpname;
    char bitbuf[BITS];
    int bit_count = 0,
        byte_count = FRAMESTART,
        header_found = 0,
        byte, i;
    int bit, len;
    char *pbuf = NULL;

#ifdef CYGWIN
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    setbuf(stdout, NULL);

    fpname = argv[0];
    ++argv;
    while ((*argv) && (!fileloaded)) {
        if      ( (strcmp(*argv, "-h") == 0) || (strcmp(*argv, "--help") == 0) ) {
            Serial.printf("%s [options] <file>\n", fpname);
            Serial.printf("  file: audio.wav or raw_data\n");
            Serial.printf("  options:\n");
            Serial.printf("       --vel; --vel1, --vel2 (-g2)\n");
            Serial.printf("       -v, -vx, -vv\n");
            Serial.printf("       -r, --raw\n");
            Serial.printf("       -i, --invert\n");
            Serial.printf("       -a, --almanac  <almanacSEM>\n");
            Serial.printf("       -e, --ephem    <ephemperisRinex>\n");
            Serial.printf("       -g1         (verbose GPS:   4 sats)\n");
            Serial.printf("       -g2         (verbose GPS: all sats)\n");
            Serial.printf("       -gg         (vverbose GPS)\n");
            Serial.printf("       --crc       (CRC check GPS)\n");
            Serial.printf("       --rawin1,2  (raw_data file)\n");
            return 0;
        }
        else if ( (strcmp(*argv, "--vel") == 0) ) {
            option_vel = 4;
        }
        else if ( (strcmp(*argv, "--vel1") == 0) ) {
            option_vel = 1;
            if (option_vergps < 1) option_vergps = 2;
        }
        else if ( (strcmp(*argv, "--vel2") == 0) ) {
            option_vel = 2;
            if (option_vergps < 1) option_vergps = 2;
        }
        else if ( (strcmp(*argv, "--iter") == 0) ) {
            option_iter = 1;
        }
        else if ( (strcmp(*argv, "-v") == 0) ) {
            option_verbose = 1;
        }
        else if ( (strcmp(*argv, "-vv") == 0) ) {
            option_verbose = 4;
        }
        else if ( (strcmp(*argv, "-vx") == 0) ) {
            option_aux = 1;
        }
        else if   (strcmp(*argv, "--crc") == 0) { option_crc = 1; }
        else if ( (strcmp(*argv, "-r") == 0) || (strcmp(*argv, "--raw") == 0) ) {
            option_raw = 1;
        }
        else if ( (strcmp(*argv, "-i") == 0) || (strcmp(*argv, "--invert") == 0) ) {
            option_inv = 1;
        }
        else if ( (strcmp(*argv, "-a") == 0) || (strcmp(*argv, "--almanac") == 0) ) {
            ++argv;
            if (*argv) fp_alm = fopen(*argv, "r"); // txt-mode
            else return -1;
            if (fp_alm == NULL) Serial.printf("[almanac] %s konnte nicht geoeffnet werden\n", *argv);
        }
        else if ( (strcmp(*argv, "-e") == 0) || (strncmp(*argv, "--ephem", 7) == 0) ) {
            ++argv;
            if (*argv) fp_eph = fopen(*argv, "rb"); // bin-mode
            else return -1;
            if (fp_eph == NULL) Serial.printf("[rinex] %s konnte nicht geoeffnet werden\n", *argv);
        }
        else if ( (strcmp(*argv, "--dop") == 0) ) {
            ++argv;
            if (*argv) {
                dop_limit = atof(*argv);
                if (dop_limit <= 0  || dop_limit >= 100)  dop_limit = 9.9;
            }
            else return -1;
        }
        else if ( (strcmp(*argv, "--der") == 0) ) {
            ++argv;
            if (*argv) {
                d_err = atof(*argv);
                if (d_err <= 0  || d_err >= 100000)  d_err = 10000;
                else option_der = 1;
            }
            else return -1;
        }
        else if ( (strcmp(*argv, "--exsat") == 0) ) {
            ++argv;
            if (*argv) {
                exSat = atoi(*argv);
                if (exSat < 1  || exSat > 32)  exSat = -1;
            }
            else return -1;
        }
        else if (strcmp(*argv, "-g1") == 0) { option_vergps = 1; }  //  verbose1 GPS
        else if (strcmp(*argv, "-g2") == 0) { option_vergps = 2; }  //  verbose2 GPS (bancroft)
        else if (strcmp(*argv, "-gg") == 0) { option_vergps = 8; }  // vverbose GPS
        else if (strcmp(*argv, "--rawin1") == 0) { rawin = 2; }     // raw_txt input1
        else if (strcmp(*argv, "--rawin2") == 0) { rawin = 3; }     // raw_txt input2 (SM)
        else if ( (strcmp(*argv, "--avg") == 0) ) {
            option_avg = 1;
        }
        else if   (strcmp(*argv, "-b") == 0) { option_b = 1; }
        else {
            if (!rawin) fp = fopen(*argv, "rb");
            else        fp = fopen(*argv, "r");
            if (fp == NULL) {
                Serial.printf("%s konnte nicht geoeffnet werden\n", *argv);
                return -1;
            }
            fileloaded = 1;
        }
        ++argv;
    }
    if (!fileloaded) fp = stdin;

    if (fp_alm) {
        i = read_SEMalmanac(fp_alm, alm);
        if (i == 0) {
            almanac = 1;
        }
        fclose(fp_alm);
        if (!option_der) d_err = 4000;
    }
    if (fp_eph) {
        /* i = read_RNXephemeris(fp_eph, eph);
           if (i == 0) {
               ephem = 1;
               almanac = 0;
           }
           fclose(fp_eph); */
        ephs = read_RNXpephs(fp_eph);
        if (ephs) {
            ephem = 1;
            almanac = 0;
        }
        fclose(fp_eph);
        if (!option_der) d_err = 1000;
    }


    if (!rawin) {

        i = read_wav_header(fp);
        if (i) {
            fclose(fp);
            return -1;
        }

        while (!read_bits_fsk(fp, &bit, &len)) {

            if (len == 0) { // reset_frame();
                if (byte_count > pos_SondeID+8) {
                    if (byte_count < FRAME_LEN-20) err_gps = 1;
                    print_frame(byte_count);
                    err_gps = 0;
                }
                bit_count = 0;
                byte_count = FRAMESTART;
                header_found = 0;
                inc_bufpos();
                buf[bufpos] = 'x';
                continue;   // ...
            }

            for (i = 0; i < len; i++) {

                inc_bufpos();
                buf[bufpos] = 0x30 + bit;  // Ascii

                if (!header_found) {
                    if (compare() >= HEADLEN) header_found = 1;
                }
                else {
                    bitbuf[bit_count] = bit;
                    bit_count++;

                    if (bit_count == BITS) {
                        bit_count = 0;
                        byte = bits2byte(bitbuf);
                        frame[byte_count] = byte;
                        byte_count++;
                        if (byte_count == FRAME_LEN) {
                            byte_count = FRAMESTART;
                            header_found = 0;
                            //inc_bufpos();
                            //buf[bufpos] = 'x';
                            print_frame(FRAME_LEN);
                        }
                    }
                }
            }
            if (header_found && option_b) {
                bitstart = 1;

                while ( byte_count < FRAME_LEN ) {
                    if (read_rawbit(fp, &bit) == EOF) break;
                    bitbuf[bit_count] = bit;
                    bit_count++;
                    if (bit_count == BITS) {
                        bit_count = 0;
                        byte = bits2byte(bitbuf);
                        frame[byte_count] = byte;
                        byte_count++;
                    }
                }
                header_found = 0;
                print_frame(byte_count);
                byte_count = FRAMESTART;

            }
        }

    }
    else //if (rawin)
    {
        if (rawin == 3) frameofs = 5;

        while (1 > 0) {

            pbuf = fgets(buffer_rawin, rawin*FRAME_LEN+4, fp);
            if (pbuf == NULL) break;
            buffer_rawin[rawin*FRAME_LEN+1] = '\0';
            len = strlen(buffer_rawin) / rawin;
            if (len > pos_SondeID+8) {
                for (i = 0; i < len-frameofs; i++) { //%2x  SCNx8=%hhx(inttypes.h)
                    sscanf(buffer_rawin+rawin*i, "%2hhx", frame+frameofs+i);
                    // wenn ohne %hhx: sscanf(buffer_rawin+rawin*i, "%2x", &byte); frame[frameofs+i] = (uint8_t)byte;
                }
                if (len < FRAME_LEN-20) err_gps = 1;
                print_frame(len);
                err_gps = 0;
            }
        }
    }

    free(ephs);
    fclose(fp);

    return 0;
}

#endif
